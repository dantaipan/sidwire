import copy

import mido
import pytest

import sid
from engine import DEFAULTS, Engine
from profiles import (PROFILES_DIR, Control, ProfileError, generic, list_profiles, load_profile,
                      parse_profile)
from sid import SidState
from transport import DryRunTransport, Flusher

PRESETS = [{"name": "A", "waveform": "pulse"}, {"name": "B", "waveform": "saw"}, {"name": "C", "waveform": "tri"}]


def engine(profile_text: str, nsids=1, **eng):
    cfg = copy.deepcopy(DEFAULTS)
    cfg["engine"].update(eng)
    st = [SidState(0xD400 + 0x20 * i) for i in range(nsids)]
    e = Engine(Flusher(DryRunTransport(echo=False), st), cfg, [dict(p) for p in PRESETS],
               parse_profile(profile_text, "test"))
    e.init_chip()
    return e, st


# ---------------------------------------------------------------- files

def test_shipped_profiles_parse():
    ids = {i: name for i, name, _ in list_profiles()}
    assert ids["generic"] == "Generic MIDI keyboard"
    assert ids["oxygen-49-mkv"].startswith("M-Audio Oxygen 49 MKV")
    assert all(not n.startswith("(invalid") for n in ids.values())


def test_generic_matches_the_classic_cc_map():
    g = generic()
    ccs = {c.number: c.param or c.action for c in g.controls if c.kind == "cc"}
    assert ccs == {1: "pulse_width", 74: "cutoff", 71: "resonance", 73: "attack", 75: "decay",
                   76: "sustain", 72: "release", 7: "volume", 64: "sustain_pedal"}
    assert g.channel == 0 and g.bend_range == 2 and g.ignore_channels == () and g.notes_port == ""


def test_oxygen_profile_contents():
    o = load_profile("oxygen-49-mkv")
    knobs = [c for c in o.controls if c.port == "controls" and c.kind == "cc"]
    assert [c.number for c in knobs] == list(range(16, 24))
    assert all(c.mode == "relative-signed" for c in knobs)
    assert o.ignore_channels == (10,)
    pad1 = [c for c in o.controls if c.kind == "note"]
    assert pad1 == [Control("note", 40, "notes", 10, action="filter_mode_next")]


def test_load_by_path_and_user_dir_wins(tmp_path):
    f = tmp_path / "generic.toml"
    f.write_text('name = "My generic"\n')
    assert load_profile("generic", [tmp_path]).name == "My generic"      # user dir first
    assert load_profile(str(f)).name == "My generic"                      # by path
    assert load_profile("generic").name == "Generic MIDI keyboard"
    with pytest.raises(ProfileError, match="available"):
        load_profile("nope")


def test_list_marks_invalid_files(tmp_path):
    (tmp_path / "broken.toml").write_text('name = "x"\n[[control]]\ncc = 300\nparam = "cutoff"\n')
    names = {i: n for i, n, _ in list_profiles([tmp_path])}
    assert names["broken"].startswith("(invalid")


@pytest.mark.parametrize("text, msg", [
    ('description = "no name"', "needs a name"),
    ('name = "x"\nfoo = 1', "unknown key"),
    ('name = "x"\n[keys]\nchanel = 1', "unknown key"),
    ('name = "x"\n[keys]\nchannel = 17', "0-16"),
    ('name = "x"\n[keys]\nignore_channels = 10', "must be a list"),
    ('name = "x"\n[[control]]\nparam = "cutoff"', "exactly one of cc"),
    ('name = "x"\n[[control]]\ncc = 1\nnote = 2\nparam = "cutoff"', "exactly one of cc"),
    ('name = "x"\n[[control]]\ncc = 128\nparam = "cutoff"', "0-127"),
    ('name = "x"\n[[control]]\ncc = 1', "exactly one of param"),
    ('name = "x"\n[[control]]\ncc = 1\nparam = "wobble"', "unknown param"),
    ('name = "x"\n[[control]]\ncc = 1\naction = "explode"', "unknown action"),
    ('name = "x"\n[[control]]\ncc = 1\nparam = "cutoff"\nmode = "spin"', "unknown mode"),
    ('name = "x"\n[[control]]\ncc = 1\nparam = "cutoff"\nport = "usb"', "port must be"),
    ('name = "x"\n[[control]]\nnote = 40\nparam = "cutoff"', "triggers an action"),
    ('name = "x"\n[[control]]\nnote = 40\naction = "sustain_pedal"', "needs a CC"),
    ('name = "x"\n[[control]]\npitchbend = true\naction = "panic"', "sets a param"),
    ('name = "x"\n[[control]]\nnote = 40\naction = "panic"\nmode = "relative-twos"', "relative modes"),
    ('name = "x"\n[[control]]\ncc = 1\nparam = "cutoff"\nsteps = 10', "only applies to relative"),
    ('name = "x"\n[control]\ncc = 1\nparam = "cutoff"', "double brackets"),
    ('name = "x"\n[[control]\n', "test"),   # TOML syntax error names the file
])
def test_profile_errors_are_clear(text, msg):
    with pytest.raises(ProfileError, match=msg):
        parse_profile(text, "test")


# ---------------------------------------------------------------- behaviour

def test_relative_decoding():
    signed = Control("cc", 1, mode="relative-signed")
    twos = Control("cc", 1, mode="relative-twos")
    offset = Control("cc", 1, mode="relative-offset")
    assert [signed.decode_relative(v) for v in (1, 3, 65, 67, 0, 64)] == [1, 3, -1, -3, 0, 0]
    assert [twos.decode_relative(v) for v in (1, 63, 127, 65, 0)] == [1, 63, -1, -63, 0]
    assert [offset.decode_relative(v) for v in (65, 64, 63, 0, 127)] == [1, 0, -1, -64, 63]


def test_relative_twos_knob_on_notes_port():
    e, (st,) = engine('name = "t"\n[[control]]\ncc = 10\nmode = "relative-twos"\nsteps = 15\nparam = "release"')
    assert e.patch["release"] == 4
    e.handle(mido.Message("control_change", control=10, value=1))
    assert e.patch["release"] == 5
    e.handle(mido.Message("control_change", control=10, value=126))   # -2
    assert e.patch["release"] == 3 and st.get_adsr(0)[3] == 3


def test_channel_specific_control_and_any_channel():
    e, _ = engine('name = "t"\n[[control]]\ncc = 20\nchannel = 2\nparam = "cutoff"\n'
                  '[[control]]\ncc = 21\nparam = "resonance"')
    e.handle(mido.Message("control_change", control=20, value=0, channel=0))
    assert e.patch["cutoff"] == 2047                 # ch1: not this control
    e.handle(mido.Message("control_change", control=20, value=0, channel=1))
    assert e.patch["cutoff"] == 0
    e.handle(mido.Message("control_change", control=21, value=127, channel=7))
    assert e.patch["resonance"] == 15


def test_keys_channel_and_ignore_channels():
    e, (st,) = engine('name = "t"\n[keys]\nchannel = 3\nignore_channels = [3]')
    e.handle(mido.Message("note_on", note=60, velocity=100, channel=2))
    assert not st.get_control(0) & sid.GATE          # ignored wins over the keys channel
    e2, (st2,) = engine('name = "t"\n[keys]\nchannel = 3')
    e2.handle(mido.Message("note_on", note=60, velocity=100, channel=0))
    assert not st2.get_control(0) & sid.GATE
    e2.handle(mido.Message("note_on", note=60, velocity=100, channel=2))
    assert st2.get_control(0) & sid.GATE


def test_bend_range_from_profile():
    e, (st,) = engine('name = "t"\n[keys]\nbend_range = 12')
    e.handle(mido.Message("note_on", note=60, velocity=100))
    e.handle(mido.Message("pitchwheel", pitch=8191))
    assert st.get_freq(0) == sid.note_to_sid(72)


def test_cc_button_actions():
    e, (st,) = engine('name = "t"\n'
                      '[[control]]\ncc = 80\naction = "preset_next"\n'
                      '[[control]]\ncc = 81\naction = "preset_prev"\n'
                      '[[control]]\ncc = 82\naction = "waveform_next"\n'
                      '[[control]]\ncc = 83\naction = "panic"\n'
                      '[[control]]\ncc = 84\naction = "filter_mode_next"')
    cc = lambda n, v=127: e.handle(mido.Message("control_change", control=n, value=v))  # noqa: E731
    cc(80); assert e.patch["name"] == "A"
    cc(80, 0); assert e.patch["name"] == "A"          # release: nothing
    cc(80); assert e.patch["name"] == "B"
    cc(81); cc(81); assert e.patch["name"] == "C"     # wraps backwards
    cc(82); assert e.patch["waveform"] == "saw"       # tri -> next in program order
    cc(84); assert e.patch["filter_mode"] == "bp"
    e.handle(mido.Message("note_on", note=60, velocity=100))
    cc(83); assert not st.get_control(0) & sid.GATE and st.get_volume() == 0


def test_preset_prev_from_nothing_goes_to_last():
    e, _ = engine('name = "t"\n[[control]]\ncc = 81\naction = "preset_prev"')
    e.handle(mido.Message("control_change", control=81, value=127))
    assert e.patch["name"] == "C"


def test_controls_port_buttons_faders_and_isolation():
    e, (st,) = engine('name = "t"\n'
                      '[[control]]\nport = "controls"\nnote = 86\naction = "preset_next"\n'
                      '[[control]]\nport = "controls"\ncc = 12\nparam = "volume"\n'
                      '[[control]]\nport = "controls"\npitchbend = true\nchannel = 2\nparam = "cutoff"\n'
                      '[[control]]\ncc = 12\nparam = "resonance"')
    e.handle(mido.Message("note_on", note=86, velocity=127), "controls")
    assert e.patch["name"] == "A"
    e.handle(mido.Message("note_on", note=60, velocity=127), "controls")   # unmapped: never plays
    assert not st.get_control(0) & sid.GATE
    e.handle(mido.Message("control_change", control=12, value=0), "controls")
    assert e.patch["volume"] == 0 and e.patch["resonance"] == 0           # notes-port CC12 untouched
    e.handle(mido.Message("pitchwheel", channel=1, pitch=-8192), "controls")
    assert e.patch["cutoff"] == 0
    e.handle(mido.Message("pitchwheel", channel=0, pitch=8191), "controls")   # other channel
    assert e.patch["cutoff"] == 0
    e.handle(mido.Message("program_change", program=2), "controls")       # ignored on controls
    assert e.patch["name"] == "A"


def test_a_key_as_a_button_never_plays():
    e, (st,) = engine('name = "t"\n[[control]]\nnote = 48\naction = "filter_mode_next"')
    e.handle(mido.Message("note_on", note=48, velocity=100))
    e.handle(mido.Message("note_off", note=48))
    assert e.patch["filter_mode"] == "bp" and not st.get_control(0) & sid.GATE
    e.handle(mido.Message("note_on", note=49, velocity=100))
    assert st.get_control(0) & sid.GATE


def test_split_layout_targets():
    e, (a, b) = engine('name = "t"\n[[control]]\ncc = 74\nparam = "cutoff"\n'
                       '[[control]]\nport = "controls"\ncc = 12\nparam = "volume"\n'
                       '[[control]]\ncc = 84\naction = "filter_mode_next"',
                       nsids=2, layout="split", knobs_target=2)
    e.handle(mido.Message("control_change", control=74, value=0, channel=0))    # notes port: ch1 -> part 1
    assert e.parts[0].patch["cutoff"] == 0 and e.parts[1].patch["cutoff"] == 2047
    e.handle(mido.Message("control_change", control=12, value=0), "controls")   # controls port: target part 2
    assert e.parts[1].patch["volume"] == 0 and e.parts[0].patch["volume"] == 15
    e.handle(mido.Message("control_change", control=84, value=127, channel=0))  # actions: target part
    assert e.parts[1].patch["filter_mode"] == "bp" and e.parts[0].patch["filter_mode"] == "lp"


def test_profile_shown_in_describe():
    assert Control("cc", 16, "controls", 0, "cutoff", mode="relative-signed").describe() == \
        "CC16 (controls port) -> cutoff [relative-signed]"
    assert Control("note", 40, channel=10, action="filter_mode_next").describe() == "note 40 ch10 -> filter_mode_next"


def test_profiles_dir_is_in_repo():
    assert (PROFILES_DIR / "generic.toml").exists()
