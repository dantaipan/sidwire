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
    mono = cfg.mode == "mono";
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
        if (mono)
            p.slots.push_back (makeSlot (p.sids[0], 0));
        else
            for (int s : p.sids)
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
    if (cfg.filterCycle.empty())
        throw std::invalid_argument ("filterCycle is empty");
    for (auto& m : cfg.filterCycle)
        if (! filterModeBits (m))
            throw std::invalid_argument ("unknown filter mode in filterCycle: '" + m + "'");
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
        st.setNote (v, note, part.bend);
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
    if (! cfg.muteAfterRelease)
        return;
    std::lock_guard g (engineMutex);
    double t = now();
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
    if (mono)
    {
        auto& slot = part.slots[0];
        auto& stack = part.monoStack;
        stack.erase (std::remove (stack.begin(), stack.end(), note), stack.end());
        stack.push_back (note);
        if (slot.gated && cfg.legato)
        {
            auto e = fl.edit();
            fl.states[(size_t) slot.sid].setNote (slot.voice, note, part.bend);
            slot.note = note;
        }
        else
        {
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
    if (mono)
    {
        auto& slot = part.slots[0];
        auto& stack = part.monoStack;
        stack.erase (std::remove (stack.begin(), stack.end(), note), stack.end());
        if (slot.note != note)
            return;
        if (! stack.empty() && cfg.legato)
        {
            int prev = stack.back();
            auto e = fl.edit();
            fl.states[(size_t) slot.sid].setNote (slot.voice, prev, part.bend);
            slot.note = prev;
        }
        else if (! stack.empty())
        {
            startVoice (part, slot, stack.back());
        }
        else
        {
            releaseVoice (slot);
        }
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
            fl.states[(size_t) s.sid].setNote (s.voice, *s.note, part.bend);
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
        part.heldByPedal.clear();
        part.pedal = false;
        part.muted = true;
    }
    info ("panic");
}

} // namespace msid
