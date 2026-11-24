// SID register model: shadow registers, dirty tracking, pitch maths.
// C++ port of sid.py — keep the two in step (golden vectors in core/tests/vectors).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace msid {

constexpr int kPalClock = 985248;
constexpr int kNtscClock = 1022727;
constexpr int kNumRegs = 25;
constexpr int kNumVoices = 3;

// control register bits
constexpr uint8_t GATE = 0x01, SYNC = 0x02, RING = 0x04, TEST = 0x08;
constexpr uint8_t TRI = 0x10, SAW = 0x20, PULSE = 0x40, NOISE = 0x80;

// global register offsets
constexpr int FC_LO = 0x15, FC_HI = 0x16, RES_FILT = 0x17, MODE_VOL = 0x18;

// filter modes ($18 high nibble)
constexpr uint8_t LP = 0x10, BP = 0x20, HP = 0x40, VOICE3_OFF = 0x80;

// ---------------------------------------------------------------- pitch maths
/// Python's round(): nearest, ties to even.
long long pyRound (double x);
double noteToHz (double note, double bendSemitones = 0.0);
int hzToSid (double hz, int clock = kPalClock);
int noteToSid (double note, double bendSemitones = 0.0, int clock = kPalClock);
/// pitchwheel value (-8192..8191) -> semitones
double bendToSemitones (int pitch, double bendRange = 2.0);

// ---------------------------------------------------------------- dirty ranges
using DirtySet = uint32_t;   // bit n = register offset n

inline DirtySet bit (int off) { return DirtySet (1) << off; }
inline bool has (DirtySet d, int off) { return (d >> off) & 1u; }
constexpr DirtySet kAllDirty = (DirtySet (1) << kNumRegs) - 1;

/// Merge offsets into inclusive (start, end) ranges; gaps of <= maxGap are joined.
std::vector<std::pair<int, int>> mergeRanges (DirtySet offsets, int maxGap = 0);

struct Write
{
    uint16_t address = 0;
    std::vector<uint8_t> data;
    bool operator== (const Write&) const = default;
};
using Batch = std::vector<Write>;

// ---------------------------------------------------------------- state
class SidState
{
public:
    explicit SidState (uint16_t baseAddress = 0xD400, int clockHz = kPalClock) : base (baseAddress), clock (clockHz) {}

    uint16_t base;
    int clock;
    std::array<uint8_t, kNumRegs> regs {};
    std::array<uint8_t, kNumRegs> committed {};   // values already handed to the transport
    DirtySet dirty = 0;

    void setReg (int off, int value, bool force = false);
    int getReg (int off) const { return regs[(size_t) off]; }
    void markAllDirty() { dirty = kAllDirty; }
    DirtySet takeDirty() { auto d = dirty; dirty = 0; return d; }

    Batch chunks (DirtySet offsets, int maxGap = kNumRegs, const uint8_t* src = nullptr) const;
    bool gateOffPending (int voice) const;
    /// Dirty set -> ordered writes (see sid.py SidState.snapshot).
    Batch snapshot (bool strictOrder = true);

    static int voff (int voice);
    void setFreq (int voice, int fn);
    int getFreq (int voice) const;
    void setNote (int voice, double note, double bendSemitones = 0.0);
    void setPulseWidth (int voice, int pw);
    int getPulseWidth (int voice) const;
    void setControl (int voice, int value) { setReg (voff (voice) + 4, value); }
    int getControl (int voice) const { return regs[(size_t) voff (voice) + 4]; }
    void setGate (int voice, bool on);
    void setWaveform (int voice, int wave);   // everything but GATE
    void setAdsr (int voice, std::optional<int> a, std::optional<int> d = {},
                  std::optional<int> s = {}, std::optional<int> r = {});
    std::array<int, 4> getAdsr (int voice) const;

    void setCutoff (int fc);
    int getCutoff() const;
    void setResonance (int res);
    void setFilterRoute (int voice, bool on);
    void setFilterExt (bool on);
    void setFilterMode (int mode);
    void setVolume (int vol);
    int getVolume() const { return regs[MODE_VOL] & 0x0F; }

private:
    void update (int off, int mask, int bits);
};

} // namespace msid
