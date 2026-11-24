import pytest

import sid
from sid import (NTSC_CLOCK, PAL_CLOCK, SidState, bend_to_semitones,
                 hz_to_sid, merge_ranges, note_to_hz, note_to_sid)


# ---------------------------------------------------------------- pitch

def test_note_to_hz():
    assert note_to_hz(69) == pytest.approx(440.0)
    assert note_to_hz(81) == pytest.approx(880.0)
    assert note_to_hz(60) == pytest.approx(261.6256, rel=1e-6)
    assert note_to_hz(69, bend_semitones=12) == pytest.approx(880.0)
    assert note_to_hz(69, bend_semitones=-2) == pytest.approx(391.9954, rel=1e-6)


@pytest.mark.parametrize("note,expected", [
    (69, 0x1D45),   # A4  — matches PAL tables and Phase 0 hardware beep
    (60, 0x1167),   # C4  — standard PAL table value
    (57, 0x0EA2),   # A3
    (81, 0x3A89),   # A5  (14984.99 — not simply 2 × rounded A4)
])
def test_pal_table(note, expected):
    assert note_to_sid(note, clock=PAL_CLOCK) == expected


def test_ntsc_a4():
    assert note_to_sid(69, clock=NTSC_CLOCK) == round(440 * 16777216 / 1022727)
    assert note_to_sid(69, clock=NTSC_CLOCK) < note_to_sid(69, clock=PAL_CLOCK)


def test_clamping():
    assert hz_to_sid(-5) == 0
    assert hz_to_sid(0) == 0
    assert note_to_sid(127) == 0xFFFF   # ~12.5 kHz is above the PAL SID max (~3.85 kHz)
    assert note_to_sid(0) > 0


def test_bend_to_semitones():
    assert bend_to_semitones(0) == 0
    assert bend_to_semitones(8191) == pytest.approx(2.0)
    assert bend_to_semitones(-8192) == pytest.approx(-2.0)
    assert bend_to_semitones(4096, bend_range=12) == pytest.approx(6.0, rel=1e-3)


# ---------------------------------------------------------------- dirty ranges

def test_merge_ranges_exact():
    assert merge_ranges([]) == []
    assert merge_ranges([4]) == [(4, 4)]
    assert merge_ranges([0, 1, 2, 4, 5, 0x18]) == [(0, 2), (4, 5), (0x18, 0x18)]
    assert merge_ranges([5, 1, 0, 1]) == [(0, 1), (5, 5)]


def test_merge_ranges_gap():
    assert merge_ranges([0, 2], max_gap=1) == [(0, 2)]
    assert merge_ranges([0, 3], max_gap=1) == [(0, 0), (3, 3)]
    assert merge_ranges([0, 7, 0x18], max_gap=100) == [(0, 0x18)]


def test_dirty_only_on_change():
    s = SidState()
    s.set_reg(0x18, 0)
    assert s.dirty == set()
    s.set_reg(0x18, 0, force=True)
    assert s.dirty == {0x18}
    assert s.take_dirty() == {0x18}
    assert s.dirty == set()


def test_chunks_use_shadow_values():
    s = SidState(base=0xD400)
    s.set_freq(0, 0x1D45)
    s.set_volume(15)
    d = s.take_dirty()
    assert d == {0, 1, 0x18}
    assert s.chunks(d, max_gap=0) == [(0xD400, b"\x45\x1D"), (0xD418, b"\x0F")]
    one = s.chunks(d)  # default: one contiguous write
    assert one == [(0xD400, bytes(s.regs))]


def test_chunks_second_sid_base():
    s = SidState(base=0xD420)
    s.set_control(2, 0x11)
    assert s.chunks(s.take_dirty()) == [(0xD420 + 14 + 4, b"\x11")]


# ---------------------------------------------------------------- packing

def test_voice_offsets_and_freq():
    s = SidState()
    for v in range(3):
        s.set_freq(v, 0xABCD + v)
        assert s.regs[7 * v] == (0xCD + v) & 0xFF
        assert s.regs[7 * v + 1] == 0xAB
        assert s.get_freq(v) == 0xABCD + v
    with pytest.raises(IndexError):
        s.set_freq(3, 0)


def test_pulse_width_12bit():
    s = SidState()
    s.set_pulse_width(1, 0x0800)
    assert s.regs[9] == 0x00 and s.regs[10] == 0x08
    s.set_pulse_width(1, 0x1FFF)  # clamp
    assert s.get_pulse_width(1) == 0xFFF


def test_gate_and_waveform_independent():
    s = SidState()
    s.set_waveform(0, sid.PULSE)
    s.set_gate(0, True)
    assert s.get_control(0) == 0x41
    s.set_waveform(0, sid.SAW | sid.RING)
    assert s.get_control(0) == 0x25
    s.set_gate(0, False)
    assert s.get_control(0) == 0x24
    s.set_waveform(0, sid.TRI | sid.GATE)  # GATE bit ignored by set_waveform
    assert s.get_control(0) == 0x10


def test_adsr_partial_updates():
    s = SidState()
    s.set_adsr(2, a=0, d=9, s=0, r=0)
    assert s.regs[19] == 0x09 and s.regs[20] == 0x00
    s.set_adsr(2, s=12)
    assert s.regs[20] == 0xC0
    s.set_adsr(2, a=15, r=3)
    assert s.get_adsr(2) == (15, 9, 12, 3)


def test_cutoff_11bit():
    s = SidState()
    s.set_cutoff(0x7FF)
    assert s.regs[0x15] == 0x07 and s.regs[0x16] == 0xFF
    s.set_cutoff(0x40B)
    assert s.regs[0x15] == 0x03 and s.regs[0x16] == 0x81
    assert s.get_cutoff() == 0x40B
    s.set_cutoff(5000)
    assert s.get_cutoff() == 0x7FF


def test_resonance_routing_mode_volume():
    s = SidState()
    s.set_resonance(0xA)
    s.set_filter_route(0, True)
    s.set_filter_route(2, True)
    s.set_filter_ext(True)
    assert s.regs[0x17] == 0xAD
    s.set_filter_route(0, False)
    assert s.regs[0x17] == 0xAC
    s.set_filter_mode(sid.LP | sid.VOICE3_OFF)
    s.set_volume(15)
    assert s.regs[0x18] == 0x9F
    s.set_volume(99)
    assert s.get_volume() == 15
    s.set_filter_mode(sid.HP)
    assert s.regs[0x18] == 0x4F
