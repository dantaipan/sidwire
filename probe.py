#!/usr/bin/env python3
"""Phase 0: prove the network write path to the C64 Ultimate.

Steps (each can be skipped):
  1. GET /v1/version and /v1/info
  2. List SID-related config categories (--configs)
  3. Border-colour test: write $D020 via PUT, read it back, then try the POST
     (binary body) variant on $D021. Restores the original colours afterwards.
  4. Beep: triangle A4 on voice 1 for 500 ms.
  5. Latency benchmark: N single-byte PUT writes, plus 25-byte PUT and POST writes.

Usage:
  .venv/bin/python probe.py                 # everything, host from config.toml
  .venv/bin/python probe.py --host 10.0.0.5 --bench 200
  .venv/bin/python probe.py --skip-border --skip-beep
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import tomllib
from pathlib import Path

import requests

PAL_CLOCK = 985248
NTSC_CLOCK = 1022727


class Ultimate:
    def __init__(self, host: str, password: str = "", timeout: float = 3.0):
        self.base = f"http://{host}"
        self.s = requests.Session()
        if password:
            self.s.headers["X-Password"] = password
        self.timeout = timeout

    def _check(self, r: requests.Response) -> requests.Response:
        if r.status_code == 403:
            sys.exit("403 Forbidden: wrong or missing network password (X-Password).")
        if r.status_code >= 400:
            raise RuntimeError(f"{r.request.method} {r.url} -> {r.status_code}: {r.text[:300]}")
        ctype = r.headers.get("Content-Type", "")
        if "json" in ctype:
            errs = r.json().get("errors") or []
            if errs:
                raise RuntimeError(f"{r.request.method} {r.url} -> API errors: {errs}")
        return r

    def get_json(self, path: str) -> dict:
        return self._check(self.s.get(self.base + path, timeout=self.timeout)).json()

    def writemem(self, address: int, data: bytes) -> requests.Response:
        """PUT variant: hex string in query, max 128 bytes."""
        return self._check(self.s.put(
            f"{self.base}/v1/machine:writemem",
            params={"address": f"{address:04X}", "data": data.hex().upper()},
            timeout=self.timeout))

    def writemem_post(self, address: int, data: bytes) -> requests.Response:
        """POST variant: binary body."""
        return self._check(self.s.post(
            f"{self.base}/v1/machine:writemem",
            params={"address": f"{address:04X}"},
            data=data,
            headers={"Content-Type": "application/octet-stream"},
            timeout=self.timeout))

    def readmem(self, address: int, length: int = 1) -> bytes:
        r = self._check(self.s.get(
            f"{self.base}/v1/machine:readmem",
            params={"address": f"{address:04X}", "length": str(length)},
            timeout=self.timeout))
        return r.content


def stats_ms(samples: list[float]) -> dict:
    ms = sorted(x * 1000 for x in samples)
    p95 = ms[max(0, int(round(0.95 * len(ms))) - 1)]
    return {
        "n": len(ms),
        "min": round(ms[0], 2),
        "mean": round(statistics.mean(ms), 2),
        "median": round(statistics.median(ms), 2),
        "p95": round(p95, 2),
        "max": round(ms[-1], 2),
        "stdev": round(statistics.pstdev(ms), 2),
    }


def bench(fn, n: int) -> dict:
    fn()  # warm-up (opens keep-alive connection)
    samples = []
    for _ in range(n):
        t0 = time.perf_counter()
        fn()
        samples.append(time.perf_counter() - t0)
    return stats_ms(samples)


def sid_freq(hz: float, clock: int) -> int:
    return max(0, min(65535, round(hz * 16777216 / clock)))


def step_info(u: Ultimate) -> None:
    print("== /v1/version"); print(json.dumps(u.get_json("/v1/version"), indent=2))
    print("== /v1/info");    print(json.dumps(u.get_json("/v1/info"), indent=2))


def step_configs(u: Ultimate) -> None:
    cats = u.get_json("/v1/configs").get("categories", [])
    print("== config categories:", cats)
    for c in cats:
        if "sid" in c.lower() or "audio" in c.lower():
            path = "/v1/configs/" + requests.utils.quote(c)
            print(f"== {c}")
            try:
                print(json.dumps(u.get_json(path), indent=2))
            except Exception as e:  # noqa: BLE001
                print("   failed:", e)


def step_border(u: Ultimate) -> None:
    print("== Border test")
    try:
        orig = u.readmem(0xD020, 2)
        print(f"   readmem $D020/$D021 before: {orig.hex(' ').upper()}")
    except Exception as e:  # noqa: BLE001
        print("   readmem failed:", e)
        orig = bytes([0x0E, 0x06])  # BASIC default light blue / blue

    u.writemem(0xD020, b"\x02")
    try:
        print(f"   readmem $D020 after PUT red: {u.readmem(0xD020, 1).hex().upper()}"
              "  (expect x2, upper nibble may read as F)")
    except Exception as e:  # noqa: BLE001
        print("   readback failed:", e)
    print("   >>> Border should be RED now.")
    time.sleep(2)

    try:
        u.writemem_post(0xD021, b"\x05")
        print(f"   POST raw body accepted; $D021 now {u.readmem(0xD021, 1).hex().upper()}"
              "  >>> background should be GREEN")
    except Exception as e:  # noqa: BLE001
        print("   POST raw body failed:", e)
    time.sleep(2)

    u.writemem(0xD020, bytes([orig[0] & 0x0F, orig[1] & 0x0F]))
    print("   colours restored")


def step_beep(u: Ultimate, clock: int) -> None:
    print("== Beep (A4 triangle, 500 ms)")
    f = sid_freq(440.0, clock)
    u.writemem(0xD400, bytes(25))                    # clear all SID registers
    u.writemem(0xD418, b"\x0F")                      # volume max, no filter
    u.writemem(0xD405, b"\x09\x00")                  # AD=09, SR=00
    u.writemem(0xD400, bytes([f & 0xFF, f >> 8]))    # freq lo/hi
    u.writemem(0xD404, b"\x11")                      # triangle + gate
    time.sleep(0.5)
    u.writemem(0xD404, b"\x10")                      # gate off
    print(f"   freq word ${f:04X} ({f}).  >>> Did you hear a beep?")
    time.sleep(0.5)


def step_bench(u: Ultimate, n: int) -> dict:
    print(f"== Latency benchmark ({n} writes each)")
    results = {}
    # Rewrite the current border colour: a harmless, visible-if-wrong target.
    border = u.readmem(0xD020, 1)[0] & 0x0F
    results["put_1byte"] = bench(lambda: u.writemem(0xD020, bytes([border])), n)
    print("   PUT 1 byte :", results["put_1byte"])
    # 25-byte range into SID with volume 0 => silent
    silent = bytes(25)
    results["put_25byte_sid"] = bench(lambda: u.writemem(0xD400, silent), n)
    print("   PUT 25 byte:", results["put_25byte_sid"])
    try:
        results["post_25byte_sid"] = bench(lambda: u.writemem_post(0xD400, silent), n)
        print("   POST 25 byte:", results["post_25byte_sid"])
    except Exception as e:  # noqa: BLE001
        print("   POST bench failed:", e)
    return results


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="config.toml")
    ap.add_argument("--host"); ap.add_argument("--password")
    ap.add_argument("--video", choices=["PAL", "NTSC"])
    ap.add_argument("--bench", type=int, default=200, help="writes per benchmark (0 = skip)")
    ap.add_argument("--configs", action="store_true", help="dump SID/audio config categories")
    ap.add_argument("--skip-border", action="store_true")
    ap.add_argument("--skip-beep", action="store_true")
    ap.add_argument("--label", default="", help="tag for results file, e.g. ethernet / wifi")
    a = ap.parse_args()

    from engine import load_config
    cfg = load_config(a.config)["device"]
    host = a.host or cfg.get("host")
    if not host:
        sys.exit("No host: set [device].host in config.toml or pass --host")
    password = a.password if a.password is not None else cfg.get("password", "")
    video = (a.video or cfg.get("video", "PAL")).upper()
    clock = NTSC_CLOCK if video == "NTSC" else PAL_CLOCK

    u = Ultimate(host, password)
    print(f"Target http://{host}  ({video}, clock {clock} Hz)")
    step_info(u)
    if a.configs:
        step_configs(u)
    if not a.skip_border:
        step_border(u)
    if not a.skip_beep:
        step_beep(u, clock)
    if a.bench > 0:
        res = step_bench(u, a.bench)
        out = Path(f"bench_{a.label or 'run'}_{time.strftime('%Y%m%d-%H%M%S')}.json")
        out.write_text(json.dumps({"host": host, "label": a.label, "results": res}, indent=2))
        print(f"   saved {out}")


if __name__ == "__main__":
    main()
