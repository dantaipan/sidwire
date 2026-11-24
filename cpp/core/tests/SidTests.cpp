// Ports of tests/test_sid.py
#include "msid/Sid.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace msid;
using Catch::Approx;

TEST_CASE ("note to Hz", "[sid]")
{
    CHECK (noteToHz (69) == Approx (440.0));
    CHECK (noteToHz (81) == Approx (880.0));
    CHECK (noteToHz (69, 12) == Approx (880.0));
}

TEST_CASE ("PAL frequency table", "[sid]")
{
    CHECK (noteToSid (69) == 0x1D45);   // A4 — the Phase 0 hardware beep
    CHECK (noteToSid (60) == 0x1167);   // C4
    CHECK (noteToSid (57) == 0x0EA2);   // A3
    CHECK (noteToSid (81) == 0x3A89);   // A5
    CHECK (noteToSid (69, 0, kNtscClock) < noteToSid (69));
}

TEST_CASE ("pyRound is ties-to-even like Python", "[sid]")
{
    CHECK (pyRound (0.5) == 0);
    CHECK (pyRound (1.5) == 2);
    CHECK (pyRound (2.5) == 2);
    CHECK (pyRound (-0.5) == 0);
    CHECK (pyRound (2.51) == 3);
}

TEST_CASE ("clamping and bend", "[sid]")
{
    CHECK (hzToSid (-5) == 0);
    CHECK (noteToSid (127) == 0xFFFF);
    CHECK (bendToSemitones (8191) == Approx (2.0));
    CHECK (bendToSemitones (-8192) == Approx (-2.0));
}

TEST_CASE ("merge ranges", "[sid]")
{
    using R = std::vector<std::pair<int, int>>;
    CHECK (mergeRanges (0) == R {});
    CHECK (mergeRanges (bit (0) | bit (1) | bit (2) | bit (4) | bit (5) | bit (0x18)) == R { { 0, 2 }, { 4, 5 }, { 0x18, 0x18 } });
    CHECK (mergeRanges (bit (0) | bit (2), 1) == R { { 0, 2 } });
    CHECK (mergeRanges (bit (0) | bit (3), 1) == R { { 0, 0 }, { 3, 3 } });
}

TEST_CASE ("dirty only on change; chunks use shadow values", "[sid]")
{
    SidState s;
    s.setReg (0x18, 0);
    CHECK (s.dirty == 0);
    s.setFreq (0, 0x1D45);
    s.setVolume (15);
    auto d = s.takeDirty();
    CHECK (d == (bit (0) | bit (1) | bit (0x18)));
    auto c = s.chunks (d, 0);
    REQUIRE (c.size() == 2);
    CHECK (c[0] == Write { 0xD400, { 0x45, 0x1D } });
    CHECK (c[1] == Write { 0xD418, { 0x0F } });
}

TEST_CASE ("snapshot splits control after ADSR", "[sid]")
{
    SidState s;
    s.setWaveform (0, SAW);
    s.setAdsr (0, 2, 3, 4, 5);
    s.setGate (0, true);
    auto w = s.snapshot (true);
    REQUIRE (w.size() == 2);
    CHECK (w[0] == Write { 0xD405, { 0x23, 0x45 } });
    CHECK (w[1] == Write { 0xD404, { 0x21 } });
    s.setAdsr (0, 1);
    s.setGate (0, false);
    CHECK (s.snapshot (false).size() == 1);
}

TEST_CASE ("register packing", "[sid]")
{
    SidState s;
    s.setPulseWidth (1, 0x1FFF);
    CHECK (s.getPulseWidth (1) == 0xFFF);
    s.setCutoff (0x40B);
    CHECK (s.regs[0x15] == 0x03);
    CHECK (s.regs[0x16] == 0x81);
    s.setResonance (0xA);
    s.setFilterRoute (0, true);
    s.setFilterRoute (2, true);
    s.setFilterExt (true);
    CHECK (s.regs[0x17] == 0xAD);
    s.setFilterMode (LP | VOICE3_OFF);
    s.setVolume (99);
    CHECK (s.regs[0x18] == 0x9F);
    s.setWaveform (0, PULSE);
    s.setGate (0, true);
    CHECK (s.getControl (0) == 0x41);
    s.setWaveform (0, TRI | GATE);   // GATE ignored
    CHECK (s.getControl (0) == 0x11);
}
