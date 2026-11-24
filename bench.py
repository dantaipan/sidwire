#!/usr/bin/env python3
"""Phase 2 hardware benchmark for HttpTransport + Flusher.

  .venv/bin/python bench.py [--host IP] [--label wifi] [--skip-sound]
"""
from __future__ import annotations

import argparse
import json
import math
import time
import tomllib
from pathlib import Path

import sid
from probe import stats_ms
from transport import Flusher, HttpTransport


def timed_writes(t: HttpTransport, n: int, gap: float) -> dict:
    t.latencies.clear()
    t.write(0xD41B, b"\x00")  # warm-up; $D41B (OSC3) is read-only -> harmless
    t.latencies.clear()
    for _ in range(n):
        if gap:
            time.sleep(gap)
        t.write(0xD41B, b"\x00")
    return stats_ms(list(t.latencies))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host"); ap.add_argument("--label", default="run")
    ap.add_argument("-n", type=int, default=200)
    ap.add_argument("--skip-sound", action="store_true")
    a = ap.parse_args()
    from engine import load_config
    cfg = load_config("config.toml")["device"]
    host, pw = a.host or cfg["host"], cfg.get("password", "")
    clock = sid.NTSC_CLOCK if cfg.get("video", "PAL").upper() == "NTSC" else sid.PAL_CLOCK
    res: dict = {}

    print("== 1. per-write latency (ms)")
    for pre in (False, True):
        for gap in (0.0, 0.02):
            t = HttpTransport(host, password=pw, preconnect=pre)
            key = f"preconnect={pre} gap={int(gap*1000)}ms"
            res[key] = timed_writes(t, a.n, gap)
            res[key]["spare_hits"], res[key]["spare_misses"], res[key]["errors"] = t.spare_hits, t.spare_misses, t.errors
            t.close()
            print(f"   {key:28s} {res[key]}")

    print("== 2. how long does an idle pre-opened socket survive?")
    res["idle"] = {}
    for idle in (0.5, 1, 2, 4, 8):
        t = HttpTransport(host, password=pw, preconnect=True, max_idle=60)
        time.sleep(idle)
        t0 = time.perf_counter()
        try:
            t.write(0xD41B, b"\x00"); ok = True
        except Exception as e:  # noqa: BLE001
            ok = f"fail: {e}"
        ms = (time.perf_counter() - t0) * 1000
        res["idle"][str(idle)] = {"ms": round(ms, 2), "hit": t.spare_hits, "retries": t.errors, "ok": ok}
        print(f"   idle {idle:>4}s -> {ms:7.2f} ms  spare_hit={t.spare_hits} retries={t.errors} ok={ok}")
        t.close()

    if not a.skip_sound:
        print("== 3. filter sweep through Flusher (saw, low-pass, 4 s) — listen")
        t = HttpTransport(host, password=pw)
        st = sid.SidState(0xD400, clock)
        fl = Flusher(t, [st], interval_ms=5).start()
        with fl.edit():
            st.mark_all_dirty()
            st.set_volume(15); st.set_filter_mode(sid.LP); st.set_resonance(12)
            st.set_filter_route(0, True); st.set_cutoff(0)
            st.set_waveform(0, sid.SAW); st.set_adsr(0, a=0, d=0, s=15, r=6)
            st.set_note(0, 45)  # A2
        fl.gate(0, 0, True)
        changes, t0 = 0, time.monotonic()
        while (el := time.monotonic() - t0) < 4.0:
            with fl.edit():
                st.set_cutoff(int(1023.5 + 1023.5 * math.sin(el * math.pi)))  # one full sweep
            changes += 1
            time.sleep(0.001)
        fl.gate(0, 0, False)
        time.sleep(0.6)
        fl.stop()
        lat = stats_ms(list(t.latencies))
        res["sweep"] = {"changes": changes, "writes": fl.writes_sent, "latency": lat}
        print(f"   {changes} cutoff changes -> {fl.writes_sent} writes ({fl.writes_sent/4:.0f}/s). latency {lat}")

        print("== 4. chords: C-F-G-C, 3 voices, retrigger test — listen")
        t = HttpTransport(host, password=pw)
        st = sid.SidState(0xD400, clock)
        fl = Flusher(t, [st], interval_ms=5).start()
        with fl.edit():
            st.mark_all_dirty()
            st.set_volume(15)
            for v in range(3):
                st.set_waveform(v, sid.PULSE); st.set_pulse_width(v, 0x600)
                st.set_adsr(v, a=1, d=8, s=8, r=5)
        for chord in ([60, 64, 67], [60, 65, 69], [59, 62, 67], [60, 64, 67]):
            with fl.edit():
                for v, n in enumerate(chord):
                    st.set_note(v, n)
            for v in range(3):
                fl.gate(0, v, True)           # hit 1
            time.sleep(0.3)
            for v in range(3):                # hit 2: off+on inside one flush window
                fl.gate(0, v, False)
                fl.gate(0, v, True)
            time.sleep(0.3)
            for v in range(3):
                fl.gate(0, v, False)
            time.sleep(0.15)
        time.sleep(0.6)
        fl.stop()
        res["chords"] = {"writes": fl.writes_sent, "batches": fl.batches_sent}
        print(f"   done: {fl.writes_sent} writes in {fl.batches_sent} batches")

    out = Path(f"bench_phase2_{a.label}_{time.strftime('%Y%m%d-%H%M%S')}.json")
    out.write_text(json.dumps(res, indent=2))
    print("saved", out)


if __name__ == "__main__":
    main()
