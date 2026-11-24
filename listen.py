#!/usr/bin/env python3
"""Capture and analyse the C64U audio stream, so tests can be checked by ear-less means.

The Ultimate sends 16-bit stereo LE at ~47983 Hz (PAL), 192 frames per UDP packet,
each packet prefixed by a u16 LE sequence number (format from the PS64 project,
src/audiostream.h). Started with PUT /v1/streams/audio:start?ip=<me>:<port>.

Uses port 11002 by default so it doesn't collide with PS64 (11001) — but the C64U
has only one audio-stream destination, so starting a capture redirects the
stream away from PS64 while it runs.

  .venv/bin/python listen.py --seconds 5 --out capture.wav     # record + report
  .venv/bin/python listen.py --analyze capture.wav             # report on a file
"""
from __future__ import annotations

import argparse
import socket
import struct
import threading
import time
import tomllib
import wave
from pathlib import Path

import numpy as np
import requests

RATE = 47983
PORT = 11002


def load_device(config: str | Path = "config.toml") -> dict:
    """[device] from config.toml plus config.local.toml (see engine.load_config)."""
    from engine import load_config
    return load_config(config)["device"]


def load_host(config: str | Path = "config.toml") -> str:
    host = load_device(config).get("host", "")
    if not host:
        raise SystemExit('No C64U address: put [device] host = "..." in config.local.toml (bridge.py --find shows it)')
    return host


def local_ip_for(host: str) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 80))
        return s.getsockname()[0]
    finally:
        s.close()


class Recorder:
    """Starts the C64U audio stream towards us and collects samples in a thread."""

    def __init__(self, host: str, port: int = PORT, password: str = ""):
        self.host, self.port = host, port
        self.headers = {"X-Password": password} if password else {}
        self.chunks: list[bytes] = []
        self.lost = 0
        self.packets = 0
        self.arrivals: list[tuple[float, int]] = []   # (perf_counter at arrival, frame index of packet start)
        self._frames = 0
        self._stop = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.sock.bind(("0.0.0.0", port))
        self.sock.settimeout(0.2)
        self._t = threading.Thread(target=self._run, daemon=True)

    def _api(self, path: str) -> None:
        r = requests.put(f"http://{self.host}{path}", headers=self.headers, timeout=3)
        r.raise_for_status()

    def start(self) -> "Recorder":
        self._t.start()
        me = local_ip_for(self.host)
        self._api(f"/v1/streams/audio:start?ip={me}:{self.port}")
        deadline = time.time() + 2
        while not self.packets and time.time() < deadline:
            time.sleep(0.01)
        if not self.packets:
            raise RuntimeError("no audio packets received — firewall, or stream not started?")
        self.chunks.clear()  # discard warm-up
        self.arrivals.clear()
        self._frames = 0
        return self

    def _run(self) -> None:
        seq = None
        while not self._stop.is_set():
            try:
                pkt = self.sock.recv(2048)
            except socket.timeout:
                continue
            if len(pkt) < 6 or (len(pkt) - 2) % 4:
                continue
            s = struct.unpack_from("<H", pkt)[0]
            if seq is not None and s != seq:
                self.lost += (s - seq) & 0xFFFF
            seq = (s + 1) & 0xFFFF
            self.packets += 1
            self.arrivals.append((time.perf_counter(), self._frames))
            self._frames += (len(pkt) - 2) // 4
            self.chunks.append(pkt[2:])

    def stop(self, stop_stream: bool = True) -> np.ndarray:
        if stop_stream:
            try:
                self._api("/v1/streams/audio:stop")
            except Exception:  # noqa: BLE001
                pass
        self._stop.set()
        self._t.join(timeout=1)
        self.sock.close()
        return np.frombuffer(b"".join(self.chunks), dtype="<i2").reshape(-1, 2)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.audio = self.stop()


def save_wav(path: str | Path, stereo: np.ndarray, rate: int = RATE) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(stereo.astype("<i2").tobytes())


def load_wav(path: str | Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, w.getnchannels())
        return data, w.getframerate()


# ---------------------------------------------------------------- analysis

def mono(stereo: np.ndarray) -> np.ndarray:
    x = stereo.astype(np.float64).mean(axis=1) / 32768.0
    return x - x.mean()


def frames(x: np.ndarray, rate: int, hop_s: float = 0.05, win_s: float = 0.1):
    hop, win = int(rate * hop_s), int(rate * win_s)
    for i in range(0, max(1, len(x) - win), hop):
        yield i / rate, x[i:i + win]


def fundamental(seg: np.ndarray, rate: int, fmin: float = 40, fmax: float = 2000) -> float:
    """Autocorrelation pitch estimate (Hz), 0 if unvoiced."""
    seg = seg * np.hanning(len(seg))
    n = 1 << (2 * len(seg) - 1).bit_length()
    spec = np.fft.rfft(seg, n)
    ac = np.fft.irfft(spec * np.conj(spec))[:len(seg)]
    if ac[0] <= 0:
        return 0.0
    lo, hi = int(rate / fmax), min(len(ac) - 1, int(rate / fmin))
    lag = lo + int(np.argmax(ac[lo:hi]))
    if ac[lag] / ac[0] < 0.3:
        return 0.0
    # parabolic interpolation
    a, b, c = ac[lag - 1], ac[lag], ac[lag + 1]
    denom = a - 2 * b + c
    shift = 0.5 * (a - c) / denom if denom else 0.0
    if not -1.0 < shift < 1.0:
        shift = 0.0
    return rate / (lag + shift)


def spectrum_peaks(seg: np.ndarray, rate: int, n: int = 4, fmin: float = 40) -> list[float]:
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    freqs = np.fft.rfftfreq(len(seg), 1 / rate)
    spec[freqs < fmin] = 0
    peaks = []
    for _ in range(n):
        k = int(np.argmax(spec))
        if spec[k] <= 0:
            break
        peaks.append(float(freqs[k]))
        lo, hi = max(0, k - 8), k + 8   # suppress neighbourhood
        spec[lo:hi] = 0
    return sorted(peaks)


def centroid(seg: np.ndarray, rate: int) -> float:
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    freqs = np.fft.rfftfreq(len(seg), 1 / rate)
    return float((spec * freqs).sum() / spec.sum()) if spec.sum() else 0.0


def note_name(hz: float) -> str:
    if hz <= 0:
        return "-"
    n = 69 + 12 * np.log2(hz / 440.0)
    names = "C C# D D# E F F# G G# A A# B".split()
    r = int(round(n))
    return f"{names[r % 12]}{r // 12 - 1}{(n - r) * 100:+.0f}c"


def report(stereo: np.ndarray, rate: int = RATE, hop_s: float = 0.05, peaks: int = 0) -> list[dict]:
    x = mono(stereo)
    rows = []
    for t, seg in frames(x, rate, hop_s):
        rms = float(np.sqrt(np.mean(seg ** 2)))
        db = 20 * np.log10(rms) if rms > 1e-6 else -120.0
        row = {"t": round(t, 3), "db": round(db, 1)}
        if db > -50:
            f0 = fundamental(seg, rate)
            row.update(f0=round(f0, 1), note=note_name(f0), centroid=round(centroid(seg, rate)))
            if peaks:
                row["peaks"] = [round(p) for p in spectrum_peaks(seg, rate, peaks)]
        rows.append(row)
    return rows


def print_report(rows: list[dict]) -> None:
    for r in rows:
        bar = "#" * max(0, int((r["db"] + 60) / 2))
        extra = ""
        if "f0" in r:
            extra = f" f0 {r['f0']:7.1f} {r['note']:>9}  centroid {r['centroid']:5d}"
            if "peaks" in r:
                extra += f"  peaks {r['peaks']}"
        print(f"{r['t']:6.2f}s {r['db']:6.1f} dB {bar:<30}{extra}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host"); ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--out", default="capture.wav")
    ap.add_argument("--analyze", help="analyse an existing WAV instead of recording")
    ap.add_argument("--hop", type=float, default=0.05)
    ap.add_argument("--peaks", type=int, default=0)
    a = ap.parse_args()
    if a.analyze:
        data, rate = load_wav(a.analyze)
    else:
        dev = load_device()
        rec = Recorder(a.host or dev["host"], a.port, dev.get("password", "")).start()
        print(f"recording {a.seconds}s ...")
        time.sleep(a.seconds)
        data, rate = rec.stop(), RATE
        save_wav(a.out, data)
        print(f"saved {a.out}: {len(data)/rate:.2f}s, {rec.packets} packets, {rec.lost} lost")
    print_report(report(data, rate, a.hop, a.peaks))


if __name__ == "__main__":
    main()
