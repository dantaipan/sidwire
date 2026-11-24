#include "msid/Sid.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <stdexcept>

namespace msid {

long long pyRound (double x)
{
    // nearbyint uses the current rounding mode; the default is FE_TONEAREST (ties to even),
    // which is exactly Python 3's round().
    return (long long) std::nearbyint (x);
}

double noteToHz (double note, double bendSemitones)
{
    return 440.0 * std::pow (2.0, (note + bendSemitones - 69) / 12.0);
}

int hzToSid (double hz, int clock)
{
    return (int) std::clamp (pyRound (hz * 16777216 / clock), 0LL, 0xFFFFLL);
}

int noteToSid (double note, double bendSemitones, int clock)
{
    return hzToSid (noteToHz (note, bendSemitones), clock);
}

double bendToSemitones (int pitch, double bendRange)
{
    return pitch / (pitch < 0 ? 8192.0 : 8191.0) * bendRange;
}

std::vector<std::pair<int, int>> mergeRanges (DirtySet offsets, int maxGap)
{
    std::vector<std::pair<int, int>> out;
    for (int o = 0; o < 32; ++o)
    {
        if (! has (offsets, o))
            continue;
        if (! out.empty() && o - out.back().second <= maxGap + 1)
            out.back().second = o;
        else
            out.emplace_back (o, o);
    }
    return out;
}

// ---------------------------------------------------------------- SidState

void SidState::setReg (int off, int value, bool force)
{
    if (off < 0 || off >= kNumRegs)
        throw std::out_of_range ("SID register offset out of range");
    auto v = (uint8_t) (value & 0xFF);
    if (force || regs[(size_t) off] != v)
    {
        regs[(size_t) off] = v;
        dirty |= bit (off);
    }
}

void SidState::update (int off, int mask, int bits)
{
    setReg (off, (regs[(size_t) off] & ~mask) | (bits & mask));
}

Batch SidState::chunks (DirtySet offsets, int maxGap, const uint8_t* src) const
{
    if (src == nullptr)
        src = regs.data();
    Batch out;
    for (auto [a, b] : mergeRanges (offsets, maxGap))
        out.push_back ({ (uint16_t) (base + a), std::vector<uint8_t> (src + a, src + b + 1) });
    return out;
}

bool SidState::gateOffPending (int voice) const
{
    auto c = (size_t) voff (voice) + 4;
    return (committed[c] & GATE) && ! (regs[c] & GATE);
}

Batch SidState::snapshot (bool strictOrder)
{
    DirtySet d = takeDirty();
    if (d == 0)
        return {};
    DirtySet ctrl = 0;
    for (int v = 0; v < kNumVoices; ++v)
        if (has (d, 7 * v + 4))
            ctrl |= bit (7 * v + 4);
    bool split = false;
    if (strictOrder)
        for (int v = 0; v < kNumVoices; ++v)
        {
            int c = 7 * v + 4;
            if (has (ctrl, c) && (has (d, c + 1) || has (d, c + 2)))
                split = true;
        }
    Batch writes;
    if (! split)
    {
        writes = chunks (d);
    }
    else
    {
        auto staged = regs;
        for (int v = 0; v < kNumVoices; ++v)
            if (has (ctrl, 7 * v + 4))
                staged[(size_t) (7 * v + 4)] = committed[(size_t) (7 * v + 4)];
        DirtySet data = d & ~ctrl;
        if (data)
            writes = chunks (data, kNumRegs, staged.data());
        auto c = chunks (ctrl);
        writes.insert (writes.end(), c.begin(), c.end());
    }
    committed = regs;
    return writes;
}

int SidState::voff (int voice)
{
    if (voice < 0 || voice >= kNumVoices)
        throw std::out_of_range ("voice out of range");
    return 7 * voice;
}

void SidState::setFreq (int voice, int fn)
{
    int o = voff (voice);
    fn = std::clamp (fn, 0, 0xFFFF);
    setReg (o, fn & 0xFF);
    setReg (o + 1, fn >> 8);
}

int SidState::getFreq (int voice) const
{
    auto o = (size_t) voff (voice);
    return regs[o] | (regs[o + 1] << 8);
}

void SidState::setNote (int voice, double note, double bendSemitones)
{
    setFreq (voice, noteToSid (note, bendSemitones, clock));
}

void SidState::setPulseWidth (int voice, int pw)
{
    int o = voff (voice);
    pw = std::clamp (pw, 0, 0xFFF);
    setReg (o + 2, pw & 0xFF);
    setReg (o + 3, pw >> 8);
}

int SidState::getPulseWidth (int voice) const
{
    auto o = (size_t) voff (voice);
    return regs[o + 2] | ((regs[o + 3] & 0x0F) << 8);
}

void SidState::setGate (int voice, bool on) { update (voff (voice) + 4, GATE, on ? GATE : 0); }
void SidState::setWaveform (int voice, int wave) { update (voff (voice) + 4, 0xFE, wave); }

void SidState::setAdsr (int voice, std::optional<int> a, std::optional<int> d,
                        std::optional<int> s, std::optional<int> r)
{
    int o = voff (voice);
    int ad = regs[(size_t) o + 5], sr = regs[(size_t) o + 6];
    if (a) ad = (ad & 0x0F) | ((*a & 0xF) << 4);
    if (d) ad = (ad & 0xF0) | (*d & 0xF);
    if (s) sr = (sr & 0x0F) | ((*s & 0xF) << 4);
    if (r) sr = (sr & 0xF0) | (*r & 0xF);
    setReg (o + 5, ad);
    setReg (o + 6, sr);
}

std::array<int, 4> SidState::getAdsr (int voice) const
{
    auto o = (size_t) voff (voice);
    int ad = regs[o + 5], sr = regs[o + 6];
    return { ad >> 4, ad & 0xF, sr >> 4, sr & 0xF };
}

void SidState::setCutoff (int fc)
{
    fc = std::clamp (fc, 0, 0x7FF);
    setReg (FC_LO, fc & 0x07);
    setReg (FC_HI, fc >> 3);
}

int SidState::getCutoff() const { return (regs[FC_LO] & 0x07) | (regs[FC_HI] << 3); }
void SidState::setResonance (int res) { update (RES_FILT, 0xF0, (res & 0xF) << 4); }
void SidState::setFilterRoute (int voice, bool on) { int b = 1 << voice; update (RES_FILT, b, on ? b : 0); }
void SidState::setFilterExt (bool on) { update (RES_FILT, 0x08, on ? 0x08 : 0); }
void SidState::setFilterMode (int mode) { update (MODE_VOL, 0xF0, mode); }
void SidState::setVolume (int vol) { update (MODE_VOL, 0x0F, std::clamp (vol, 0, 15)); }

} // namespace msid
