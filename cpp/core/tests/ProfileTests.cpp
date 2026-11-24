// Controller profiles: parsing rules (same as profiles.py) and the shipped files.
#include "msid/Engine.h"
#include "msid/Profile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace msid;
using Catch::Matchers::ContainsSubstring;

TEST_CASE ("profile: built-in generic profile is identical to profiles/generic.toml", "[profile]")
{
    auto fromFile = loadProfile (std::string (MSID_PROFILES_DIR) + "/generic.toml");
    CHECK (fromFile == genericProfile());
    CHECK (fromFile.controls.size() == 9);
}

TEST_CASE ("profile: the Oxygen profile", "[profile]")
{
    auto o = loadProfile (std::string (MSID_PROFILES_DIR) + "/oxygen-49-mkv.toml");
    CHECK (o.name == "M-Audio Oxygen 49 MKV (DAW mode)");
    CHECK (o.notesPort == "Oxygen 49 MKV USB MIDI");
    CHECK (o.controlsPort == "Oxygen 49 MKV Mackie/HUI");
    CHECK (o.ignoreChannels == std::vector<int> { 10 });
    int knobs = 0;
    for (auto& c : o.controls)
        if (c.port == Port::Controls && c.kind == Control::Kind::CC && c.mode == "relative-signed")
            ++knobs;
    CHECK (knobs == 8);
    auto& pad = o.controls.back();
    CHECK (pad.kind == Control::Kind::Note);
    CHECK (pad.number == 40);
    CHECK (pad.channel == 10);
    CHECK (pad.action == "filter_mode_next");
    CHECK (pad.describe() == "note 40 ch10 -> filter_mode_next");
}

TEST_CASE ("profile: the test profile with every kind of control parses", "[profile]")
{
    auto t = loadProfile (std::string (MSID_TEST_PROFILES_DIR) + "/test-all.toml");
    CHECK (t.bendRange == 7.0);
    CHECK (t.ignoreChannels == std::vector<int> { 9, 10 });
    CHECK (t.controls.size() == 17);
}

TEST_CASE ("profile: relative knob decoding", "[profile]")
{
    Control s, t, o;
    s.mode = "relative-signed";
    t.mode = "relative-twos";
    o.mode = "relative-offset";
    CHECK (s.decodeRelative (1) == 1);
    CHECK (s.decodeRelative (65) == -1);
    CHECK (s.decodeRelative (67) == -3);
    CHECK (t.decodeRelative (63) == 63);
    CHECK (t.decodeRelative (127) == -1);
    CHECK (t.decodeRelative (65) == -63);
    CHECK (o.decodeRelative (65) == 1);
    CHECK (o.decodeRelative (63) == -1);
    CHECK (o.decodeRelative (64) == 0);
}

TEST_CASE ("profile: errors are clear and name the file", "[profile]")
{
    struct Case { const char* text; const char* msg; };
    const Case cases[] = {
        { "description = \"no name\"", "needs a name" },
        { "name = \"x\"\nfoo = 1", "unknown key" },
        { "name = \"x\"\n[keys]\nchanel = 1", "unknown key" },
        { "name = \"x\"\n[keys]\nchannel = 17", "0-16" },
        { "name = \"x\"\n[keys]\nignore_channels = 10", "must be a list" },
        { "name = \"x\"\n[[control]]\nparam = \"cutoff\"", "exactly one of cc" },
        { "name = \"x\"\n[[control]]\ncc = 1\nnote = 2\nparam = \"cutoff\"", "exactly one of cc" },
        { "name = \"x\"\n[[control]]\ncc = 128\nparam = \"cutoff\"", "0-127" },
        { "name = \"x\"\n[[control]]\ncc = 1", "exactly one of param" },
        { "name = \"x\"\n[[control]]\ncc = 1\nparam = \"wobble\"", "unknown param" },
        { "name = \"x\"\n[[control]]\ncc = 1\naction = \"explode\"", "unknown action" },
        { "name = \"x\"\n[[control]]\ncc = 1\nparam = \"cutoff\"\nmode = \"spin\"", "unknown mode" },
        { "name = \"x\"\n[[control]]\ncc = 1\nparam = \"cutoff\"\nport = \"usb\"", "port must be" },
        { "name = \"x\"\n[[control]]\nnote = 40\nparam = \"cutoff\"", "triggers an action" },
        { "name = \"x\"\n[[control]]\nnote = 40\naction = \"sustain_pedal\"", "needs a CC" },
        { "name = \"x\"\n[[control]]\npitchbend = true\naction = \"panic\"", "sets a param" },
        { "name = \"x\"\n[[control]]\nnote = 40\naction = \"panic\"\nmode = \"relative-twos\"", "relative modes" },
        { "name = \"x\"\n[[control]]\ncc = 1\nparam = \"cutoff\"\nsteps = 10", "only applies to relative" },
        { "name = \"x\"\n[control]\ncc = 1\nparam = \"cutoff\"", "double brackets" },
        { "name = \"x\"\n[[control]\n", "test" },
        { "name = \"x\"\nname = \"y\"", "set twice" },
    };
    for (auto& c : cases)
    {
        INFO (c.text);
        CHECK_THROWS_WITH (parseProfile (c.text, "test"), ContainsSubstring (c.msg));
    }
}

TEST_CASE ("profile: missing file", "[profile]")
{
    CHECK_THROWS_AS (loadProfile ("/nonexistent/x.toml"), ProfileError);
}

TEST_CASE ("profile: written TOML reads back to the same profile", "[profile]")
{
    for (auto path : { std::string (MSID_PROFILES_DIR) + "/generic.toml", std::string (MSID_PROFILES_DIR) + "/oxygen-49-mkv.toml",
                       std::string (MSID_TEST_PROFILES_DIR) + "/test-all.toml" })
    {
        INFO (path);
        auto p = loadProfile (path);
        p.description = "with \"quotes\" and \\ backslash";
        auto text = profileToToml (p, "Written by a test\nsecond line");
        CHECK (text.rfind ("# Written by a test\n# second line\n", 0) == 0);
        CHECK (parseProfile (text, "roundtrip") == p);
    }
}

TEST_CASE ("profile: short trigger labels", "[profile]")
{
    Control c;
    c.number = 74;
    CHECK (shortTrigger (c) == "CC74");
    c.port = Port::Controls;
    c.number = 16;
    CHECK (shortTrigger (c) == "CC16 ctl");
    c.kind = Control::Kind::PitchBend;
    c.channel = 1;
    CHECK (shortTrigger (c) == "Bend ch1 ctl");
    c.kind = Control::Kind::Note;
    c.port = Port::Notes;
    c.number = 40;
    c.channel = 10;
    CHECK (shortTrigger (c) == "Note 40 ch10");
}

TEST_CASE ("engine: setProfile swaps the mapping while running", "[profile]")
{
    DryRunTransport t;
    std::vector<SidState> states;
    states.emplace_back ((uint16_t) 0xD400);
    Flusher fl (t, std::move (states));
    Engine e (fl, EngineConfig {});
    e.initChip();
    e.handle (MidiEvent::cc (0, 20, 0));
    CHECK (e.parts[0].patch.cutoff == 2047);           // generic: CC20 unmapped
    auto p = genericProfile();
    Control c;
    c.number = 20;
    c.param = "cutoff";
    p.controls.push_back (c);
    p.channel = 3;
    p.bendRange = 12;
    e.setProfile (p);
    e.handle (MidiEvent::cc (2, 20, 0));
    CHECK (e.parts[0].patch.cutoff == 0);
    CHECK (e.parts[0].channel == 3);
    e.handle (MidiEvent::noteOn (2, 60, 100));
    e.handle (MidiEvent::bend (2, 8191));
    CHECK (fl.states[0].getFreq (0) == noteToSid (72));
}

TEST_CASE ("engine: lp+bp+hp is a filter mode", "[profile]")
{
    CHECK (filterModeBits ("lp+bp+hp") == (LP | BP | HP));
    CHECK (filterModeNames().back() == "lp+bp+hp");
}
