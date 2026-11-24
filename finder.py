"""Find C64 Ultimates on the local network, the way PS64 does (src/discover.c there).

Try port 80 on every address of the local /24 (plus the /24 of a known address), 64 at a time
with a short connect window, then ask each host that answered for GET /v1/info and keep the
ones whose product names an Ultimate. A 403 means a network password is set: listed as locked.
The plugin has the same search in C++ (cpp/core/src/Finder.cpp).

  .venv/bin/python bridge.py --find
"""
from __future__ import annotations

import ipaddress
import json
import socket
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

import requests


@dataclass
class FoundDevice:
    ip: str
    hostname: str = ""
    product: str = ""
    firmware: str = ""
    locked: bool = False

    def describe(self) -> str:
        if self.locked:
            return f"{self.ip}  (password protected)"
        return f"{self.ip}  {self.hostname or '?'}  {self.product}, firmware {self.firmware or '?'}"


def local_address() -> str:
    """The address this machine uses on its LAN ('' if none). Sends nothing."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        return "" if ip == "0.0.0.0" else ip
    except OSError:
        return ""
    finally:
        s.close()


def _net24(ip: str) -> list[str]:
    try:
        net = ipaddress.ip_network(f"{ip}/24", strict=False)
    except ValueError:
        return []
    return [str(h) for h in net.hosts()]


def parse_info(body: str) -> FoundDevice | None:
    try:
        d = json.loads(body)
    except ValueError:
        return None
    product = str(d.get("product", ""))
    if "Ultimate" not in product:
        return None
    return FoundDevice("", str(d.get("hostname", "")), product, str(d.get("firmware_version", "")))


def _port_open(ip: str, port: int, timeout: float) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def find_ultimates(hosts: list[str] | None = None, also_near: str = "", password: str = "",
                   port: int = 80, timeout: float = 0.6, progress=None) -> list[FoundDevice]:
    if hosts is None:
        hosts = _net24(local_address()) if local_address() else []
        if also_near:
            hosts += [h for h in _net24(also_near) if h not in hosts]
    headers = {"X-Password": password} if password else {}
    found: list[FoundDevice] = []
    with ThreadPoolExecutor(max_workers=64) as pool:
        for first in range(0, len(hosts), 64):
            batch = hosts[first:first + 64]
            for ip, is_open in zip(batch, pool.map(lambda h: _port_open(h, port, timeout), batch)):
                if not is_open:
                    continue
                try:
                    r = requests.get(f"http://{ip}:{port}/v1/info", headers=headers, timeout=2)
                except requests.RequestException:
                    continue
                if r.status_code == 403:
                    found.append(FoundDevice(ip, product="Password protected", locked=True))
                elif r.status_code == 200 and (d := parse_info(r.text)):
                    d.ip = ip
                    found.append(d)
            if progress:
                progress(min(len(hosts), first + 64) * 100 // max(1, len(hosts)))
    return found
