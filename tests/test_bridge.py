import copy

import mido
import pytest

import sid
from bridge import DEFAULTS, Engine, find_port
from profiles import load_profile
from sid import SidState, note_to_sid
from transport import DryRunTransport, Flusher


def make(mode="poly", hard_restart=False, nsids=1, **patch):
    cfg = copy.deepcopy(DEFAULTS)
    cfg["engine"].update(mode=mode, hard_restart=hard_restart)
    cfg["patch"].update(patch)
    t = DryRunTransport(echo=False)
    states = [SidState(0xD400 + 0x20 * i) for i in range(nsids)]
    fl = Flusher(t, states)
    e = Engine(fl, cfg, profile=load_profile("oxygen-49-mkv"))
    e.init_chip()
    fl.flush()
    t.writes.clear()
    return e, states[0], fl, t


def gate(st, v):
    return bool(st.get_control(v) & sid.GATE)


def on(e, n, vel=100): e.handle(mido.Message("note_on", note=n, velocity=vel))
def off(e, n): e.handle(mido.Message("note_off", note=n))
def cc(e, c, v): e.handle(mido.Message("control_change", control=c, value=v))


def test_init_patch():
    e, st, fl, t = make()
    assert st.get_volume() == 15
    assert st.get_cutoff() == 2047
    assert st.regs[0x17] & 0x07 == 0x07          # all voices filtered
    assert st.regs[0x18] & 0xF0 == sid.LP
    for v in range(3):
        assert st.get_control(v) == sid.PULSE
        assert st.get_adsr(v) == (0, 9, 10, 4)
        assert st.get_pulse_width(v) == 2048


def test_three_note_chord_uses_three_voices():
    e, st, fl, t = make()
    for n in (60, 64, 67):
        on(e, n)
    fl.flush()
    assert sorted(st.get_freq(v) for v in range(3)) == sorted(note_to_sid(n) for n in (60, 64, 67))
    assert all(gate(st, v) for v in range(3))


def test_note_off_clears_only_that_gate():
    e, st, fl, t = make()
    for n in (60, 64, 67):
        on(e, n)
    off(e, 64)
    v64 = next(s.voice for s in e.slots if s.note == 64)
    assert not gate(st, v64)
    assert sum(gate(st, v) for v in range(3)) == 2


def test_velocity_zero_is_note_off():
    e, st, fl, t = make()
    on(e, 60)
    on(e, 60, vel=0)
    assert not gate(st, 0)


def test_fourth_note_steals_oldest_with_gate_off_first():
    e, st, fl, t = make()
    for n in (60, 64, 67):
        on(e, n)
    fl.flush(); t.writes.clear()
    oldest = next(s for s in e.slots if s.note == 60).voice
    on(e, 72)
    fl.flush()
    assert st.get_freq(oldest) == note_to_sid(72) and gate(st, oldest)
    ctrl = 0xD400 + 7 * oldest + 4
    vals = [d[ctrl - a] for a, d in t.writes if a <= ctrl < a + len(d)]
    assert vals[0] & sid.GATE == 0 and vals[-1] & sid.GATE   # off, then on


def test_free_voice_preferred_over_steal():
    e, st, fl, t = make()
    on(e, 60); on(e, 64); on(e, 67)
    off(e, 64)
    v64 = next(s.voice for s in e.slots if s.note == 64)
    on(e, 70)
    assert st.get_freq(v64) == note_to_sid(70)
    assert sorted(s.note for s in e.slots) == [60, 67, 70]


def test_free_voice_released_longest_ago_first():
    e, st, fl, t = make()
    on(e, 60); on(e, 64); on(e, 67)
    off(e, 67); off(e, 60)               # 67 released first
    v67 = next(s.voice for s in e.slots if s.note == 67)
    on(e, 50)
    assert st.get_freq(v67) == note_to_sid(50)


def test_pitch_bend_retunes_all_sounding():
    e, st, fl, t = make()
    on(e, 60); on(e, 64)
    e.handle(mido.Message("pitchwheel", pitch=8191))
    for s in e.slots:
        if s.note is not None:
            assert st.get_freq(s.voice) == note_to_sid(s.note, 2.0)
    on(e, 67)  # new notes use the current bend
    v = next(s.voice for s in e.slots if s.note == 67)
    assert st.get_freq(v) == note_to_sid(67, 2.0)


def test_cc_map():
    e, st, fl, t = make()
    cc(e, 74, 0);   assert st.get_cutoff() == 0
    cc(e, 74, 127); assert st.get_cutoff() == 2047
    cc(e, 71, 127); assert st.regs[0x17] >> 4 == 15
    cc(e, 1, 64);   assert st.get_pulse_width(2) == round(64 / 127 * 4095)
    cc(e, 73, 127); cc(e, 75, 0); cc(e, 76, 64); cc(e, 72, 32)
    assert st.get_adsr(1) == (15, 0, 8, 4)
    cc(e, 7, 40);   assert st.get_volume() == 5
    cc(e, 99, 10)   # unmapped: ignored


def test_program_change_keeps_gate():
    e, st, fl, t = make()
    on(e, 60)
    e.handle(mido.Message("program_change", program=1))
    assert st.get_control(0) == sid.SAW | sid.GATE
    e.handle(mido.Message("program_change", program=10))  # wraps: 10 % 8 = 2 -> pulse
    assert st.get_control(0) & 0xF0 == sid.PULSE


def test_panic_cc123_and_unmute_on_next_note():
    e, st, fl, t = make()
    on(e, 60); on(e, 64)
    cc(e, 123, 0)
    assert not any(gate(st, v) for v in range(3))
    assert st.get_volume() == 0
    on(e, 67)
    assert st.get_volume() == 15


def test_sustain_pedal_holds_notes():
    e, st, fl, t = make()
    cc(e, 64, 127)
    on(e, 60); off(e, 60)
    assert gate(st, 0)
    cc(e, 64, 0)
    assert not gate(st, 0)


def test_channel_filter():
    e, st, fl, t = make()
    e.parts[0].channel = 2
    e.handle(mido.Message("note_on", note=60, velocity=100, channel=0))
    assert not any(gate(st, v) for v in range(3))
    e.handle(mido.Message("note_on", note=60, velocity=100, channel=1))
    assert gate(st, 0)


def test_mono_legato():
    e, st, fl, t = make(mode="mono")
    on(e, 60); fl.flush(); t.writes.clear()
    on(e, 64)                                   # legato: pitch only, no retrigger
    fl.flush()
    assert st.get_freq(0) == note_to_sid(64)
    assert all(not (a <= 0xD404 < a + len(d)) or d[0xD404 - a] & sid.GATE for a, d in t.writes)
    off(e, 64)                                  # back to held 60
    assert st.get_freq(0) == note_to_sid(60) and gate(st, 0)
    off(e, 60)
    assert not gate(st, 0)
    assert not gate(st, 1) and not gate(st, 2)  # other voices never used


def test_hard_restart_sequence():
    e, st, fl, t = make(hard_restart=True)
    on(e, 60)
    fl.flush()
    # batch 1: ADSR zeroed (gate off); later: ADSR restored, then gate on
    ad = [d[0xD405 - a] for a, d in t.writes if a <= 0xD405 < a + len(d)]
    assert ad[0] == 0x00 and ad[-1] == 0x09
    last_ctrl = [d[0xD404 - a] for a, d in t.writes if a <= 0xD404 < a + len(d)][-1]
    assert last_ctrl & sid.GATE
    assert t.writes[-1][0] == 0xD404          # gate is the very last write


def test_two_sids_six_voices():
    e, st, fl, t = make(nsids=2)
    for n in range(60, 66):
        on(e, n)
    assert len(e.slots) == 6
    assert all(gate(e.states[s.sid], s.voice) for s in e.slots)


def test_find_port():
    names = ["Oxygen 49 MKV USB MIDI", "Oxygen 49 MKV Mackie/HUI"]
    assert find_port("Oxygen 49 MKV USB MIDI", names) == names[0]
    assert find_port("mackie", names) == names[1]
    with pytest.raises(SystemExit):
        find_port("Korg", names)


def mk(e, c, v): e.handle(mido.Message("control_change", control=c, value=v), "controls")


def test_mackie_encoder_relative():
    e, st, fl, t = make(cutoff=1024)
    for _ in range(10):
        mk(e, 16, 1)                      # knob 1 = cutoff, +10 ticks
    assert st.get_cutoff() == round((1024 / 2047 + 10 / 128) * 2047)
    for _ in range(200):
        mk(e, 16, 65)                     # far past zero: clamps
    assert st.get_cutoff() == 0
    mk(e, 16, 3)                          # accelerated: 3 ticks at once
    assert st.get_cutoff() == round(3 / 128 * 2047)


def test_mackie_slow_encoder_on_coarse_param_accumulates():
    e, st, fl, t = make(resonance=0)
    for _ in range(4):
        mk(e, 17, 1)                      # 4/128 of 15 = 0.47 -> rounds to 0
    assert st.regs[0x17] >> 4 == 0
    for _ in range(5):
        mk(e, 17, 1)                      # 9/128*15 = 1.05 -> 1
    assert st.regs[0x17] >> 4 == 1


def test_mackie_fader_volume_and_unmapped():
    e, st, fl, t = make()
    e.handle(mido.Message("pitchwheel", channel=0, pitch=-8192), "controls")
    assert st.get_volume() == 0
    e.handle(mido.Message("pitchwheel", channel=0, pitch=8191), "controls")
    assert st.get_volume() == 15
    e.handle(mido.Message("pitchwheel", channel=5, pitch=0), "controls")   # unmapped fader
    mk(e, 40, 1)                                                      # not an encoder
    assert st.get_volume() == 15


def test_cc_and_encoder_share_state():
    e, st, fl, t = make()
    cc(e, 74, 0)
    mk(e, 16, 1)
    assert st.get_cutoff() == round(1 / 128 * 2047)


def test_dma_default_falls_back_to_http_when_service_is_off():
    import bridge
    from transport import HttpTransport
    t = bridge.open_transport("127.0.0.1", "", "dma")    # nothing listens on port 64 here
    assert isinstance(t, HttpTransport)
    assert isinstance(bridge.open_transport("127.0.0.1", "", "http"), HttpTransport)


def test_config_local_overrides(tmp_path):
    from engine import load_config
    (tmp_path / "config.toml").write_text('[device]\nhost = ""\nvideo = "PAL"\n')
    assert load_config(tmp_path / "config.toml")["device"]["host"] == ""
    (tmp_path / "config.local.toml").write_text('[device]\nhost = "10.0.0.5"\n')
    dev = load_config(tmp_path / "config.toml")["device"]
    assert dev["host"] == "10.0.0.5" and dev["video"] == "PAL"
