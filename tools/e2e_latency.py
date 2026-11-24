#!/usr/bin/env python3
"""End-to-end latency: MIDI note-on sent -> SID sound arriving in the C64U audio stream.

Drives a target through a virtual MIDI port, records the C64U audio with packet arrival
times and finds each note's onset. The audio-stream delay is the same for every target,
so differences between targets are real; absolute numbers include the stream's buffering.

  .venv/bin/python tools/e2e_latency.py python      # bridge.py
  .venv/bin/python tools/e2e_latency.py plugin      # SID Bridge standalone app
  .venv/bin/python tools/e2e_latency.py none        # just play into whatever already listens
Extra checks with --checks: 6-voice chord pitches, encoder sweep, preset change.
"""
from __future__ import annotations

import argparse
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

import mido
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import listen  # noqa: E402

HOST = os.environ.get("MSID_HW_HOST") or listen.load_host()   # the C64U: env, or config(.local).toml
APP = ROOT / "cpp/build/release/plugin/SidBridge_artefacts/Release/Standalone/SID Bridge.app/Contents/MacOS/SID Bridge"
NOTE_PORT, ENC_PORT = "MSID Test Notes", "MSID Test Knobs"


def start_target(kind: str, notes_name: str, enc_name: str, dma: bool = False):
    if kind == "python":
        cfg = ROOT / "captures" / "e2e_config.toml"
        text = (ROOT / "config.toml").read_text().replace('notes_port = ""', f'notes_port = "{notes_name}"')
        text = text.replace('controls_port = ""', f'controls_port = "{enc_name}"')
        text = text.replace('host = ""', f'host = "{HOST}"', 1)
        text = text.replace('presets = "presets.toml"', f'presets = "{ROOT / "presets.toml"}"')
        cfg.write_text(text)
        flags = ["--dma" if dma else "--http"]
        return subprocess.Popen([sys.executable, "bridge.py", "--config", str(cfg)] + flags, cwd=ROOT,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if kind == "plugin":
        env = dict(os.environ, MSID_HOST=HOST, MSID_NOTE_PORT=notes_name, MSID_ENCODER_PORT=enc_name,
                   MSID_TRANSPORT="dma" if dma else "http")
        return subprocess.Popen([str(APP)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return None


def onset_times(audio: np.ndarray, rec: listen.Recorder, sends: list[float], window: float) -> list[float]:
    """For each send time, ms until the first loud 1 ms block after it (None = no onset found)."""
    x = np.abs(listen.mono(audio))
    arr = np.array(rec.arrivals)
    # map frame index -> arrival time (linear fit over packets: smooths per-packet jitter)
    k, b = np.polyfit(arr[:, 1], arr[:, 0], 1)
    t_of = lambda frame: k * frame + b  # noqa: E731
    frame_of = lambda t: int((t - b) / k)  # noqa: E731
    blk = int(listen.RATE / 1000)
    lat = []
    for ts in sends:
        f0, f1 = max(0, frame_of(ts) - 5 * blk), min(len(x), frame_of(ts + window))
        seg = x[f0:f1]
        if len(seg) < blk:
            lat.append(None)
            continue
        env = seg[: len(seg) // blk * blk].reshape(-1, blk).max(axis=1)
        noise = np.median(env[:3]) if len(env) > 3 else 0
        hits = np.nonzero(env > max(0.02, noise * 8))[0]
        if len(hits):
            lat.append((t_of(f0 + hits[0] * blk) - ts) * 1000)
        else:
            lat.append(None)
    return lat


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("target", choices=["python", "plugin", "none"])
    ap.add_argument("-n", type=int, default=30)
    ap.add_argument("--checks", action="store_true")
    ap.add_argument("--dma", action="store_true", help="use the DMA socket transport (default: HTTP)")
    a = ap.parse_args()

    notes = mido.open_output(NOTE_PORT, virtual=True)
    knobs = mido.open_output(ENC_PORT, virtual=True)
    proc = start_target(a.target, NOTE_PORT, ENC_PORT, a.dma)
    time.sleep(4.0)   # let the target connect and init the SIDs
    if proc and proc.poll() is not None:
        print(proc.stdout.read()); sys.exit("target exited")

    rec = listen.Recorder(HOST).start()
    time.sleep(0.5)
    for ch in range(2):  # a known patch: square, fast attack, full sustain
        notes.send(mido.Message("program_change", program=0, channel=ch))
    time.sleep(0.3)
    sends = []
    for i in range(a.n):
        t = time.perf_counter()
        notes.send(mido.Message("note_on", note=60 + (i % 5) * 2, velocity=100))
        sends.append(t)
        time.sleep(0.12)
        notes.send(mido.Message("note_off", note=60 + (i % 5) * 2))
        time.sleep(0.38)
    result = {}
    if a.checks:
        chord = [48, 52, 55, 72, 76, 79]
        for n in chord:
            notes.send(mido.Message("note_on", note=n, velocity=100))
        time.sleep(1.2)
        t_chord = time.perf_counter()
        for _ in range(100):                       # K1 cutoff down
            knobs.send(mido.Message("control_change", control=16, value=65)); time.sleep(0.01)
        t_sweep = time.perf_counter()
        for n in chord:
            notes.send(mido.Message("note_off", note=n))
        time.sleep(0.6)
        notes.send(mido.Message("program_change", program=6))   # Noise Drum
        time.sleep(0.2)
        t_noise = time.perf_counter()
        notes.send(mido.Message("note_on", note=60, velocity=100)); time.sleep(0.4)
        notes.send(mido.Message("note_off", note=60)); time.sleep(0.4)
        notes.send(mido.Message("control_change", control=74, value=127))
        notes.send(mido.Message("program_change", program=0))
        result.update(t_chord=t_chord, t_sweep=t_sweep, t_noise=t_noise)
    notes.send(mido.Message("control_change", control=123, value=0))   # panic
    time.sleep(0.5)
    audio = rec.stop()
    tag = a.target + ("_dma" if a.dma else "")
    listen.save_wav(ROOT / "captures" / f"e2e_{tag}.wav", audio)

    per_note = onset_times(audio, rec, sends, 0.2)
    lat = [v for v in per_note if v is not None]
    print(f"== {tag}: {len(lat)}/{len(sends)} onsets found, lost packets {rec.lost}")
    odd = [(i, v) for i, v in enumerate(per_note) if v is None or abs(v - statistics.median(lat)) > 5] if lat else []
    if odd:
        print("   outliers (note #, ms):", ", ".join(f"#{i} {'miss' if v is None else f'{v:.1f}'}" for i, v in odd))
    if lat:
        s = sorted(lat)
        print(f"   end-to-end ms: min {s[0]:.1f}  median {statistics.median(s):.1f}  "
              f"p95 {s[int(0.95 * len(s)) - 1]:.1f}  max {s[-1]:.1f}  stdev {statistics.pstdev(s):.1f}")

    if a.checks:
        arr = np.array(rec.arrivals); k, b = np.polyfit(arr[:, 1], arr[:, 0], 1)
        fr = lambda t: int((t - b) / k)  # noqa: E731
        st = audio.astype(float)
        seg = st[fr(result["t_chord"]) - int(0.8 * listen.RATE): fr(result["t_chord"])]
        L, R = seg[:, 0] - seg[:, 0].mean(), seg[:, 1] - seg[:, 1].mean()

        def peaks(x):
            sp = np.abs(np.fft.rfft(x * np.hanning(len(x)))); f = np.fft.rfftfreq(len(x), 1 / listen.RATE)
            sp[(f < 100) | (f > 900)] = 0; out = []
            for _ in range(3):
                i = int(np.argmax(sp)); out.append(round(float(f[i]))); sp[max(0, i - 12): i + 12] = 0
            return sorted(out)
        print(f"   chord L peaks {peaks(L)} (expect ~131 165 196)   R peaks {peaks(R)}")
        x = listen.mono(audio)
        c0 = listen.centroid(x[fr(result['t_chord']) - 4800: fr(result['t_chord'])], listen.RATE)
        c1 = listen.centroid(x[fr(result['t_sweep']) - 4800: fr(result['t_sweep'])], listen.RATE)
        print(f"   encoder sweep: centroid {c0:.0f} Hz -> {c1:.0f} Hz (expect a clear drop)")
        nseg = x[fr(result["t_noise"]) + 4800: fr(result["t_noise"]) + 14400]
        print(f"   preset 6 (Noise Drum): centroid {listen.centroid(nseg, listen.RATE):.0f} Hz, "
              f"f0 {listen.fundamental(nseg, listen.RATE):.0f} (noise: high centroid, no stable f0)")

    if proc:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill(); out, _ = proc.communicate()
        tail = [l for l in (out or "").splitlines() if l.strip()][-5:]
        if tail:
            print("   target output tail:", *tail, sep="\n     ")
    notes.close(); knobs.close()


if __name__ == "__main__":
    main()
