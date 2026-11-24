#include "msid/Engine.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace msid {

// ---------------------------------------------------------------- names / tables

namespace {
const char* kParamNames[kNumParams] = { "pulse_width", "cutoff", "resonance", "attack",
                                        "decay", "sustain", "release", "volume" };
const int kParamMax[kNumParams] = { 4095, 2047, 15, 15, 15, 15, 15, 15 };
// SID release times for the full envelope, by release nibble (datasheet), seconds
const double kReleaseS[16] = { 0.006, 0.024, 0.048, 0.072, 0.114, 0.168, 0.204, 0.24,
                               0.3, 0.75, 1.5, 2.4, 3.0, 9.0, 15.0, 24.0 };

const std::vector<std::pair<std::string, int>> kWaveforms {
    { "tri", TRI }, { "saw", SAW }, { "pulse", PULSE }, { "noise", NOISE },
    { "tri+saw", TRI | SAW }, { "tri+pulse", TRI | PULSE }, { "saw+pulse", SAW | PULSE },
    { "tri+saw+pulse", TRI | SAW | PULSE } };
const std::vector<std::pair<std::string, int>> kFilterModes {
    { "off", 0 }, { "lp", LP }, { "bp", BP }, { "hp", HP }, { "lp+hp", LP | HP },
    { "lp+bp", LP | BP }, { "bp+hp", BP | HP }, { "lp+bp+hp", LP | BP | HP } };

const std::vector<std::string> kVoiceModes { "poly", "mono", "unison", "octave", "fifth", "chord", "arp" };
const std::vector<std::pair<std::string, int>> kArpRates { { "1/4", 1 }, { "1/8", 2 }, { "1/8T", 3 }, { "1/16", 4 }, { "1/16T", 6 }, { "1/32", 8 } };
const std::vector<std::string> kArpPatterns { "up", "down", "up-down", "random", "as played" };

std::vector<int> stackFor (const std::string& mode)
{
    if (mode == "octave") return { 0, 12 };
    if (mode == "fifth") return { 0, 7 };
    if (mode == "chord") return { 0, 7, 12 };
    return {};
}

int arpStepsPerBeat (const std::string& rate)
{
    for (auto& [k, v] : kArpRates)
        if (k == rate)
            return v;
    return 4;
}

template <typename T>
std::vector<std::string> keys (const T& table)
{
    std::vector<std::string> out;
    for (auto& [k, v] : table)
        out.push_back (k);
    return out;
}
} // namespace

const char* paramName (Param p) { return kParamNames[(int) p]; }
const std::vector<std::string>& voiceModeNames() { return kVoiceModes; }
const std::vector<std::string>& arpRateNames()
{
    static const auto names = keys (kArpRates);
    return names;
}
const std::vector<std::string>& arpPatternNames() { return kArpPatterns; }
int paramMax (Param p) { return kParamMax[(int) p]; }

std::optional<Param> paramFromName (const std::string& n)
{
    for (int i = 0; i < kNumParams; ++i)
        if (n == kParamNames[i])
            return (Param) i;
    return std::nullopt;
}

const std::vector<std::string>& waveformNames()
{
    static const auto names = keys (kWaveforms);
    return names;
}

std::optional<int> waveformBits (const std::string& name)
{
    for (auto& [k, v] : kWaveforms)
        if (k == name)
            return v;
    return std::nullopt;
}

const std::vector<std::string>& filterModeNames()
{
    static const auto names = keys (kFilterModes);
    return names;
}

std::optional<int> filterModeBits (const std::string& name)
{
    for (auto& [k, v] : kFilterModes)
        if (k == name)
            return v;
    return std::nullopt;
}

int& Patch::get (Param p)
{
    switch (p)
    {
        case Param::PulseWidth: return pulseWidth;
        case Param::Cutoff:     return cutoff;
        case Param::Resonance:  return resonance;
        case Param::Attack:     return attack;
        case Param::Decay:      return decay;
        case Param::Sustain:    return sustain;
        case Param::Release:    return release;
        case Param::Volume:     return volume;
    }
    throw std::logic_error ("bad Param");
}

PatchUpdate PatchUpdate::full (const Patch& p)
{
    PatchUpdate u;
    u.name = p.name; u.waveform = p.waveform; u.filterMode = p.filterMode;
    u.pulseWidth = p.pulseWidth; u.attack = p.attack; u.decay = p.decay; u.sustain = p.sustain;
    u.release = p.release; u.cutoff = p.cutoff; u.resonance = p.resonance; u.volume = p.volume;
    u.filterVoices = p.filterVoices;
    return u;
}

// ---------------------------------------------------------------- engine

Engine::Engine (Flusher& f, EngineConfig c, std::vector<Preset> presets)
    : fl (f), cfg (std::move (c)), presetList (std::move (presets))
{
    now = [] { return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count(); };
    int n = (int) fl.states.size();
    std::vector<std::pair<int, std::vector<int>>> groups;
    if (cfg.layout == "split" && n > 1)
    {
        for (int i = 0; i < n; ++i)
            groups.push_back ({ i + 1, { i } });
    }
    else
    {
        std::vector<int> all;
        for (int i = 0; i < n; ++i)
            all.push_back (i);
        groups.push_back ({ cfg.profile.channel, all });
    }
    layout = groups.size() > 1 ? "split" : "poly";
    for (size_t i = 0; i < groups.size(); ++i)
    {
        Part p;
        p.index = (int) i;
        p.channel = groups[i].first;
        p.sids = groups[i].second;
        auto makeSlot = [] (int sidIndex, int voice) { Slot sl; sl.sid = sidIndex; sl.voice = voice; return sl; };
        for (int s : p.sids)   // every voice, in every mode
            for (int v = 0; v < kNumVoices; ++v)
                p.slots.push_back (makeSlot (s, v));
        p.patch = cfg.patch;
        parts.push_back (std::move (p));
    }
    if (cfg.knobsTarget == 0)
        for (size_t i = 0; i < parts.size(); ++i)
            knobPartIdx.push_back ((int) i);
    else
        knobPartIdx.push_back (std::min (cfg.knobsTarget, (int) parts.size()) - 1);
    for (auto& p : parts)
        for (auto& s : p.slots)
            slotAt[{ s.sid, s.voice }] = &s;
    setVelocityFields (cfg.velocity, cfg.velocityAmount);
    setArp (cfg.arpRate, cfg.arpPattern, cfg.arpOctaves, cfg.arpGate, cfg.bpm);
    if (std::find (kVoiceModes.begin(), kVoiceModes.end(), cfg.mode) == kVoiceModes.end())
        throw std::invalid_argument ("voice mode must be poly, mono, unison, octave, fifth, chord or arp, not '" + cfg.mode + "'");
    vmode = cfg.mode;
    setOffsets();
    if (cfg.filterCycle.empty())
        throw std::invalid_argument ("filterCycle is empty");
    for (auto& m : cfg.filterCycle)
        if (! filterModeBits (m))
            throw std::invalid_argument ("unknown filter mode in filterCycle: '" + m + "'");
}

// ---------------------------------------------------------------- voice modes / arp

void Engine::setVoiceMode (const std::string& mode)
{
    if (std::find (kVoiceModes.begin(), kVoiceModes.end(), mode) == kVoiceModes.end())
        throw std::invalid_argument ("unknown voice mode '" + mode + "'");
    std::lock_guard g (engineMutex);
    if (mode == vmode)
        return;
    for (auto& part : parts)
    {
        for (auto& s : part.slots)
            if (s.gated)
                releaseVoice (s);
        part.monoStack.clear();
        part.arpNotes.clear();
        part.arpRunning = false;
        part.arpGateOff.reset();
    }
    vmode = mode;
    setOffsets();
    info ("voice mode -> " + mode);
}

std::string Engine::cycleVoiceMode()
{
    std::lock_guard g (engineMutex);
    auto it = std::find (kVoiceModes.begin(), kVoiceModes.end(), vmode);
    auto next = kVoiceModes[(size_t) ((it - kVoiceModes.begin() + 1) % (long) kVoiceModes.size())];
    setVoiceMode (next);
    return next;
}

void Engine::setArp (const std::string& rate, const std::string& pattern, int octaves, double gate, std::optional<double> bpm)
{
    auto rates = arpRateNames();
    if (std::find (rates.begin(), rates.end(), rate) == rates.end())
        throw std::invalid_argument ("unknown arp rate '" + rate + "'");
    if (std::find (kArpPatterns.begin(), kArpPatterns.end(), pattern) == kArpPatterns.end())
        throw std::invalid_argument ("unknown arp pattern '" + pattern + "'");
    std::lock_guard g (engineMutex);
    arpRateName = rate;
    arpPatternName = pattern;
    arpOct = std::clamp (octaves, 1, 3);
    arpGateFrac = std::min (1.0, std::max (0.05, gate));
    if (bpm)
        setBpm (*bpm);
}

void Engine::setBpm (double b)
{
    tempo = std::min (300.0, std::max (20.0, b));
}

void Engine::setOffsets()
{
    for (auto& part : parts)
        for (size_t i = 0; i < part.slots.size(); ++i)
        {
            auto& s = part.slots[i];
            if (vmode == "unison")
            {
                int k = (int) (i + 1) / 2;
                s.offset = (i % 2 ? k : -k) * cfg.unisonDetune / 100.0;
            }
            else
                s.offset = 0.0;
        }
}

double Engine::arpStepLen() const
{
    return 60.0 / tempo / arpStepsPerBeat (arpRateName);
}

std::vector<std::pair<int, int>> Engine::arpSequence (const Part& part) const
{
    auto base = part.arpNotes;
    if (arpPatternName != "as played")
        std::sort (base.begin(), base.end());
    std::vector<std::pair<int, int>> seq;
    for (int o = 0; o < arpOct; ++o)
        for (auto& [n, v] : base)
            seq.push_back ({ n + 12 * o, v });
    if (arpPatternName == "down")
        std::reverse (seq.begin(), seq.end());
    else if (arpPatternName == "up-down" && seq.size() >= 3)
        for (size_t i = seq.size() - 2; i >= 1; --i)   // bounce without repeating the ends
            seq.push_back (seq[i]);
    return seq;
}

void Engine::arpStep (Part& part, double t)
{
    auto seq = arpSequence (part);
    if (seq.empty())
        return;
    int n = (int) seq.size();
    if (arpPatternName == "random")
    {
        part.arpRng = (part.arpRng * 1103515245ULL + 12345ULL) & 0x7FFFFFFFULL;
        part.arpIndex = (int) (part.arpRng % (uint64_t) n);
    }
    else
        part.arpIndex = (part.arpIndex + 1) % n;
    auto [note, vel] = seq[(size_t) part.arpIndex];
    auto& slot = part.slots[0];
    slot.offset = 0.0;
    startVoice (part, slot, note, vel);
    if (arpGateFrac < 1.0 && ! clockActive (t))
        part.arpGateOff = t + arpStepLen() * arpGateFrac;
    else
        part.arpGateOff.reset();
}

void Engine::arpNoteOn (Part& part, int note, int velocity)
{
    bool found = false;
    for (auto& nv : part.arpNotes)
        if (nv.first == note)
        {
            nv.second = velocity;
            found = true;
            break;
        }
    if (! found)
        part.arpNotes.push_back ({ note, velocity });
    if (! part.arpRunning)   // the first note: play at once
    {
        part.arpRunning = true;
        part.arpIndex = -1;
        part.arpPulse = 0;
        part.arpRng = 1;
        double t = now();
        arpStep (part, t);
        part.arpNext = t + arpStepLen();
    }
}

void Engine::arpNoteOff (Part& part, int note)
{
    part.arpNotes.erase (std::remove_if (part.arpNotes.begin(), part.arpNotes.end(),
                                         [note] (auto& nv) { return nv.first == note; }), part.arpNotes.end());
    if (part.arpNotes.empty() && part.arpRunning)
    {
        part.arpRunning = false;
        part.arpGateOff.reset();
        if (part.slots[0].gated)
            releaseVoice (part.slots[0]);
    }
}

void Engine::arpService (double t)
{
    bool clocked = clockActive (t);
    for (auto& part : parts)
    {
        if (! part.arpRunning)
            continue;
        if (! clocked)
        {
            double step = arpStepLen();
            if (t - part.arpNext > 1.0)   // stalled (e.g. the computer slept): no burst
                part.arpNext = t;
            while (t >= part.arpNext)
            {
                double at = part.arpNext;
                if (part.arpGateOff && *part.arpGateOff <= at)
                {
                    if (part.slots[0].gated)
                        releaseVoice (part.slots[0]);
                    part.arpGateOff.reset();
                }
                arpStep (part, at);
                part.arpNext = at + step;
            }
        }
        if (part.arpGateOff && t >= *part.arpGateOff)
        {
            if (part.slots[0].gated)
                releaseVoice (part.slots[0]);
            part.arpGateOff.reset();
        }
    }
}

void Engine::clockPulse()
{
    double t = now();
    clockSeenAt = t;
    if (vmode != "arp")
        return;
    int pps = 24 / arpStepsPerBeat (arpRateName);
    int gatePulses = std::max (1, (int) pyRound (pps * arpGateFrac));
    for (auto& part : parts)
    {
        if (! part.arpRunning)
            continue;
        part.arpNext = t + arpStepLen();   // time-based stepping waits while clocked
        part.arpPulse += 1;
        if (arpGateFrac < 1.0 && part.arpPulse == gatePulses && part.slots[0].gated)
            releaseVoice (part.slots[0]);
        if (part.arpPulse >= pps)
        {
            part.arpPulse = 0;
            arpStep (part, t);
        }
    }
}

void Engine::clockStart()
{
    for (auto& part : parts)
    {
        part.arpPulse = 0;
        part.arpIndex = -1;
    }
}

std::vector<Slot*> Engine::monoVoices (Part& part)
{
    std::vector<Slot*> out;
    if (vmode == "unison")
        for (auto& s : part.slots)
            out.push_back (&s);
    else
        out.push_back (&part.slots[0]);
    return out;
}

Slot& Engine::allocStack (Part& part, int note, int interval)
{
    for (auto& s : part.slots)   // this note's voice for this interval -> reuse
        if (s.note == note && s.offset == (double) interval)
            return s;
    Slot* best = nullptr;
    for (auto& s : part.slots)
        if (! s.gated && (best == nullptr || s.offTime < best->offTime))
            best = &s;
    if (best)
        return *best;
    best = &part.slots[0];
    for (auto& s : part.slots)
        if (s.onTime < best->onTime)
            best = &s;
    return *best;
}

// ---------------------------------------------------------------- velocity

void Engine::setVelocityFields (const std::string& mode, double amount)
{
    if (mode != "off" && mode != "sustain" && mode != "volume")
        throw std::invalid_argument ("velocity must be off, sustain or volume, not '" + mode + "'");
    velMode = mode;
    velAmount = std::min (1.0, std::max (0.0, amount));
}

void Engine::setVelocity (const std::string& mode, double amount)
{
    std::lock_guard g (engineMutex);
    setVelocityFields (mode, amount);
    auto e = fl.edit();
    for (auto& part : parts)
    {
        part.vel = 1.0;
        for (auto& s : part.slots)
            s.vel = 1.0;
        for (int i : part.sids)
        {
            auto& st = fl.states[(size_t) i];
            for (int v = 0; v < kNumVoices; ++v)
                st.setAdsr (v, {}, {}, sustainFor (part, i, v));
            if (! part.muted)
                st.setVolume (volumeFor (part));
        }
    }
}

int Engine::sustainFor (const Part& part, int sidIndex, int voice) const
{
    if (velMode != "sustain")
        return part.patch.sustain;
    auto it = slotAt.find ({ sidIndex, voice });
    return (int) pyRound (part.patch.sustain * (it != slotAt.end() ? it->second->vel : 1.0));
}

int Engine::volumeFor (const Part& part) const
{
    if (velMode != "volume")
        return part.patch.volume;
    return (int) pyRound (part.patch.volume * part.vel);
}

void Engine::initChip()
{
    std::lock_guard g (engineMutex);
    {
        auto e = fl.edit();
        for (auto& st : fl.states)
        {
            st.regs.fill (0);
            st.markAllDirty();
        }
    }
    for (auto& part : parts)
        applyPatch (part, PatchUpdate::full (part.patch), true);
}

void Engine::applyPatch (Part& part, const PatchUpdate& u, bool force)
{
    std::lock_guard g (engineMutex);
    auto& p = part.patch;
    if (u.name) p.name = *u.name;
    if (u.waveform) p.waveform = *u.waveform;
    if (u.pulseWidth) p.pulseWidth = *u.pulseWidth;
    if (u.attack) p.attack = *u.attack;
    if (u.decay) p.decay = *u.decay;
    if (u.sustain) p.sustain = *u.sustain;
    if (u.release) p.release = *u.release;
    if (u.filterMode) p.filterMode = *u.filterMode;
    if (u.filterVoices) p.filterVoices = *u.filterVoices;
    if (u.cutoff) p.cutoff = *u.cutoff;
    if (u.resonance) p.resonance = *u.resonance;
    if (u.volume) p.volume = *u.volume;
    if (u.volume)
        part.muted = false;
    part.norm.fill (std::nullopt);   // encoders resume from the new values

    auto e = fl.edit();
    for (int i : part.sids)
    {
        auto& st = fl.states[(size_t) i];
        for (int v = 0; v < kNumVoices; ++v)
        {
            if (force || u.waveform)
                st.setWaveform (v, waveformBits (p.waveform).value_or (PULSE));
            if (force || u.pulseWidth)
                st.setPulseWidth (v, p.pulseWidth);
            if (force || u.hasAdsr())
                st.setAdsr (v, p.attack, p.decay, sustainFor (part, i, v), p.release);
            if (force || u.filterVoices || u.filterMode)
                // routing a voice into the filter with no mode selected silences it
                st.setFilterRoute (v, p.filterVoices && p.filterMode != "off");
        }
        if (force || u.filterMode)
            st.setFilterMode (filterModeBits (p.filterMode).value_or (0));
        if (force || u.cutoff)
            st.setCutoff (p.cutoff);
        if (force || u.resonance)
            st.setResonance (p.resonance);
        if (force || u.volume)
            st.setVolume (part.muted ? 0 : volumeFor (part));
    }
}

const Preset* Engine::selectPreset (Part& part, int index)
{
    if (presetList.empty())
        return nullptr;
    int n = (int) presetList.size();
    int i = ((index % n) + n) % n;
    const auto& pr = presetList[(size_t) i];
    applyPatch (part, pr);
    part.presetIndex = i;
    info ("part " + std::to_string (part.index + 1) + ": preset '" + pr.name.value_or ("") + "'");
    return &pr;
}

const Preset* Engine::selectPreset (Part& part, const std::string& name)
{
    auto lower = [] (std::string s) {
        std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        return s;
    };
    for (size_t i = 0; i < presetList.size(); ++i)
        if (lower (presetList[i].name.value_or ("")) == lower (name))
        {
            applyPatch (part, presetList[i]);
            part.presetIndex = (int) i;
            return &presetList[i];
        }
    throw std::out_of_range ("no preset named '" + name + "'");
}

Part* Engine::partFor (int channel)
{
    for (auto& p : parts)
        if (p.channel == 0 || channel == p.channel - 1)
            return &p;
    return nullptr;
}

std::vector<Part*> Engine::knobParts()
{
    std::vector<Part*> out;
    for (int i : knobPartIdx)
        out.push_back (&parts[(size_t) i]);
    return out;
}

std::vector<const Control*> Engine::match (Port port, Control::Kind kind, int channel0, int number) const
{
    std::vector<const Control*> out;
    for (auto& c : cfg.profile.controls)
        if (c.matches (port, kind, channel0, number))
            out.push_back (&c);
    return out;
}

void Engine::handle (const MidiEvent& e, Port port)
{
    std::lock_guard g (engineMutex);
    using T = MidiEvent::Type;
    using K = Control::Kind;
    if (e.type == T::Clock)
    {
        clockPulse();
        return;
    }
    if (e.type == T::Start)
    {
        clockStart();
        return;
    }
    if (e.type == T::NoteOn || e.type == T::NoteOff)
    {
        auto hits = match (port, K::Note, e.channel, e.data1);
        if (! hits.empty())   // a pad / button: never plays the SID
        {
            if (e.type == T::NoteOn && e.data2 > 0)
                for (auto* c : hits)
                    action (c->action);
            return;
        }
        auto& ign = cfg.profile.ignoreChannels;
        if (port != Port::Notes || std::find (ign.begin(), ign.end(), e.channel + 1) != ign.end())
            return;
    }
    else if (port != Port::Notes)   // knobs / faders / buttons
    {
        if (e.type == T::ControlChange)
        {
            auto targets = knobParts();
            for (auto* c : match (port, K::CC, e.channel, e.data1))
                applyControl (*c, e.data2, targets);
        }
        else if (e.type == T::PitchBend)
        {
            for (auto* c : match (port, K::PitchBend, e.channel))
                if (auto p = paramFromName (c->param))
                    for (auto* part : knobParts())
                        setParam (*part, *p, (e.data1 + 8192) / 16383.0);
        }
        return;
    }
    Part* part = partFor (e.channel);
    if (part == nullptr)
        return;
    switch (e.type)
    {
        case T::NoteOn:
            if (e.data2 > 0) noteOn (e.data1, e.data2, *part);
            else             noteOff (e.data1, *part);
            break;
        case T::NoteOff:       noteOff (e.data1, *part); break;
        case T::ControlChange: controlChange (e.data1, e.data2, *part, e.channel); break;
        case T::PitchBend:
        {
            auto hits = match (Port::Notes, K::PitchBend, e.channel);
            if (hits.empty())
                pitchBend (e.data1, *part);
            for (auto* c : hits)
                if (auto p = paramFromName (c->param))
                    setParam (*part, *p, (e.data1 + 8192) / 16383.0);
            break;
        }
        case T::ProgramChange: programChange (e.data1, *part); break;
        case T::Clock: case T::Start: break;   // handled above
    }
}

void Engine::applyControl (const Control& c, int value, const std::vector<Part*>& targets)
{
    if (! c.param.empty())
    {
        auto p = paramFromName (c.param);
        if (! p)
            return;
        for (auto* part : targets)
        {
            if (c.mode == "absolute")
                setParam (*part, *p, value / 127.0);
            else
                nudgeParam (*part, *p, c.decodeRelative (value) / c.steps);
        }
    }
    else if (c.action == "sustain_pedal")
    {
        for (auto* part : targets)
            setPedal (*part, value >= 64);
    }
    else if (value >= 64)   // actions act on the knob target parts
    {
        action (c.action);
    }
}

void Engine::action (const std::string& name)
{
    auto targets = knobParts();
    if (name == "filter_mode_next")
    {
        cycleFilterMode();
    }
    else if ((name == "preset_next" || name == "preset_prev") && ! presetList.empty())
    {
        int n = (int) presetList.size();
        int cur = targets[0]->presetIndex;
        int next = name == "preset_next" ? (cur + 1) % n : (cur >= 0 ? ((cur - 1) % n + n) % n : n - 1);
        for (auto* part : targets)
            selectPreset (*part, next);
    }
    else if (name == "waveform_next")
    {
        const auto& names = waveformNames();
        auto it = std::find (names.begin(), names.end(), targets[0]->patch.waveform);
        auto next = it == names.end() ? names[0] : names[(size_t) ((it - names.begin() + 1) % (long) names.size())];
        PatchUpdate u;
        u.waveform = next;
        for (auto* part : targets)
            applyPatch (*part, u);
    }
    else if (name == "panic")
    {
        panic();
    }
    else if (name == "voice_mode_next")
    {
        cycleVoiceMode();
    }
}

// ---------------------------------------------------------------- notes

void Engine::unmute (Part& part)
{
    if (part.muted)
    {
        part.muted = false;
        auto e = fl.edit();
        for (int i : part.sids)
            fl.states[(size_t) i].setVolume (volumeFor (part));
    }
}

Slot& Engine::alloc (Part& part, int note)
{
    for (auto& s : part.slots)   // same note already assigned -> reuse
        if (s.note == note)
            return s;
    Slot* best = nullptr;
    for (auto& s : part.slots)   // free: released longest ago first
        if (! s.gated && (best == nullptr || s.offTime < best->offTime))
            best = &s;
    if (best)
        return *best;
    best = &part.slots[0];       // steal oldest
    for (auto& s : part.slots)
        if (s.onTime < best->onTime)
            best = &s;
    return *best;
}

void Engine::startVoice (Part& part, Slot& slot, int note, std::optional<int> velocity)
{
    auto& st = fl.states[(size_t) slot.sid];
    int v = slot.voice;
    auto& p = part.patch;
    if (velocity && velMode == "sustain")
        slot.vel = velScale (*velocity);
    if (cfg.hardRestart)
    {
        {
            auto e = fl.edit();
            st.setGate (v, false);
            st.setAdsr (v, 0, 0, 0, 0);
        }
        fl.barrier();
        auto e = fl.edit();
        st.setAdsr (v, p.attack, p.decay, sustainFor (part, slot.sid, v), p.release);
    }
    else if (slot.gated)   // steal / retrigger: gate-off goes out first
    {
        fl.gate (slot.sid, v, false);
        fl.barrier();
    }
    {
        auto e = fl.edit();
        st.setNote (v, note, part.bend + slot.offset);
        st.setAdsr (v, {}, {}, sustainFor (part, slot.sid, v));   // before the gate (flush order)
    }
    fl.gate (slot.sid, v, true);
    slot.note = note;
    slot.gated = true;
    slot.onTime = tick();
}

void Engine::releaseVoice (Slot& slot)
{
    fl.gate (slot.sid, slot.voice, false);
    slot.gated = false;
    slot.offTime = tick();
    slot.offAt = now();
}

void Engine::service()
{
    std::lock_guard g (engineMutex);
    double t = now();
    if (vmode == "arp")
        arpService (t);
    if (! cfg.muteAfterRelease)
        return;
    for (auto& part : parts)
    {
        double doneAfter = kReleaseS[std::clamp (part.patch.release, 0, 15)] * 1.2 + 0.05;
        for (auto& s : part.slots)
            if (! s.gated && s.note && t - s.offAt >= doneAfter)
            {
                {
                    auto e = fl.edit();
                    fl.states[(size_t) s.sid].setFreq (s.voice, 0);
                }
                s.note.reset();
            }
    }
}

void Engine::noteOn (int note, int velocity, Part& part)
{
    std::lock_guard g (engineMutex);
    if (velMode == "volume")
    {
        part.vel = velScale (velocity);
        if (! part.muted)
        {
            auto e = fl.edit();
            for (int i : part.sids)
                fl.states[(size_t) i].setVolume (volumeFor (part));
        }
    }
    unmute (part);
    part.heldByPedal.erase (note);
    if (vmode == "arp")
    {
        arpNoteOn (part, note, velocity);
        return;
    }
    if (vmode == "mono" || vmode == "unison")
    {
        auto voices = monoVoices (part);
        auto& stack = part.monoStack;
        stack.erase (std::remove (stack.begin(), stack.end(), note), stack.end());
        stack.push_back (note);
        if (voices[0]->gated && cfg.legato)
        {
            auto e = fl.edit();
            for (auto* slot : voices)
            {
                fl.states[(size_t) slot->sid].setNote (slot->voice, note, part.bend + slot->offset);
                slot->note = note;
            }
        }
        else
            for (auto* slot : voices)
                startVoice (part, *slot, note, velocity);
        return;
    }
    if (auto intervals = stackFor (vmode); ! intervals.empty())
    {
        for (int interval : intervals)
        {
            auto& slot = allocStack (part, note, interval);
            slot.offset = (double) interval;
            startVoice (part, slot, note, velocity);
        }
        return;
    }
    startVoice (part, alloc (part, note), note, velocity);
}

void Engine::noteOff (int note, Part& part)
{
    std::lock_guard g (engineMutex);
    if (part.pedal)
    {
        part.heldByPedal.insert (note);
        return;
    }
    if (vmode == "arp")
    {
        arpNoteOff (part, note);
        return;
    }
    if (vmode == "mono" || vmode == "unison")
    {
        auto voices = monoVoices (part);
        auto& stack = part.monoStack;
        stack.erase (std::remove (stack.begin(), stack.end(), note), stack.end());
        if (voices[0]->note != note)
            return;
        if (! stack.empty() && cfg.legato)
        {
            int prev = stack.back();
            auto e = fl.edit();
            for (auto* slot : voices)
            {
                fl.states[(size_t) slot->sid].setNote (slot->voice, prev, part.bend + slot->offset);
                slot->note = prev;
            }
        }
        else if (! stack.empty())
        {
            int prev = stack.back();
            for (auto* slot : voices)
                startVoice (part, *slot, prev);
        }
        else
            for (auto* slot : voices)
                releaseVoice (*slot);
        return;
    }
    for (auto& s : part.slots)
        if (s.note == note && s.gated)
            releaseVoice (s);
}

// ---------------------------------------------------------------- controllers

void Engine::pitchBend (int pitch, Part& part)
{
    std::lock_guard g (engineMutex);
    part.bend = bendToSemitones (pitch, cfg.profile.bendRange);
    auto e = fl.edit();
    for (auto& s : part.slots)
        if (s.note)
            fl.states[(size_t) s.sid].setNote (s.voice, *s.note, part.bend + s.offset);
}

void Engine::controlChange (int cc, int value, Part& part, int channel0)
{
    std::lock_guard g (engineMutex);
    if (cc == 120 || cc == 123)
    {
        panic();
        return;
    }
    for (auto* c : match (Port::Notes, Control::Kind::CC, channel0, cc))
        applyControl (*c, value, { &part });
}

void Engine::setPedal (Part& part, bool down)
{
    std::lock_guard g (engineMutex);
    part.pedal = down;
    if (! down)
    {
        auto held = part.heldByPedal;   // sorted
        for (int n : held)
            noteOff (n, part);
        part.heldByPedal.clear();
    }
}

double Engine::getParam (const Part& part, Param p) const
{
    return part.patch.get (p) / (double) paramMax (p);
}

void Engine::setParam (Part& part, Param p, double norm)
{
    std::lock_guard g (engineMutex);
    norm = std::min (1.0, std::max (0.0, norm));
    part.norm[(size_t) p] = norm;
    int value = (int) pyRound (norm * paramMax (p));
    if (value == part.patch.get (p) && ! (p == Param::Volume && part.muted))
        return;
    part.patch.get (p) = value;
    {
        auto e = fl.edit();
        for (int i : part.sids)
        {
            auto& st = fl.states[(size_t) i];
            switch (p)
            {
                case Param::PulseWidth:
                    for (int v = 0; v < kNumVoices; ++v) st.setPulseWidth (v, value);
                    break;
                case Param::Cutoff:    st.setCutoff (value); break;
                case Param::Resonance: st.setResonance (value); break;
                case Param::Attack:  for (int v = 0; v < kNumVoices; ++v) st.setAdsr (v, value); break;
                case Param::Decay:   for (int v = 0; v < kNumVoices; ++v) st.setAdsr (v, {}, value); break;
                case Param::Sustain: for (int v = 0; v < kNumVoices; ++v) st.setAdsr (v, {}, {}, sustainFor (part, i, v)); break;
                case Param::Release: for (int v = 0; v < kNumVoices; ++v) st.setAdsr (v, {}, {}, {}, value); break;
                case Param::Volume:  st.setVolume (volumeFor (part)); break;
            }
        }
    }
    if (p == Param::Volume)
        part.muted = false;
}

void Engine::nudgeParam (Part& part, Param p, double delta)
{
    std::lock_guard g (engineMutex);
    auto cur = part.norm[(size_t) p].value_or (getParam (part, p));
    setParam (part, p, cur + delta);
}

void Engine::programChange (int program, Part& part)
{
    std::lock_guard g (engineMutex);
    if (! presetList.empty())
    {
        selectPreset (part, program);
        return;
    }
    auto& progs = cfg.waveformPrograms;
    const auto& name = progs[(size_t) (program % (int) progs.size())];
    PatchUpdate u;
    u.waveform = name;
    applyPatch (part, u);
    info ("part " + std::to_string (part.index + 1) + ": program " + std::to_string (program) + " -> " + name);
}

void Engine::setProfile (const Profile& p)
{
    std::lock_guard g (engineMutex);
    cfg.profile = p;
    if (layout == "poly")
        parts[0].channel = p.channel;
}

std::string Engine::cycleFilterMode()
{
    std::lock_guard g (engineMutex);
    auto targets = knobParts();
    const auto& modes = cfg.filterCycle;
    const auto& cur = targets[0]->patch.filterMode;
    auto it = std::find (modes.begin(), modes.end(), cur);
    std::string next = it == modes.end() ? modes[0] : modes[(size_t) ((it - modes.begin() + 1) % (long) modes.size())];
    PatchUpdate u;
    u.filterMode = next;
    for (auto* part : targets)
        applyPatch (*part, u);
    info ("filter mode -> " + next);
    return next;
}

void Engine::panic()
{
    std::lock_guard g (engineMutex);
    {
        auto e = fl.edit();
        for (auto& st : fl.states)
        {
            for (int v = 0; v < kNumVoices; ++v)
                st.setGate (v, false);
            st.setVolume (0);
        }
    }
    for (auto& part : parts)
    {
        for (auto& s : part.slots)
        {
            s.gated = false;
            s.offTime = tick();
            s.offAt = now();
        }
        part.monoStack.clear();
        part.arpNotes.clear();
        part.arpRunning = false;
        part.arpGateOff.reset();
        part.heldByPedal.clear();
        part.pedal = false;
        part.muted = true;
    }
    info ("panic");
}

} // namespace msid
