import copy
import threading
import time

import mido
import pytest

import sid
from engine import DEFAULTS, Engine, load_presets
from profiles import generic, load_profile

OXYGEN = load_profile("oxygen-49-mkv")
from sid import SidState, note_to_sid
from transport import DryRunTransport, Flusher, TransportError

PRESETS = [
    {"name": "Lead", "waveform": "pulse", "attack": 0, "decay": 9, "sustain": 10, "release": 4},
    {"name": "Bass", "waveform": "saw", "cutoff": 600, "resonance": 10, "filter_mode": "lp"},
    {"name": "Organ", "waveform": "tri+pulse", "filter_mode": "off"},
]


def make(layout="poly", nsids=2, presets=None, mode="poly", target="all", profile=OXYGEN):
    cfg = copy.deepcopy(DEFAULTS)
    cfg["engine"].update(layout=layout, mode=mode, knobs_target=target)
    t = DryRunTransport(echo=False)
    states = [SidState(0xD400 + 0x20 * i) for i in range(nsids)]
    fl = Flusher(t, states)
    e = Engine(fl, cfg, presets if presets is not None else [dict(p) for p in PRESETS], profile)
    e.init_chip()
    fl.flush()
    t.writes.clear()
    return e, states, fl, t


def on(e, n, ch=0): e.handle(mido.Message("note_on", note=n, velocity=100, channel=ch))
def off(e, n, ch=0): e.handle(mido.Message("note_off", note=n, channel=ch))
def gates(st): return [bool(st.get_control(v) & sid.GATE) for v in range(3)]


# ---------------------------------------------------------------- 6 voices / split

def test_poly_six_voices_across_two_sids():
    e, (a, b), fl, t = make()
    assert e.layout == "poly" and len(e.parts) == 1 and len(e.slots) == 6
    for n in range(60, 66):
        on(e, n)
    assert gates(a) == [True] * 3 and gates(b) == [True] * 3
    on(e, 70)                                     # 7th note steals the oldest (60 on SID 1 voice 1)
    assert a.get_freq(0) == note_to_sid(70)


def test_split_routes_channels_to_sids():
    e, (a, b), fl, t = make(layout="split")
    assert e.layout == "split" and [p.channel for p in e.parts] == [1, 2]
    on(e, 60, ch=0)
    on(e, 60, ch=1)                               # same note, other channel → other SID
    assert gates(a) == [True, False, False] and gates(b) == [True, False, False]
    on(e, 62, ch=5)                               # channel 6: no part → ignored
    assert sum(gates(a) + gates(b)) == 2
    off(e, 60, ch=1)
    assert gates(a)[0] and not gates(b)[0]


def test_split_with_one_sid_falls_back_to_poly():
    e, (a,), fl, t = make(layout="split", nsids=1)
    assert e.layout == "poly" and len(e.parts) == 1


def test_split_per_part_bend_and_cc():
    e, (a, b), fl, t = make(layout="split")
    on(e, 60, ch=0); on(e, 60, ch=1)
    e.handle(mido.Message("pitchwheel", pitch=8191, channel=1))
    assert a.get_freq(0) == note_to_sid(60) and b.get_freq(0) == note_to_sid(60, 2.0)
    e.handle(mido.Message("control_change", control=74, value=0, channel=0))
    assert a.get_cutoff() == 0 and b.get_cutoff() == 2047


def test_split_program_change_per_part():
    e, (a, b), fl, t = make(layout="split")
    e.handle(mido.Message("program_change", program=1, channel=1))
    assert b.get_control(0) & 0xF0 == sid.SAW and a.get_control(0) & 0xF0 == sid.PULSE
    assert e.parts[1].patch["name"] == "Bass" and e.parts[0].patch["name"] == "init"


def test_mackie_target_part():
    e, (a, b), fl, t = make(layout="split", target=2)
    e.handle(mido.Message("control_change", control=16, value=65), "controls")
    assert a.get_cutoff() == 2047 and b.get_cutoff() < 2047


def test_mackie_target_all_moves_both():
    e, (a, b), fl, t = make(layout="split")
    e.handle(mido.Message("control_change", control=16, value=65), "controls")
    assert a.get_cutoff() == b.get_cutoff() < 2047


def test_split_mono_one_voice_per_part():
    e, (a, b), fl, t = make(layout="split", mode="mono")
    assert [len(p.slots) for p in e.parts] == [1, 1]
    on(e, 60, ch=0); on(e, 64, ch=0); on(e, 67, ch=1)
    assert gates(a) == [True, False, False] and gates(b) == [True, False, False]
    assert a.get_freq(0) == note_to_sid(64)


# ---------------------------------------------------------------- presets

def test_preset_applies_and_keeps_gates():
    e, (a, b), fl, t = make()
    on(e, 60)
    e.handle(mido.Message("program_change", program=1))
    assert a.get_control(0) == sid.SAW | sid.GATE
    assert a.get_cutoff() == 600 and a.regs[0x17] >> 4 == 10
    assert b.get_cutoff() == 600                 # poly part spans both SIDs
    assert e.patch["name"] == "Bass"
    assert e.patch["sustain"] == 10              # keys not in preset are kept
    assert a.get_adsr(1) == (0, 9, 10, 4)
    assert a.get_adsr(0) == (0, 9, 8, 4)         # the held note keeps its velocity (100/127)


def test_preset_wraps_and_by_name():
    e, (a, b), fl, t = make()
    e.handle(mido.Message("program_change", program=4))   # 4 % 3 = 1
    assert e.patch["name"] == "Bass"
    e.select_preset(e.parts[0], "organ")
    assert e.patch["name"] == "Organ"
    with pytest.raises(KeyError):
        e.select_preset(e.parts[0], "nope")


def test_filter_off_unroutes_voices():
    e, (a, b), fl, t = make()
    assert a.regs[0x17] & 0x07 == 0x07
    e.select_preset(e.parts[0], "Organ")
    assert a.regs[0x17] & 0x07 == 0 and a.regs[0x18] & 0x70 == 0
    e.select_preset(e.parts[0], "Bass")          # lp again → routed again
    assert a.regs[0x17] & 0x07 == 0x07


def test_preset_resets_encoder_position():
    e, (a, b), fl, t = make()
    e.handle(mido.Message("control_change", control=16, value=65), "controls")   # norm now < 1
    e.select_preset(e.parts[0], "Bass")                                     # cutoff 600
    e.handle(mido.Message("control_change", control=16, value=1), "controls")
    assert a.get_cutoff() == round((600 / 2047 + 1 / 128) * 2047)


def test_load_presets_file(tmp_path):
    p = tmp_path / "p.toml"
    p.write_text('[[preset]]\nname="A"\nwaveform="saw"\n[[preset]]\nwaveform="tri"\n')
    pr = load_presets(p)
    assert [x["name"] for x in pr] == ["A", "preset 1"]
    p.write_text('[[preset]]\nname="A"\nwavefrom="saw"\n')
    with pytest.raises(ValueError, match="wavefrom"):
        load_presets(p)
    p.write_text('[[preset]]\nwaveform="square"\n')
    with pytest.raises(ValueError, match="square"):
        load_presets(p)
    assert load_presets(tmp_path / "missing.toml") == []


def test_shipped_presets_file_is_valid():
    pr = load_presets("presets.toml")
    assert len(pr) >= 8 and len({p["name"] for p in pr}) == len(pr)


# ---------------------------------------------------------------- reconnect

class Flaky(DryRunTransport):
    def __init__(self):
        super().__init__(echo=False)
        self.down = False

    def write(self, a, d):
        if self.down:
            raise TransportError("unreachable")
        super().write(a, d)


def test_flusher_reconnects_and_resyncs():
    t = Flaky()
    st = SidState()
    fl = Flusher(t, [st])
    fl.backoff_min = fl.backoff_max = 0.02
    reconnected = threading.Event()
    fl.on_reconnect = reconnected.set
    fl.start()
    try:
        t.down = True
        with fl.edit():
            st.set_volume(9)
        deadline = time.time() + 1
        while fl.connected and time.time() < deadline:
            time.sleep(0.005)
        assert not fl.connected and fl.errors >= 1
        with fl.edit():
            st.set_cutoff(100)                    # edits keep working while down
        t.down = False
        assert reconnected.wait(1)
        assert fl.connected
        # full resync: one write covering all 25 registers with the current values
        assert any(a == 0xD400 and len(d) == 25 for a, d in t.writes)
        full = next(d for a, d in t.writes if a == 0xD400 and len(d) == 25)
        assert full[0x18] == 9 and (full[0x16] << 3 | full[0x15]) == 100
    finally:
        fl.stop(final_flush=False)


# ---------------------------------------------------------------- SID detection

def test_detect_sids(monkeypatch):
    import bridge

    pages = {
        "SID Addressing": {"SID Socket 1 Address": "$D400", "SID Socket 2 Address": "$D420",
                           "UltiSID 1 Address": "$D400", "UltiSID 2 Address": "Unmapped"},
        "SID Sockets Configuration": {"SID Socket 1": "Enabled", "SID Socket 2": "Disabled",
                                      "SID Detected Socket 1": "6581", "SID Detected Socket 2": "8580"},
    }

    class R:
        def __init__(self, cat): self.cat = cat
        def raise_for_status(self): pass
        def json(self): return {self.cat: pages[self.cat], "errors": []}

    def fake_get(url, headers=None, timeout=None):
        from urllib.parse import unquote
        return R(unquote(url.rsplit("/", 1)[1]))

    monkeypatch.setattr(bridge.requests, "get", fake_get)
    assert bridge.detect_sids("x") == {0xD400: ["socket 1 (6581)", "UltiSID 1"]}


def test_instance_lock(tmp_path):
    import subprocess, sys
    lock = tmp_path / "x.lock"
    code = ("import sys, time; sys.path.insert(0, '.'); import bridge; "
            f"bridge.acquire_instance_lock(bridge.Path({str(lock)!r})); print('locked', flush=True); time.sleep(5)")
    holder = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    try:
        assert holder.stdout.readline().strip() == "locked"
        second = subprocess.run([sys.executable, "-c", code.replace("time.sleep(5)", "")],
                                capture_output=True, text=True, timeout=10)
        assert second.returncode != 0 and "already driving this C64U" in second.stderr
        assert str(holder.pid) in second.stderr
    finally:
        holder.kill(); holder.wait()


# ---------------------------------------------------------------- velocity

def vel_engine(vel="sustain", amount=1.0, **kw):
    e, sids, fl, t = make(**kw)
    e.set_velocity(vel, amount)
    return e, sids, fl, t


def test_velocity_sustain_per_voice():
    e, (a, b), fl, t = vel_engine()
    e.handle(mido.Message("note_on", note=60, velocity=127))
    e.handle(mido.Message("note_on", note=64, velocity=64))
    e.handle(mido.Message("note_on", note=67, velocity=1))
    assert [a.get_adsr(v)[2] for v in range(3)] == [10, 5, 0]   # 10 * (1, 64/127, 1/127)
    assert a.get_volume() == 15                                  # volume untouched


def test_velocity_sustain_written_before_gate():
    e, (a, b), fl, t = vel_engine()
    fl.flush(); t.writes.clear()
    e.handle(mido.Message("note_on", note=60, velocity=64))
    fl.flush()
    sr = [d for addr, d in t.writes if addr <= 0xD406 <= addr + len(d) - 1]
    gate = [i for i, (addr, d) in enumerate(t.writes) if addr <= 0xD404 <= addr + len(d) - 1
            and d[0xD404 - addr] & sid.GATE]
    first_sr = next(i for i, (addr, d) in enumerate(t.writes) if addr <= 0xD406 <= addr + len(d) - 1)
    assert sr and gate and first_sr <= gate[0]


def test_velocity_amount_and_off():
    e, (a, b), fl, t = vel_engine(amount=0.5)
    e.handle(mido.Message("note_on", note=60, velocity=1))
    assert a.get_adsr(0)[2] == 5                 # 10 * (1 - 0.5 * 126/127) = 5.04
    e2, (a2, _), _, _ = vel_engine(vel="off")
    e2.handle(mido.Message("note_on", note=60, velocity=1))
    assert a2.get_adsr(0)[2] == 10


def test_velocity_sustain_knob_keeps_scaling():
    e, (a, b), fl, t = vel_engine()
    e.handle(mido.Message("note_on", note=60, velocity=127))
    e.handle(mido.Message("note_on", note=64, velocity=64))
    e.set_param(e.parts[0], "sustain", 1.0)      # sustain 15
    assert a.get_adsr(0)[2] == 15 and a.get_adsr(1)[2] == 8     # 15 * 64/127 = 7.56


def test_velocity_volume_mode():
    e, (a, b), fl, t = vel_engine(vel="volume")
    e.handle(mido.Message("note_on", note=60, velocity=64))
    assert a.get_volume() == 8 and b.get_volume() == 8           # 15 * 64/127 = 7.56
    assert a.get_adsr(0)[2] == 10                                # sustain untouched
    e.panic()
    assert a.get_volume() == 0
    e.handle(mido.Message("note_on", note=60, velocity=127))
    assert a.get_volume() == 15


def test_velocity_split_parts_independent():
    e, (a, b), fl, t = vel_engine(vel="volume", layout="split")
    e.handle(mido.Message("note_on", note=60, velocity=127, channel=0))
    e.handle(mido.Message("note_on", note=60, velocity=32, channel=1))
    assert a.get_volume() == 15 and b.get_volume() == 4          # 15 * 32/127 = 3.78


def test_velocity_mono_retrigger_keeps_level():
    e, (a, b), fl, t = vel_engine(mode="mono")
    e.legato = False
    e.handle(mido.Message("note_on", note=60, velocity=40))
    e.handle(mido.Message("note_on", note=64, velocity=127))
    assert a.get_adsr(0)[2] == 10
    e.handle(mido.Message("note_off", note=64))  # falls back to 60: retriggers at 64's level
    assert a.get_adsr(0)[2] == 10 and a.get_freq(0) == sid.note_to_sid(60)


def test_velocity_mode_change_resets_levels():
    e, (a, b), fl, t = vel_engine()
    e.handle(mido.Message("note_on", note=60, velocity=1))
    assert a.get_adsr(0)[2] == 0
    e.set_velocity("volume", 1.0)
    assert a.get_adsr(0)[2] == 10 and a.get_volume() == 15
    with pytest.raises(ValueError):
        e.set_velocity("loud", 1.0)


def test_velocity_config_default_is_sustain():
    e, (a, b), fl, t = make()
    assert e.velocity_mode == "sustain" and e.velocity_amount == 1.0


# ---------------------------------------------------------------- pads

def pad(e, n, vel=100): e.handle(mido.Message("note_on", note=n, velocity=vel, channel=9))


def test_pad_cycles_filter_mode():
    e, (a, b), fl, t = make()
    assert e.patch["filter_mode"] == "lp"
    seen = []
    for _ in range(5):
        pad(e, 40)
        seen.append(e.patch["filter_mode"])
    assert seen == ["bp", "hp", "off", "lp", "bp"]
    assert a.regs[0x18] & 0x70 == sid.BP and b.regs[0x18] & 0x70 == sid.BP


def test_pad_off_unroutes_voices_and_back():
    e, (a, b), fl, t = make()
    for _ in range(3):
        pad(e, 40)                                  # -> off
    assert a.regs[0x17] & 0x07 == 0 and a.regs[0x18] & 0x70 == 0
    pad(e, 40)                                      # -> lp: voices routed again
    assert a.regs[0x17] & 0x07 == 0x07 and a.regs[0x18] & 0x70 == sid.LP


def test_pad_notes_never_play_and_others_ignored():
    e, (a, b), fl, t = make()
    pad(e, 37); pad(e, 40, vel=0)
    e.handle(mido.Message("note_off", note=40, channel=9))
    assert e.patch["filter_mode"] == "lp"
    assert all(not s.gated and s.note is None for s in e.slots)
    e.handle(mido.Message("note_on", note=40, velocity=100, channel=0))   # a key, same note
    assert e.patch["filter_mode"] == "lp" and a.get_control(0) & sid.GATE


def test_pad_follows_knob_target_in_split():
    e, (a, b), fl, t = make(layout="split", target=2)
    pad(e, 40)
    assert e.parts[1].patch["filter_mode"] == "bp" and e.parts[0].patch["filter_mode"] == "lp"
    assert b.regs[0x18] & 0x70 == sid.BP and a.regs[0x18] & 0x70 == sid.LP


def test_pad_unknown_mode_starts_list_and_config_checked():
    e, (a, b), fl, t = make()
    e.apply_patch(e.parts[0], {"filter_mode": "lp+hp"})
    pad(e, 40)
    assert e.patch["filter_mode"] == "lp"
    cfg = copy.deepcopy(DEFAULTS); cfg["engine"]["filter_cycle"] = ["lp", "nope"]
    with pytest.raises(ValueError):
        Engine(Flusher(DryRunTransport(echo=False), [SidState()]), cfg, [])


def test_generic_profile_has_no_pads():
    st = SidState()
    e = Engine(Flusher(DryRunTransport(echo=False), [st]), copy.deepcopy(DEFAULTS), [])
    assert e.profile == generic()
    e.init_chip()
    pad(e, 40)
    assert e.patch["filter_mode"] == "lp" and st.get_control(0) & sid.GATE   # plays as a note (omni)
