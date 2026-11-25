#!/usr/bin/env python3
"""MIDI -> C64 Ultimate SID bridge.

  .venv/bin/python bridge.py --list-ports
  .venv/bin/python bridge.py --monitor              # print incoming MIDI, no SID output
  .venv/bin/python bridge.py --sids                 # show which SIDs the C64U has mapped
  .venv/bin/python bridge.py [--ui] [--split] [--preset NAME] [--port NAME] [--dry-run] [-v]
  .venv/bin/python bridge.py --http                 # REST API instead of the DMA service
  .venv/bin/python bridge.py --find                 # look for C64 Ultimates on the network
  .venv/bin/python bridge.py --list-profiles        # controller profiles (profiles/*.toml)
  .venv/bin/python bridge.py --profile NAME|PATH    # use another controller profile
"""
from __future__ import annotations

import argparse
import fcntl
import os
import logging
import signal
import sys
import time
from pathlib import Path

import requests

import sid
from engine import (DEFAULTS, Engine, Part, Slot, load_config, load_presets,  # noqa: F401 (re-exports)
                    PARAM_MAX)
from profiles import Profile, ProfileError, list_profiles, load_profile
from sid import SidState
from transport import DmaSocketTransport, DryRunTransport, Flusher, HttpTransport, TransportError

DMA_HINT = "enable C64U menu → Network Settings → Ultimate DMA Service"

log = logging.getLogger("bridge")
NOTE_NAMES = "C C# D D# E F F# G G# A A# B".split()


def note_name(n: int | None) -> str:
    return "--" if n is None else f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def find_port(want: str, names: list[str], required: bool = True) -> str | None:
    if want in names:
        return want
    matches = [n for n in names if want.lower() in n.lower()]
    if not matches:
        if not required:
            return None
        sys.exit(f"No MIDI input matching {want!r}. Available: {names}")
    return matches[0]


def profile_dirs(config_path: str) -> list[Path]:
    """Where profile names are looked up: next to the config file, then the repo's profiles/."""
    return [Path(config_path).resolve().parent / "profiles"]


def resolve_profile(cfg: dict, config_path: str, override: str | None) -> Profile:
    name = override or cfg["controller"].get("profile", "generic")
    p = Path(name).expanduser()
    if not p.is_absolute() and p.suffix == ".toml":
        p = Path(config_path).resolve().parent / p
        name = str(p) if p.exists() else name
    try:
        return load_profile(name, profile_dirs(config_path))
    except ProfileError as e:
        sys.exit(f"Controller profile: {e}")


# ---------------------------------------------------------------- SID detection

def detect_sids(host: str, password: str = "") -> dict[int, list[str]]:
    """Ask the C64U which SIDs answer at which address: {address: [source, ...]}."""
    h = {"X-Password": password} if password else {}

    def get(cat: str) -> dict:
        r = requests.get(f"http://{host}/v1/configs/{requests.utils.quote(cat)}", headers=h, timeout=3)
        r.raise_for_status()
        return r.json().get(cat, {})

    addr, sockets = get("SID Addressing"), get("SID Sockets Configuration")
    found: dict[int, list[str]] = {}
    for n in (1, 2):
        a = addr.get(f"SID Socket {n} Address", "Unmapped")
        if sockets.get(f"SID Socket {n}") == "Enabled" and a.startswith("$"):
            chip = sockets.get(f"SID Detected Socket {n}", "?")
            found.setdefault(int(a[1:], 16), []).append(f"socket {n} ({chip})")
        a = addr.get(f"UltiSID {n} Address", "Unmapped")
        if a.startswith("$"):
            found.setdefault(int(a[1:], 16), []).append(f"UltiSID {n}")
    return dict(sorted(found.items()))


def resolve_bases(cfg: dict, host: str | None, password: str, dry_run: bool) -> list[int]:
    bases = cfg["sid"]["bases"]
    found: dict[int, list[str]] | None = None
    if host and not dry_run:
        try:
            found = detect_sids(host, password)
        except Exception as e:  # noqa: BLE001
            log.warning("could not read SID config from the C64U: %s", e)
    if bases == "auto":
        if not found:
            sys.exit("[sid].bases = \"auto\" but the SID config could not be read")
        bases = list(found)
    bases = [int(b) for b in bases]
    if found is not None:
        for b in bases:
            if b not in found:
                log.warning("nothing is mapped at $%04X on the C64U — that SID will be silent", b)
            elif len(found[b]) > 1:
                log.info("$%04X drives %s together", b, " + ".join(found[b]))
    return bases


# ---------------------------------------------------------------- single instance

_lock_file = None


def lock_path_for(host: str) -> Path:
    """Shared with the JUCE plugin (cpp/plugin): one lock per C64U address."""
    return Path("/tmp") / f"midi-sid-bridge-{host.replace(':', '_').replace('/', '_')}.lock"


def acquire_instance_lock(path: Path) -> None:
    """Exit if another bridge (script or plugin) already drives this C64U — two would fight
    over the SID registers."""
    global _lock_file
    f = open(path, "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.seek(0)
        pid = f.read().strip() or "?"
        sys.exit(f"Another bridge is already driving this C64U (pid {pid}) — a bridge.py or the "
                 "SIDwire plugin. Quit it first: two bridges fight over the same SID voices.")
    f.seek(0); f.truncate(); f.write(str(os.getpid())); f.flush()
    _lock_file = f   # keep open: the lock lives as long as this process


# ---------------------------------------------------------------- transport

def open_transport(host: str, password: str, want: str):
    """The DMA service if wanted and reachable, else HTTP (with a warning)."""
    if want == "dma":
        # probe on its own connection with 2.5 s: the service serves one connection at a time
        # and drops an idle one only after 1 s
        probe = DmaSocketTransport(host, password=password, timeout=2.5)
        try:
            log.info("DMA service: %s", probe.identify().strip("* "))
            probe.close()
            return DmaSocketTransport(host, password=password)
        except (OSError, TransportError) as e:
            probe.close()
            log.warning("Ultimate DMA Service not reachable (%s) — using the HTTP API instead (%s)", e, DMA_HINT)
    return HttpTransport(host, password=password)


# ---------------------------------------------------------------- terminal UI

class StatusUI:
    """rich Live view: connection, latency, each part's patch and voices."""

    def __init__(self, engine: Engine, fl: Flusher, transport, port_names: list[str]):
        self.e, self.fl, self.t, self.ports = engine, fl, transport, port_names
        self._last = (time.monotonic(), 0)
        self.rate = 0.0

    def __rich__(self):
        from rich.table import Table
        from rich.text import Text
        from rich.console import Group

        now = time.monotonic()
        t0, w0 = self._last
        if now - t0 >= 1.0:
            self.rate = (self.fl.writes_sent - w0) / (now - t0)
            self._last = (now, self.fl.writes_sent)
        lat = ""
        if isinstance(self.t, HttpTransport) and self.t.latencies:
            recent = sorted(list(self.t.latencies)[-50:])
            lat = f"{recent[len(recent) // 2] * 1000:.1f} ms"
        conn = Text("● connected", "green") if self.fl.connected else Text(f"● RETRYING  {self.fl.last_error[:60]}", "bold red")
        via = "DMA" if isinstance(self.t, DmaSocketTransport) else "HTTP"
        head = Text.assemble(conn, f"   via {via}   writes/s {self.rate:4.0f}   latency {lat or '-'}   "
                             f"errors {self.fl.errors}   in: {', '.join(self.ports)}")
        parts = []
        for part in self.e.parts:
            p = part.patch
            sids = " ".join(f"${self.e.states[i].base:04X}" for i in part.sids)
            title = (f"Part {part.index + 1}  ch {'omni' if part.channel == 0 else part.channel}  "
                     f"SID {sids}  [b]{p.get('name', '')}[/b]  {p['waveform']}"
                     + ("  [red]MUTED[/red]" if part.muted else "")
                     + ("  [yellow]pedal[/yellow]" if part.pedal else ""))
            tb = Table(title=title, title_justify="left", show_edge=False, pad_edge=False, expand=False)
            for col in ("cutoff", "res", "PW", "A", "D", "S", "R", "vol", "filter", "bend"):
                tb.add_column(col, justify="right")
            tb.add_row(str(p["cutoff"]), str(p["resonance"]), str(p["pulse_width"]),
                       str(p["attack"]), str(p["decay"]), str(p["sustain"]), str(p["release"]),
                       str(p["volume"]), p["filter_mode"], f"{part.bend:+.2f}")
            voices = Text("voices  ")
            for s in part.slots:
                style = "bold green" if s.gated else "dim"
                voices.append(f" {self.e.states[s.sid].base:04X}.{s.voice + 1} {note_name(s.note):>4} ", style)
            parts += [tb, voices, Text("")]
        return Group(head, Text(""), *parts)


# ---------------------------------------------------------------- main

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="config.toml")
    ap.add_argument("--port", help="notes MIDI input (overrides the profile)"); ap.add_argument("--host")
    ap.add_argument("--profile", help="controller profile: name in profiles/ or a .toml path")
    ap.add_argument("--list-ports", action="store_true")
    ap.add_argument("--list-profiles", action="store_true", help="show the controller profiles and exit")
    ap.add_argument("--monitor", action="store_true", help="print incoming MIDI only")
    ap.add_argument("--sids", action="store_true", help="show the C64U's SID mapping and exit")
    ap.add_argument("--find", action="store_true", help="look for C64 Ultimates on the local network and exit")
    ap.add_argument("--dry-run", action="store_true", help="log SID writes instead of sending")
    ap.add_argument("--mono", action="store_true")
    ap.add_argument("--mode", choices=["poly", "mono", "unison", "octave", "fifth", "chord", "arp"],
                    help="voice mode (overrides [engine] mode)")
    ap.add_argument("--split", action="store_true", help="MIDI ch1 -> SID 1, ch2 -> SID 2, ...")
    ap.add_argument("--preset", help="startup preset (name or number) for every part")
    ap.add_argument("--ui", action="store_true", help="live terminal status view")
    ap.add_argument("--dma", action="store_true",
                    help="use the Ultimate DMA Service (TCP 64); the default unless [device].transport = \"http\"")
    ap.add_argument("--http", action="store_true", help="use the REST API (HTTP writemem) instead of the DMA service")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    cfg = load_config(a.config)
    if a.list_profiles:
        current = a.profile or cfg["controller"].get("profile", "generic")
        for pid, name, path in list_profiles(profile_dirs(a.config)):
            print(f"{'*' if pid == current else ' '} {pid:20s} {name}   ({path})")
        return
    profile = resolve_profile(cfg, a.config, a.profile)
    dev = cfg["device"]
    host = a.host or dev["host"]
    password = dev.get("password", "")

    console = None
    if a.ui:
        from rich.console import Console
        from rich.logging import RichHandler
        console = Console()
        logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(message)s",
                            handlers=[RichHandler(console=console, show_path=False)])
    else:
        logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                            format="%(asctime)s %(levelname)s %(name)s: %(message)s", datefmt="%H:%M:%S")

    if a.find:
        from finder import find_ultimates, local_address
        print(f"Searching the network around {local_address() or '?'} for C64 Ultimates...")
        found = find_ultimates(also_near=host or "", password=password)
        for d in found:
            print("  " + d.describe())
        if not found:
            print("None found. Is the C64U on this network, with Web Remote Control enabled?")
        elif len(found) == 1 and not found[0].locked:
            print(f'Put this in config.toml: [device] host = "{found[0].ip}"')
        return

    if a.sids:
        for addr, srcs in detect_sids(host, password).items():
            print(f"${addr:04X}: {' + '.join(srcs)}")
        return

    import mido  # imported late so --help / --sids work without rtmidi

    names = mido.get_input_names()
    if a.list_ports:
        print("\n".join(names) or "(no MIDI inputs)")
        return
    if a.mono:
        cfg["engine"]["mode"] = "mono"
    if a.mode:
        cfg["engine"]["mode"] = a.mode
    if a.split:
        cfg["engine"]["layout"] = "split"
    ctl = cfg["controller"]
    port_name = find_port(a.port or ctl.get("notes_port") or profile.notes_port or (names[0] if names else ""), names)

    if a.monitor:
        print(f"Monitoring {port_name!r} — Ctrl-C to stop")
        with mido.open_input(port_name) as port:
            try:
                for msg in port:
                    print(msg)
            except KeyboardInterrupt:
                pass
        return

    if not a.dry_run:
        acquire_instance_lock(lock_path_for(host))
    clock = sid.NTSC_CLOCK if dev["video"].upper() == "NTSC" else sid.PAL_CLOCK
    if not a.dry_run and not host:
        from finder import find_ultimates
        log.info("No C64U address set: searching the network...")
        found = [d for d in find_ultimates(password=password) if not d.locked]
        if len(found) != 1:
            sys.exit(("Found " + ", ".join(d.describe() for d in found) + ". " if found else "No C64 Ultimate found. ")
                     + 'Put its address in config.local.toml: [device] host = "..." (or pass --host)')
        host = found[0].ip
        log.info("Using %s. To skip the search next time, put this in config.local.toml: "
                 '[device] host = "%s"', found[0].describe(), host)
    bases = resolve_bases(cfg, host, password, a.dry_run)
    states = [SidState(b, clock) for b in bases]
    want = "http" if a.http else "dma" if a.dma else str(dev.get("transport", "dma")).lower()
    if a.dry_run:
        transport = DryRunTransport(echo=True, out=lambda s: log.info("%s", s))
    else:
        transport = open_transport(host, password, want)
    eng_cfg = cfg["engine"]
    fl = Flusher(transport, states, interval_ms=float(eng_cfg["flush_ms"]),
                 strict_order=bool(eng_cfg["strict_order"]))
    presets_path = Path(eng_cfg.get("presets", "presets.toml"))
    if not presets_path.is_absolute():
        presets_path = Path(a.config).parent / presets_path
    presets = load_presets(presets_path)
    engine = Engine(fl, cfg, presets, profile)
    if a.preset is not None:
        if not presets:
            sys.exit(f"--preset given but no presets loaded from {presets_path}")
        try:
            for part in engine.parts:
                engine.select_preset(part, int(a.preset) if a.preset.isdigit() else a.preset)
        except KeyError as e:
            sys.exit(f"{e.args[0]}; available: {', '.join(p['name'] for p in presets)}")
    engine.init_chip()
    try:
        fl.flush()
    except TransportError as e:
        sys.exit(f"Cannot reach the C64U: {e}")
    fl.start()

    port = mido.open_input(port_name, callback=engine.handle)
    opened = [port_name]
    controls = None
    ctl_want = ctl.get("controls_port") or profile.controls_port
    if ctl_want:
        ctl_name = find_port(ctl_want, names, required=False)
        if ctl_name is None:
            log.warning("controls port %r not found (knobs/buttons on it won't work). Available: %s",
                        ctl_want, names)
        elif ctl_name != port_name:
            controls = mido.open_input(ctl_name, callback=lambda m: engine.handle(m, "controls"))
            opened.append(ctl_name)
    log.info("controller profile: %s (%d controls)", profile.name, len(profile.controls))
    for c in profile.controls:
        log.debug("  %s", c.describe())
    voices = sum(len(p.slots) for p in engine.parts)
    via = "dry run" if a.dry_run else f"{host} via {'DMA' if isinstance(transport, DmaSocketTransport) else 'HTTP'}"
    log.info("playing %r -> SID %s: %s layout, %d voices, %d presets (%s). Ctrl-C to stop.",
             port_name, ", ".join(f"${b:04X}" for b in bases), engine.layout, voices,
             len(presets), via)
    def _graceful(signum, _frame):     # kill / closed terminal: shut down like Ctrl-C
        raise KeyboardInterrupt
    for sig_ in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig_, _graceful)
    try:
        if a.ui:
            from rich.live import Live
            with Live(StatusUI(engine, fl, transport, opened), console=console,
                      refresh_per_second=10, transient=False):
                while True:
                    engine.service()
                    time.sleep(0.003 if engine.voice_mode == "arp" else 0.03)   # the arp steps in service()
        else:
            while True:
                engine.service()
                time.sleep(0.003 if engine.voice_mode == "arp" else 0.03)   # the arp steps in service()
    except KeyboardInterrupt:
        print("\nPanic + shutdown")
    finally:
        port.close()
        if controls:
            controls.close()
        engine.panic()
        fl.stop(final_flush=True)
        transport.close()
        if isinstance(transport, HttpTransport) and transport.latencies:
            from probe import stats_ms
            print("write latency ms:", stats_ms(list(transport.latencies)))


if __name__ == "__main__":
    main()
