import copy

import mido
import pytest

import sid
from engine import ARP_RATES, DEFAULTS, VOICE_MODES, Engine
from profiles import load_profile
from sid import SidState, note_to_sid
from transport import DryRunTransport, Flusher


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def make(mode="poly", nsids=2, layout="poly", **arp):
    cfg = copy.deepcopy(DEFAULTS)
    cfg["engine"].update(mode=mode, layout=layout)
    cfg["arp"].update(arp)
    states = [SidState(0xD400 + 0x20 * i) for i in range(nsids)]
    t = DryRunTransport(echo=False)
    e = Engine(Flusher(t, states), cfg, [], load_profile("generic"))
    clock = Clock()
    e.now = clock
    e.init_chip()
    return e, states, clock


def on(e, n, vel=100, ch=0): e.handle(mido.Message("note_on", note=n, velocity=vel, channel=ch))
def off(e, n, ch=0): e.handle(mido.Message("note_off", note=n, channel=ch))
def gated(states): return [bool(st.get_control(v) & sid.GATE) for st in states for v in range(3)]
def freqs(states): return [st.get_freq(v) for st in states for v in range(3)]


def test_modes_listed_and_checked():
    assert VOICE_MODES == ("poly", "mono", "unison", "octave", "fifth", "chord", "arp")
    e, _, _ = make()
    with pytest.raises(ValueError):
        e.set_voice_mode("cluster")
    cfg = copy.deepcopy(DEFAULTS); cfg["engine"]["mode"] = "drone"
    with pytest.raises(ValueError):
        Engine(Flusher(DryRunTransport(echo=False), [SidState()]), cfg, [])


def test_octave_fifth_chord_stacks():
    for mode, intervals in (("octave", (0, 12)), ("fifth", (0, 7)), ("chord", (0, 7, 12))):
        e, st, _ = make(mode)
        on(e, 48)
        sounding = sorted(f for f, g in zip(freqs(st), gated(st)) if g)
        assert sounding == sorted(note_to_sid(48 + i) for i in intervals), mode
        off(e, 48)
        assert not any(gated(st)), mode


def test_chord_polyphony_is_voices_divided_by_stack():
    e, st, _ = make("chord")       # 6 voices / 3 = 2 chords
    on(e, 48); on(e, 60)
    assert sum(gated(st)) == 6
    on(e, 72)                      # steals the oldest chord's voices
    assert sum(gated(st)) == 6
    assert note_to_sid(72 + 12) in freqs(st)


def test_unison_all_voices_detuned_and_legato():
    e, st, _ = make("unison")
    on(e, 57)
    assert all(gated(st))
    f = freqs(st)
    assert f[0] == note_to_sid(57)                      # the first voice is in tune
    assert f[1] == note_to_sid(57, 0.08) and f[2] == note_to_sid(57, -0.08)   # +/- 8 cents
    on(e, 60)                                           # legato: retuned, not retriggered
    assert freqs(st)[0] == note_to_sid(60) and all(gated(st))
    off(e, 60)
    assert freqs(st)[0] == note_to_sid(57)              # back to the held note
    off(e, 57)
    assert not any(gated(st))


def test_bend_keeps_stack_intervals():
    e, st, _ = make("octave")
    on(e, 48)
    e.handle(mido.Message("pitchwheel", pitch=8191))
    sounding = sorted(f for f, g in zip(freqs(st), gated(st)) if g)
    assert sounding == sorted([note_to_sid(48, 2.0), note_to_sid(48, 14.0)])


def test_arp_up_steps_with_time():
    e, st, clock = make("arp", rate="1/16", bpm=120, gate=0.5)   # a step every 125 ms
    on(e, 60); on(e, 64); on(e, 67)
    played = [st[0].get_freq(0)]
    for _ in range(4):
        clock.t += 0.125
        e.service()
        played.append(st[0].get_freq(0))
    assert played == [note_to_sid(n) for n in (60, 64, 67, 60, 64)]
    assert sum(gated(st)) <= 1                          # one voice


def test_arp_gate_and_release():
    e, st, clock = make("arp", rate="1/8", bpm=120, gate=0.5)    # step 250 ms, gate off at 125 ms
    on(e, 60)
    assert gated(st)[0]
    clock.t += 0.13
    e.service()
    assert not gated(st)[0]
    clock.t += 0.12
    e.service()
    assert gated(st)[0]                                 # next step: on again
    off(e, 60)
    assert not any(gated(st))
    clock.t += 1.0
    e.service()
    assert not any(gated(st))                           # stopped


@pytest.mark.parametrize("pattern, pressed, expect", [
    # the first step plays the only note held then (index 0); later steps walk the full pattern
    ("up", (60, 64, 67), [60, 64, 67, 60]),
    ("down", (60, 64, 67), [60, 64, 60, 67]),          # down = 67 64 60: index 1, 2, 0
    ("up-down", (60, 64, 67), [60, 64, 67, 64, 60, 64]),
    ("as played", (64, 60, 67), [64, 60, 67, 64]),
])
def test_arp_patterns(pattern, pressed, expect):
    e, st, clock = make("arp", pattern=pattern, bpm=120, rate="1/16")
    for n in pressed:
        on(e, n)
    played = [st[0].get_freq(0)]
    for _ in range(len(expect) - 1):
        clock.t += 0.125
        e.service()
        played.append(st[0].get_freq(0))
    assert played == [note_to_sid(n) for n in expect]


def test_arp_octaves_and_random_is_repeatable():
    e, st, clock = make("arp", octaves=2, bpm=120, rate="1/16")
    on(e, 60)
    seq = [st[0].get_freq(0)]
    for _ in range(3):
        clock.t += 0.125
        e.service()
        seq.append(st[0].get_freq(0))
    assert seq == [note_to_sid(n) for n in (60, 72, 60, 72)]
    runs = []
    for _ in range(2):
        e, st, clock = make("arp", pattern="random", bpm=120, rate="1/16")
        for n in (60, 62, 64, 65):
            on(e, n)
        r = []
        for _ in range(8):
            clock.t += 0.125
            e.service()
            r.append(st[0].get_freq(0))
        runs.append(r)
    assert runs[0] == runs[1] and len(set(runs[0])) > 1


def test_arp_follows_midi_clock():
    e, st, clock = make("arp", rate="1/16", bpm=60, gate=0.5)    # 6 pulses per step
    e.handle(mido.Message("start"))
    on(e, 60); on(e, 64)
    first = st[0].get_freq(0)
    for i in range(6):
        clock.t += 0.02
        e.handle(mido.Message("clock"))
        e.service()                                     # time stepping waits while clocked
        if i == 2:
            assert not gated(st)[0]                     # gate off after 3 pulses
    assert st[0].get_freq(0) != first and gated(st)[0]  # stepped on the 6th pulse
    assert ARP_RATES["1/16"] == 4


def test_pedal_holds_the_arp_and_mode_change_releases():
    e, st, clock = make("arp", bpm=120, rate="1/16")
    e.handle(mido.Message("control_change", control=64, value=127))
    on(e, 60); off(e, 60)
    clock.t += 0.125
    e.service()
    assert e.parts[0].arp_running
    e.handle(mido.Message("control_change", control=64, value=0))
    assert not e.parts[0].arp_running and not any(gated(st))
    on(e, 60)
    e.set_voice_mode("poly")
    assert not any(gated(st)) and not e.parts[0].arp_notes


def test_voice_mode_next_action_and_split_parts():
    from profiles import parse_profile
    cfg = copy.deepcopy(DEFAULTS)
    e = Engine(Flusher(DryRunTransport(echo=False), [SidState()]), cfg, [],
               parse_profile('name = "t"\n[[control]]\ncc = 85\naction = "voice_mode_next"'))
    e.init_chip()
    seen = []
    for _ in range(8):
        e.handle(mido.Message("control_change", control=85, value=127))
        seen.append(e.voice_mode)
    assert seen == ["mono", "unison", "octave", "fifth", "chord", "arp", "poly", "mono"]
    e2, st, _ = make("fifth", layout="split")
    on(e2, 60, ch=0); on(e2, 62, ch=1)
    assert sum(gated(st[:1])) == 2 and sum(gated(st[1:])) == 2
