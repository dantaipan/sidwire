"""MIDI engine: turns MIDI messages into SidState edits.

Structure
  Engine  – owns the Flusher, the config, the presets and one or more Parts.
  Part    – a group of SIDs answering one MIDI channel, with its own patch,
            voices, pitch bend, sustain pedal and mono note stack.
            * layout "poly"  : one Part over all SIDs (3 voices × N SIDs),
                               channel = [midi].channel (0 = omni)
            * layout "split" : Part n = SID n on MIDI channel n (1-based)
  Slot    – one SID voice.

Released voices are muted once their release has run out (frequency set to 0): a 6581 keeps
leaking the oscillator at about -66 dBFS even with the envelope at zero, a hum at the last
note's pitch that never fades (measured, NOTES.md). service() does this; call it every ~20-50 ms.

Velocity ([engine] velocity / velocity_amount): scale = 1 - amount * (1 - velocity/127)
  * "sustain": each voice's sustain level = round(patch sustain * scale). Per voice, so every
               note of a chord keeps its own level. The attack peak is the same for every note
               (the SID envelope always rises to full), so this is an accent on the body of the
               note: most audible with a decay into a sustain below 15.
  * "volume" : the part's master volume = round(patch volume * scale) on each note-on. One
               volume per SID, so a soft note also turns down notes still sounding there.
  * "off"    : velocity is ignored.

Controllers: a profile (profiles.py, profiles/*.toml) maps the controller's CCs, notes and pitch
bend to sound parameters and actions. Messages arrive on one of two ports: "notes" (keys,
wheels, pedal) and "controls" (an optional second input for knobs / buttons, e.g. a Mackie port).
Params from the notes port go to the part playing that channel; params from the controls port and
all actions except the sustain pedal go to the knob target parts ([engine] knobs_target).

All public methods are called from the MIDI callback threads; state edits go
through flusher.edit() / flusher.gate(), which take the flusher lock.
"""
from __future__ import annotations

import copy
import logging
import threading
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

import sid
from profiles import Control, Profile
from profiles import generic as generic_profile
from transport import Flusher

log = logging.getLogger("engine")

WAVEFORMS = {
    "tri": sid.TRI, "saw": sid.SAW, "pulse": sid.PULSE, "noise": sid.NOISE,
    "tri+saw": sid.TRI | sid.SAW, "tri+pulse": sid.TRI | sid.PULSE,
    "saw+pulse": sid.SAW | sid.PULSE, "tri+saw+pulse": sid.TRI | sid.SAW | sid.PULSE,
}
FILTER_MODES = {"off": 0, "lp": sid.LP, "bp": sid.BP, "hp": sid.HP, "lp+hp": sid.LP | sid.HP,
                "lp+bp": sid.LP | sid.BP, "bp+hp": sid.BP | sid.HP, "lp+bp+hp": sid.LP | sid.BP | sid.HP}
# SID release times for the full envelope, by release nibble (datasheet), seconds
RELEASE_S = [0.006, 0.024, 0.048, 0.072, 0.114, 0.168, 0.204, 0.24,
             0.3, 0.75, 1.5, 2.4, 3.0, 9.0, 15.0, 24.0]
PARAM_MAX = {"pulse_width": 4095, "cutoff": 2047, "resonance": 15, "attack": 15,
             "decay": 15, "sustain": 15, "release": 15, "volume": 15}
VELOCITY_MODES = ("off", "sustain", "volume")
PATCH_KEYS = {"name", "waveform", "pulse_width", "attack", "decay", "sustain", "release",
              "filter_mode", "filter_voices", "cutoff", "resonance", "volume"}

DEFAULTS: dict = {
    "device": {"host": "", "password": "", "video": "PAL", "transport": "dma"},
    "sid": {"bases": [0xD400]},
    "controller": {"profile": "generic", "notes_port": "", "controls_port": ""},
    "engine": {"mode": "poly", "layout": "poly", "legato": True, "flush_ms": 5,
               "strict_order": True, "hard_restart": False, "presets": "presets.toml",
               "mute_after_release": True, "velocity": "sustain", "velocity_amount": 1.0,
               "knobs_target": "all", "filter_cycle": ["lp", "bp", "hp", "off"]},
    "patch": {"name": "init", "waveform": "pulse", "pulse_width": 2048, "attack": 0, "decay": 9,
              "sustain": 10, "release": 4, "filter_mode": "lp", "filter_voices": True,
              "cutoff": 2047, "resonance": 0, "volume": 15},
    "programs": {"waveforms": list(WAVEFORMS)},
}


def load_config(path: str | Path) -> dict:
    """config.toml, then config.local.toml next to it (your own settings, kept out of git)."""
    cfg = copy.deepcopy(DEFAULTS)
    p = Path(path)
    for f in (p, p.with_name(p.stem + ".local" + p.suffix)):
        if not f.exists():
            continue
        user = tomllib.loads(f.read_text())
        legacy = sorted({"midi", "cc", "mackie", "pads"} & set(user))
        if legacy:
            log.warning("%s: [%s] are no longer read; the controller settings moved to profiles "
                        "(profiles/*.toml, [controller] profile = ...)", f, "], [".join(legacy))
        for section, values in user.items():
            if isinstance(values, dict):
                cfg.setdefault(section, {}).update(values)
            else:
                cfg[section] = values
    return cfg


def load_presets(path: str | Path) -> list[dict]:
    """presets.toml: a list of [[preset]] tables. Unknown keys are rejected."""
    p = Path(path)
    if not p.exists():
        return []
    presets = tomllib.loads(p.read_text()).get("preset", [])
    for i, pr in enumerate(presets):
        bad = set(pr) - PATCH_KEYS
        if bad:
            raise ValueError(f"{p}: preset {i} ({pr.get('name', '?')}) has unknown keys {sorted(bad)}")
        if "waveform" in pr and pr["waveform"] not in WAVEFORMS:
            raise ValueError(f"{p}: preset {pr.get('name', i)}: unknown waveform {pr['waveform']!r}")
        if "filter_mode" in pr and pr["filter_mode"] not in FILTER_MODES:
            raise ValueError(f"{p}: preset {pr.get('name', i)}: unknown filter_mode {pr['filter_mode']!r}")
        pr.setdefault("name", f"preset {i}")
    return presets


@dataclass
class Slot:
    sid: int
    voice: int
    note: int | None = None   # note assigned (sounding or releasing)
    gated: bool = False
    on_time: int = 0
    off_time: int = 0
    off_at: float = 0.0       # clock time of the note-off (for mute_after_release)
    vel: float = 1.0          # velocity scale of the note (velocity "sustain")


@dataclass
class Part:
    index: int
    channel: int                     # 1-16, or 0 = omni
    sids: list[int]
    slots: list[Slot]
    patch: dict
    bend: float = 0.0
    pedal: bool = False
    held_by_pedal: set[int] = field(default_factory=set)
    mono_stack: list[int] = field(default_factory=list)
    muted: bool = False
    norm: dict[str, float] = field(default_factory=dict)   # fractional encoder positions
    preset_index: int = -1           # last preset selected (preset_next / preset_prev)
    vel: float = 1.0                 # velocity scale of the last note (velocity "volume")


class Engine:
    def __init__(self, flusher: Flusher, cfg: dict, presets: list[dict] | None = None,
                 profile: Profile | None = None):
        self.fl = flusher
        self.states = flusher.states
        self.cfg = cfg
        eng = cfg["engine"]
        self.profile = profile if profile is not None else generic_profile()
        self.mono = eng.get("mode", "poly") == "mono"
        self.legato = bool(eng.get("legato", True))
        self.hard_restart = bool(eng.get("hard_restart", False))
        self.bend_range = self.profile.bend_range
        self.presets = presets if presets is not None else []
        self.waveform_programs = list(cfg["programs"]["waveforms"])
        self.clock = 0
        self._tick_lock = threading.Lock()
        self.lock = threading.RLock()    # MIDI callbacks vs service() on another thread
        self.mute_after_release = bool(eng.get("mute_after_release", True))
        self.now = time.monotonic        # replaceable clock (golden vectors use a fake one)
        self.velocity_mode = "off"
        self.velocity_amount = 0.0
        self._set_velocity_fields(eng.get("velocity", "sustain"), eng.get("velocity_amount", 1.0))
        self.filter_cycle = list(eng.get("filter_cycle", ["lp", "bp", "hp", "off"]))
        bad = [m for m in self.filter_cycle if m not in FILTER_MODES]
        if bad or not self.filter_cycle:
            raise ValueError(f"[engine] filter_cycle: unknown or empty {bad or self.filter_cycle}")

        layout = eng.get("layout", "poly")
        n = len(self.states)
        base_patch = dict(cfg["patch"])
        if layout == "split" and n > 1:
            groups = [(i + 1, [i]) for i in range(n)]
        else:
            if layout == "split":
                log.warning("layout 'split' needs 2+ SIDs; using poly")
            groups = [(self.profile.channel, list(range(n)))]
        self.layout = "split" if len(groups) > 1 else "poly"
        self.parts: list[Part] = []
        for i, (ch, sids) in enumerate(groups):
            if self.mono:
                slots = [Slot(sids[0], 0)]
            else:
                slots = [Slot(s, v) for s in sids for v in range(sid.NUM_VOICES)]
            self.parts.append(Part(i, ch, sids, slots, dict(base_patch)))

        self._slot_at = {(s.sid, s.voice): s for p in self.parts for s in p.slots}

        target = eng.get("knobs_target", "all")
        if target == "all":
            self.knob_parts = self.parts
        else:
            self.knob_parts = [self.parts[min(int(target), len(self.parts)) - 1]]

    # ---------------------------------------------------------------- compat helpers
    @property
    def patch(self) -> dict:
        return self.parts[0].patch

    @property
    def slots(self) -> list[Slot]:
        return [s for p in self.parts for s in p.slots]

    def _tick(self) -> int:
        with self._tick_lock:
            self.clock += 1
            return self.clock

    # ---------------------------------------------------------------- velocity
    def _set_velocity_fields(self, mode: str, amount: float) -> None:
        if mode not in VELOCITY_MODES:
            raise ValueError(f"[engine] velocity must be one of {', '.join(VELOCITY_MODES)}, not {mode!r}")
        self.velocity_mode = mode
        self.velocity_amount = min(1.0, max(0.0, float(amount)))

    def set_velocity(self, mode: str, amount: float) -> None:
        """Change the velocity response at run time (plugin). Sounding notes go back to full level."""
        with self.lock:
            self._set_velocity_fields(mode, amount)
            with self.fl.edit():
                for part in self.parts:
                    part.vel = 1.0
                    for s in part.slots:
                        s.vel = 1.0
                    for i in part.sids:
                        st = self.states[i]
                        for v in range(sid.NUM_VOICES):
                            st.set_adsr(v, s=self._sustain_for(part, i, v))
                        if not part.muted:
                            st.set_volume(self._volume_for(part))

    def _vel_scale(self, velocity: int) -> float:
        return 1.0 - self.velocity_amount * (1.0 - velocity / 127.0)

    def _sustain_for(self, part: Part, sid_index: int, voice: int) -> int:
        """The sustain nibble to write for one voice: the patch's, scaled by its note's velocity."""
        if self.velocity_mode != "sustain":
            return part.patch["sustain"]
        slot = self._slot_at.get((sid_index, voice))
        return round(part.patch["sustain"] * (slot.vel if slot is not None else 1.0))

    def _volume_for(self, part: Part) -> int:
        if self.velocity_mode != "volume":
            return part.patch["volume"]
        return round(part.patch["volume"] * part.vel)

    # ---------------------------------------------------------------- setup / patches
    def init_chip(self) -> None:
        """Full register reset and startup patch on every SID."""
        with self.fl.edit():
            for st in self.states:
                st.regs[:] = bytes(sid.NUM_REGS)
                st.mark_all_dirty()
        for part in self.parts:
            self.apply_patch(part, part.patch, force=True)

    def apply_patch(self, part: Part, patch: dict, force: bool = False) -> None:
        """Apply (a subset of) patch keys to all SIDs of a part. Gates are kept."""
        p = part.patch
        p.update({k: v for k, v in patch.items() if k in PATCH_KEYS})
        if "volume" in patch:
            part.muted = False
        part.norm.clear()   # encoders resume from the new values
        with self.fl.edit():
            for i in part.sids:
                st = self.states[i]
                for v in range(sid.NUM_VOICES):
                    if force or "waveform" in patch:
                        st.set_waveform(v, WAVEFORMS[p["waveform"]])
                    if force or "pulse_width" in patch:
                        st.set_pulse_width(v, p["pulse_width"])
                    if force or patch.keys() & {"attack", "decay", "sustain", "release"}:
                        st.set_adsr(v, p["attack"], p["decay"], self._sustain_for(part, i, v), p["release"])
                    if force or patch.keys() & {"filter_voices", "filter_mode"}:
                        # routing a voice into the filter with no mode selected silences it
                        st.set_filter_route(v, bool(p["filter_voices"]) and p["filter_mode"] != "off")
                if force or "filter_mode" in patch:
                    st.set_filter_mode(FILTER_MODES[p["filter_mode"]])
                if force or "cutoff" in patch:
                    st.set_cutoff(p["cutoff"])
                if force or "resonance" in patch:
                    st.set_resonance(p["resonance"])
                if force or "volume" in patch:
                    st.set_volume(0 if part.muted else self._volume_for(part))

    def select_preset(self, part: Part, index_or_name) -> dict | None:
        if not self.presets:
            return None
        if isinstance(index_or_name, str):
            matches = [p for p in self.presets if p["name"].lower() == index_or_name.lower()]
            if not matches:
                raise KeyError(f"no preset named {index_or_name!r}")
            index = self.presets.index(matches[0])
        else:
            index = index_or_name % len(self.presets)
        pr = self.presets[index]
        self.apply_patch(part, pr)
        part.preset_index = index
        log.info("part %d: preset %r", part.index + 1, pr["name"])
        return pr

    # ---------------------------------------------------------------- dispatch
    def part_for(self, msg) -> Part | None:
        ch = getattr(msg, "channel", None)
        for part in self.parts:
            if part.channel == 0 or ch is None or ch == part.channel - 1:
                return part
        return None

    def handle(self, msg, port: str = "notes") -> None:
        """A MIDI message from the "notes" or the "controls" port (see the module docstring)."""
        with self.lock:
            try:
                self._handle(msg, port)
            except Exception:  # noqa: BLE001 — never kill the MIDI thread
                log.exception("error handling %s from %s", msg, port)

    def _match(self, port: str, kind: str, channel0: int | None, number: int = 0) -> list[Control]:
        return [c for c in self.profile.controls if c.matches(port, kind, channel0, number)]

    def _handle(self, msg, port: str) -> None:
        t = msg.type
        ch = getattr(msg, "channel", None)
        if t in ("note_on", "note_off"):
            hits = self._match(port, "note", ch, msg.note)
            if hits:                                  # a pad / button: never plays the SID
                if t == "note_on" and msg.velocity > 0:
                    for c in hits:
                        self._action(c.action, self.knob_parts)
                return
            if port != "notes" or ch + 1 in self.profile.ignore_channels:
                return
        elif port != "notes":                         # knobs / faders / buttons
            if t == "control_change":
                for c in self._match(port, "cc", ch, msg.control):
                    self._apply_control(c, msg.value, self.knob_parts)
            elif t == "pitchwheel":
                for c in self._match(port, "pitchbend", ch):
                    for part in self.knob_parts:
                        self.set_param(part, c.param, (msg.pitch + 8192) / 16383.0)
            return
        part = self.part_for(msg)
        if part is None:
            return
        if t == "note_on" and msg.velocity > 0:
            self.note_on(msg.note, msg.velocity, part)
        elif t == "note_off" or t == "note_on":
            self.note_off(msg.note, part)
        elif t == "control_change":
            self.control_change(msg.control, msg.value, part, ch)
        elif t == "pitchwheel":
            hits = self._match("notes", "pitchbend", ch)
            if hits:
                for c in hits:
                    self.set_param(part, c.param, (msg.pitch + 8192) / 16383.0)
            else:
                self.pitch_bend(msg.pitch, part)
        elif t == "program_change":
            self.program_change(msg.program, part)

    def _apply_control(self, c: Control, value: int, parts: list[Part]) -> None:
        if c.param:
            for part in parts:
                if c.mode == "absolute":
                    self.set_param(part, c.param, value / 127.0)
                else:
                    self.nudge_param(part, c.param, c.decode_relative(value) / c.steps)
        elif c.action == "sustain_pedal":
            for part in parts:
                self.set_pedal(part, value >= 64)
        elif value >= 64:                             # actions act on the knob target parts
            self._action(c.action, self.knob_parts)

    def _action(self, action: str, parts: list[Part]) -> None:
        if action == "filter_mode_next":
            self.cycle_filter_mode(parts)
        elif action in ("preset_next", "preset_prev") and self.presets:
            n = len(self.presets)
            cur = parts[0].preset_index
            nxt = (cur + 1) % n if action == "preset_next" else (cur - 1) % n if cur >= 0 else n - 1
            for part in parts:
                self.select_preset(part, nxt)
        elif action == "waveform_next":
            names = list(WAVEFORMS)
            cur = parts[0].patch["waveform"]
            nxt = names[(names.index(cur) + 1) % len(names)] if cur in names else names[0]
            for part in parts:
                self.apply_patch(part, {"waveform": nxt})
        elif action == "panic":
            self._panic()
        elif action == "sustain_pedal":            # from a note: not supported (profiles reject it)
            pass

    # ---------------------------------------------------------------- notes
    def _p(self, part: Part | None) -> Part:
        return part if part is not None else self.parts[0]

    def _unmute(self, part: Part) -> None:
        if part.muted:
            part.muted = False
            with self.fl.edit():
                for i in part.sids:
                    self.states[i].set_volume(self._volume_for(part))

    def _alloc(self, part: Part, note: int) -> Slot:
        for s in part.slots:                        # same note already assigned → reuse
            if s.note == note:
                return s
        free = [s for s in part.slots if not s.gated]
        if free:                                    # released longest ago first
            return min(free, key=lambda s: s.off_time)
        return min(part.slots, key=lambda s: s.on_time)   # steal oldest

    def _start_voice(self, part: Part, slot: Slot, note: int, velocity: int | None = None) -> None:
        """velocity None = keep the slot's level (mono retrigger of a held note)."""
        st = self.states[slot.sid]
        v = slot.voice
        p = part.patch
        if velocity is not None and self.velocity_mode == "sustain":
            slot.vel = self._vel_scale(velocity)
        if self.hard_restart:
            with self.fl.edit():
                st.set_gate(v, False)
                st.set_adsr(v, 0, 0, 0, 0)
            self.fl.barrier()
            with self.fl.edit():
                st.set_adsr(v, p["attack"], p["decay"], self._sustain_for(part, slot.sid, v), p["release"])
        elif slot.gated:                             # steal / retrigger: gate-off goes out first
            self.fl.gate(slot.sid, v, False)
            self.fl.barrier()
        with self.fl.edit():
            st.set_note(v, note, part.bend)
            st.set_adsr(v, s=self._sustain_for(part, slot.sid, v))   # before the gate (flush order)
        self.fl.gate(slot.sid, v, True)
        slot.note, slot.gated, slot.on_time = note, True, self._tick()

    def _release_voice(self, slot: Slot) -> None:
        self.fl.gate(slot.sid, slot.voice, False)
        slot.gated, slot.off_time, slot.off_at = False, self._tick(), self.now()

    def service(self) -> None:
        """Mute voices whose release has run out (see module docstring)."""
        if not self.mute_after_release:
            return
        with self.lock:
            now = self.now()
            for part in self.parts:
                done_after = RELEASE_S[part.patch["release"]] * 1.2 + 0.05
                for s in part.slots:
                    if not s.gated and s.note is not None and now - s.off_at >= done_after:
                        with self.fl.edit():
                            self.states[s.sid].set_freq(s.voice, 0)
                        s.note = None

    def note_on(self, note: int, velocity: int = 100, part: Part | None = None) -> None:
        part = self._p(part)
        if self.velocity_mode == "volume":
            part.vel = self._vel_scale(velocity)
            if not part.muted:
                with self.fl.edit():
                    for i in part.sids:
                        self.states[i].set_volume(self._volume_for(part))
        self._unmute(part)
        part.held_by_pedal.discard(note)
        if self.mono:
            slot = part.slots[0]
            if note in part.mono_stack:
                part.mono_stack.remove(note)
            part.mono_stack.append(note)
            if slot.gated and self.legato:
                with self.fl.edit():
                    self.states[slot.sid].set_note(slot.voice, note, part.bend)
                slot.note = note
            else:
                self._start_voice(part, slot, note, velocity)
            return
        self._start_voice(part, self._alloc(part, note), note, velocity)

    def note_off(self, note: int, part: Part | None = None) -> None:
        part = self._p(part)
        if part.pedal:
            part.held_by_pedal.add(note)
            return
        if self.mono:
            slot = part.slots[0]
            if note in part.mono_stack:
                part.mono_stack.remove(note)
            if slot.note != note:
                return
            if part.mono_stack and self.legato:
                prev = part.mono_stack[-1]
                with self.fl.edit():
                    self.states[slot.sid].set_note(slot.voice, prev, part.bend)
                slot.note = prev
            elif part.mono_stack:
                self._start_voice(part, slot, part.mono_stack[-1])
            else:
                self._release_voice(slot)
            return
        for s in part.slots:
            if s.note == note and s.gated:
                self._release_voice(s)

    # ---------------------------------------------------------------- controllers
    def pitch_bend(self, pitch: int, part: Part | None = None) -> None:
        part = self._p(part)
        part.bend = sid.bend_to_semitones(pitch, self.bend_range)
        with self.fl.edit():
            for s in part.slots:
                if s.note is not None:
                    self.states[s.sid].set_note(s.voice, s.note, part.bend)

    def control_change(self, cc: int, value: int, part: Part | None = None, channel0: int | None = None) -> None:
        """A CC from the notes port (channel0 = its MIDI channel 0-15, None = any)."""
        part = self._p(part)
        if cc in (120, 123):
            self.panic()
            return
        for c in self._match("notes", "cc", channel0, cc):
            self._apply_control(c, value, [part])

    def set_pedal(self, part: Part, down: bool) -> None:
        part.pedal = down
        if not down:
            for n in sorted(part.held_by_pedal):
                self.note_off(n, part)
            part.held_by_pedal.clear()

    def get_param(self, part: Part, name: str) -> float:
        return part.patch[name] / PARAM_MAX[name]

    def set_param(self, part: Part, name: str, norm: float) -> None:
        if name not in PARAM_MAX:
            log.warning("unknown parameter %r", name)
            return
        norm = min(1.0, max(0.0, norm))
        part.norm[name] = norm
        value = round(norm * PARAM_MAX[name])
        if value == part.patch[name] and not (name == "volume" and part.muted):
            return
        part.patch[name] = value
        with self.fl.edit():
            for i in part.sids:
                st = self.states[i]
                if name == "pulse_width":
                    for v in range(sid.NUM_VOICES):
                        st.set_pulse_width(v, value)
                elif name == "cutoff":
                    st.set_cutoff(value)
                elif name == "resonance":
                    st.set_resonance(value)
                elif name == "sustain":
                    for v in range(sid.NUM_VOICES):
                        st.set_adsr(v, s=self._sustain_for(part, i, v))
                elif name in ("attack", "decay", "release"):
                    for v in range(sid.NUM_VOICES):
                        st.set_adsr(v, **{name[0]: value})
                elif name == "volume":
                    st.set_volume(self._volume_for(part))
        if name == "volume":
            part.muted = False

    def nudge_param(self, part: Part, name: str, delta: float) -> None:
        """Relative change (encoders). Keeps a fractional position so slow turns still move."""
        if name in PARAM_MAX:
            self.set_param(part, name, part.norm.get(name, self.get_param(part, name)) + delta)

    def cycle_filter_mode(self, parts: list[Part] | None = None) -> str:
        """Next filter mode in [engine] filter_cycle (default: for the knob target parts)."""
        parts = parts or self.knob_parts
        modes = self.filter_cycle
        cur = parts[0].patch["filter_mode"]
        nxt = modes[(modes.index(cur) + 1) % len(modes)] if cur in modes else modes[0]
        for part in parts:
            self.apply_patch(part, {"filter_mode": nxt})
        log.info("filter mode -> %s", nxt)
        return nxt

    def program_change(self, program: int, part: Part | None = None) -> None:
        part = self._p(part)
        if self.presets:
            self.select_preset(part, program)
            return
        name = self.waveform_programs[program % len(self.waveform_programs)]
        self.apply_patch(part, {"waveform": name})
        log.info("part %d: program %d -> %s", part.index + 1, program, name)

    def panic(self) -> None:
        """All gates off and master volume 0 on every SID. Volume returns on the next note."""
        with self.lock:
            self._panic()

    def _panic(self) -> None:
        with self.fl.edit():
            for st in self.states:
                for v in range(sid.NUM_VOICES):
                    st.set_gate(v, False)
                st.set_volume(0)
        for part in self.parts:
            for s in part.slots:
                s.gated, s.off_time, s.off_at = False, self._tick(), self.now()
            part.mono_stack.clear()
            part.held_by_pedal.clear()
            part.pedal = False
            part.muted = True
        log.info("panic")
