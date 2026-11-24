"""Controller profiles: how one MIDI keyboard / controller talks to the bridge.

A profile is a TOML file (profiles/*.toml; the format is documented in profiles/generic.toml).
It names the MIDI ports, the keys' channel, the bend range, channels to ignore, and a list of
[[control]] entries that map a CC, note or pitch bend to a sound parameter or an action. The
plugin reads the same files (cpp/core/src/Profile.cpp), so a profile works in both.
"""
from __future__ import annotations

import tomllib
from dataclasses import dataclass, field, replace
from functools import lru_cache
from pathlib import Path

PROFILES_DIR = Path(__file__).resolve().parent / "profiles"

PARAMS = ("pulse_width", "cutoff", "resonance", "attack", "decay", "sustain", "release", "volume")
ACTIONS = ("sustain_pedal", "filter_mode_next", "preset_next", "preset_prev", "waveform_next", "panic",
           "voice_mode_next")
MODES = ("absolute", "relative-signed", "relative-twos", "relative-offset")
PORTS = ("notes", "controls")


class ProfileError(ValueError):
    pass


@dataclass(frozen=True)
class Control:
    kind: str                  # "cc" | "note" | "pitchbend"
    number: int = 0            # CC or note number (0 for pitch bend)
    port: str = "notes"        # "notes" | "controls"
    channel: int = 0           # 1-16, 0 = any
    param: str = ""            # a sound parameter ...
    action: str = ""           # ... or an action
    mode: str = "absolute"     # CC params: how the value is read
    steps: float = 128.0       # relative modes: ticks for a full sweep

    def matches(self, port: str, kind: str, channel0: int | None, number: int) -> bool:
        """channel0 = MIDI channel 0-15 of the message (None = any)."""
        return (self.port == port and self.kind == kind and self.number == number
                and (self.channel == 0 or channel0 is None or self.channel == channel0 + 1))

    def describe(self) -> str:
        trig = {"cc": f"CC{self.number}", "note": f"note {self.number}", "pitchbend": "pitch bend"}[self.kind]
        where = f"{trig}{f' ch{self.channel}' if self.channel else ''}{' (controls port)' if self.port == 'controls' else ''}"
        what = self.param + (f" [{self.mode}]" if self.mode != "absolute" else "") if self.param else self.action
        return f"{where} -> {what}"

    def decode_relative(self, value: int) -> int:
        """Relative encoder value -> signed ticks."""
        if self.mode == "relative-signed":      # Mackie V-Pot: bit 6 = sign, bits 0-5 = ticks
            return -(value & 0x3F) if value & 0x40 else value & 0x3F
        if self.mode == "relative-twos":        # two's complement: 1..63 up, 127..65 down
            return value if value < 64 else value - 128
        return value - 64                       # relative-offset: 64 = no change


@dataclass(frozen=True)
class Profile:
    name: str
    description: str = ""
    notes_port: str = ""
    controls_port: str = ""
    channel: int = 0                     # keys: 1-16, 0 = any
    bend_range: float = 2.0
    ignore_channels: tuple[int, ...] = ()
    controls: tuple[Control, ...] = field(default_factory=tuple)

    def with_channel(self, channel: int) -> "Profile":
        return replace(self, channel=channel)


def _int(v, where: str, lo: int, hi: int) -> int:
    if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
        raise ProfileError(f"{where}: must be a whole number {lo}-{hi}, not {v!r}")
    return v


def _str(v, where: str) -> str:
    if not isinstance(v, str):
        raise ProfileError(f"{where}: must be a string, not {v!r}")
    return v


def _keys(table: dict, allowed: set[str], where: str) -> None:
    bad = set(table) - allowed
    if bad:
        raise ProfileError(f"{where}: unknown key(s) {', '.join(sorted(bad))} (allowed: {', '.join(sorted(allowed))})")


def _control(t: dict, where: str) -> Control:
    _keys(t, {"cc", "note", "pitchbend", "port", "channel", "param", "action", "mode", "steps"}, where)
    triggers = [k for k in ("cc", "note", "pitchbend") if k in t]
    if len(triggers) != 1:
        raise ProfileError(f"{where}: needs exactly one of cc = N, note = N or pitchbend = true")
    kind = triggers[0]
    number = 0
    if kind == "pitchbend":
        if t["pitchbend"] is not True:
            raise ProfileError(f"{where}: pitchbend must be true")
    else:
        number = _int(t[kind], f"{where}: {kind}", 0, 127)
    port = _str(t.get("port", "notes"), f"{where}: port")
    if port not in PORTS:
        raise ProfileError(f"{where}: port must be \"notes\" or \"controls\", not {port!r}")
    channel = _int(t.get("channel", 0), f"{where}: channel", 0, 16)
    if ("param" in t) == ("action" in t):
        raise ProfileError(f"{where}: needs exactly one of param = \"...\" or action = \"...\"")
    param = _str(t.get("param", ""), f"{where}: param")
    action = _str(t.get("action", ""), f"{where}: action")
    if param and param not in PARAMS:
        raise ProfileError(f"{where}: unknown param {param!r} (one of {', '.join(PARAMS)})")
    if action and action not in ACTIONS:
        raise ProfileError(f"{where}: unknown action {action!r} (one of {', '.join(ACTIONS)})")
    mode = _str(t.get("mode", "absolute"), f"{where}: mode")
    if mode not in MODES:
        raise ProfileError(f"{where}: unknown mode {mode!r} (one of {', '.join(MODES)})")
    if mode != "absolute" and not (kind == "cc" and param):
        raise ProfileError(f"{where}: relative modes are for CC knobs that set a param")
    steps = t.get("steps", 128)
    if isinstance(steps, bool) or not isinstance(steps, (int, float)) or steps <= 0:
        raise ProfileError(f"{where}: steps must be a positive number")
    if "steps" in t and mode == "absolute":
        raise ProfileError(f"{where}: steps only applies to relative modes")
    if kind == "note" and param:
        raise ProfileError(f"{where}: a note (pad / button) triggers an action, not a param")
    if kind == "pitchbend" and action:
        raise ProfileError(f"{where}: pitch bend sets a param, not an action")
    if action == "sustain_pedal" and kind != "cc":
        raise ProfileError(f"{where}: sustain_pedal needs a CC")
    return Control(kind, number, port, channel, param, action, mode, float(steps))


def parse_profile(text: str, source: str = "profile") -> Profile:
    try:
        d = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ProfileError(f"{source}: {e}") from None
    _keys(d, {"name", "description", "ports", "keys", "control"}, source)
    if "name" not in d:
        raise ProfileError(f"{source}: needs a name = \"...\"")
    ports = d.get("ports", {})
    keys = d.get("keys", {})
    _keys(ports, {"notes", "controls"}, f"{source} [ports]")
    _keys(keys, {"channel", "bend_range", "ignore_channels"}, f"{source} [keys]")
    bend = keys.get("bend_range", 2)
    if isinstance(bend, bool) or not isinstance(bend, (int, float)) or not 0 < bend <= 48:
        raise ProfileError(f"{source} [keys]: bend_range must be a number of semitones, 1-48")
    ignore = keys.get("ignore_channels", [])
    if not isinstance(ignore, list):
        raise ProfileError(f"{source} [keys]: ignore_channels must be a list, e.g. [10]")
    controls = d.get("control", [])
    if not isinstance(controls, list):
        raise ProfileError(f"{source}: use [[control]] (double brackets) for each control")
    return Profile(
        name=_str(d["name"], f"{source}: name"),
        description=_str(d.get("description", ""), f"{source}: description"),
        notes_port=_str(ports.get("notes", ""), f"{source} [ports] notes"),
        controls_port=_str(ports.get("controls", ""), f"{source} [ports] controls"),
        channel=_int(keys.get("channel", 0), f"{source} [keys] channel", 0, 16),
        bend_range=float(bend),
        ignore_channels=tuple(_int(c, f"{source} [keys] ignore_channels", 1, 16) for c in ignore),
        controls=tuple(_control(c, f"{source} control #{i + 1}") for i, c in enumerate(controls)),
    )


def find_profile(name_or_path: str, dirs: list[Path] | None = None) -> Path:
    """A profile by file path, or by name (file stem) in dirs (default: profiles/)."""
    p = Path(name_or_path).expanduser()
    if p.suffix == ".toml" and p.exists():
        return p
    for d in (dirs or []) + [PROFILES_DIR]:
        cand = Path(d) / f"{name_or_path}.toml"
        if cand.exists():
            return cand
    known = ", ".join(i for i, _, _ in list_profiles(dirs)) or "none"
    raise ProfileError(f"no controller profile {name_or_path!r} (available: {known})")


def load_profile(name_or_path: str, dirs: list[Path] | None = None) -> Profile:
    path = find_profile(name_or_path, dirs)
    return parse_profile(path.read_text(), str(path))


def list_profiles(dirs: list[Path] | None = None) -> list[tuple[str, str, Path]]:
    """(id, name, path) for every readable profile; earlier dirs win, profiles/ last."""
    seen: dict[str, tuple[str, str, Path]] = {}
    for d in (dirs or []) + [PROFILES_DIR]:
        for f in sorted(Path(d).glob("*.toml")) if Path(d).is_dir() else []:
            if f.stem in seen:
                continue
            try:
                seen[f.stem] = (f.stem, parse_profile(f.read_text(), str(f)).name, f)
            except ProfileError as e:
                seen[f.stem] = (f.stem, f"(invalid: {e})", f)
    return sorted(seen.values())


@lru_cache(maxsize=1)
def generic() -> Profile:
    """The default profile: profiles/generic.toml."""
    return load_profile("generic")
