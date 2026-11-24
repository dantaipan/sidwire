// MIDI engine: MIDI events -> SidState edits. C++ port of engine.py; the golden
// vectors (core/tests/vectors, made by tools/gen_vectors.py) hold the two to the
// same register writes.
#pragma once

#include "msid/Flusher.h"
#include "msid/Profile.h"

#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace msid {

enum class Param { PulseWidth, Cutoff, Resonance, Attack, Decay, Sustain, Release, Volume };
constexpr int kNumParams = 8;
const char* paramName (Param);
std::optional<Param> paramFromName (const std::string&);
int paramMax (Param);

/// Waveform names in program-change order (tri, saw, pulse, noise, combos).
const std::vector<std::string>& waveformNames();
std::optional<int> waveformBits (const std::string& name);
const std::vector<std::string>& filterModeNames();
std::optional<int> filterModeBits (const std::string& name);

struct Patch
{
    std::string name = "init";
    std::string waveform = "pulse";
    int pulseWidth = 2048, attack = 0, decay = 9, sustain = 10, release = 4;
    std::string filterMode = "lp";
    bool filterVoices = true;
    int cutoff = 2047, resonance = 0, volume = 15;

    int& get (Param p);
    int get (Param p) const { return const_cast<Patch*> (this)->get (p); }
};

/// A partial patch: only the keys that are set are applied (presets, program changes).
struct PatchUpdate
{
    std::optional<std::string> name, waveform, filterMode;
    std::optional<int> pulseWidth, attack, decay, sustain, release, cutoff, resonance, volume;
    std::optional<bool> filterVoices;

    static PatchUpdate full (const Patch&);
    bool hasAdsr() const { return attack || decay || sustain || release; }
};
using Preset = PatchUpdate;

/// presets.toml ([[preset]] tables). Throws std::runtime_error on unknown keys / values.
std::vector<Preset> parsePresets (const std::string& tomlText, const std::string& sourceName = "presets");
std::vector<Preset> loadPresets (const std::string& path);   // empty if the file is missing

struct EngineConfig
{
    std::string mode = "poly";      // poly | mono
    std::string layout = "poly";    // poly | split
    bool legato = true;
    bool hardRestart = false;
    bool muteAfterRelease = true;   // zero a voice's frequency once its release is over (6581 leak hum)
    std::string velocity = "sustain";   // off | sustain | volume (see engine.py)
    double velocityAmount = 1.0;        // 0..1: scale = 1 - amount * (1 - velocity/127)
    Patch patch;
    std::vector<std::string> waveformPrograms = waveformNames();
    Profile profile = genericProfile();   // the controller: channel, bend range, CC/note/bend mapping
    int knobsTarget = 0;            // controls-port params and actions go to: 0 = all parts, n = part n
    std::vector<std::string> filterCycle { "lp", "bp", "hp", "off" };   // filter_mode_next steps
};

struct MidiEvent
{
    enum class Type { NoteOn, NoteOff, ControlChange, PitchBend, ProgramChange };
    Type type = Type::NoteOn;
    int channel = 0;   // 0-15
    int data1 = 0;     // note / controller / program / bend (-8192..8191)
    int data2 = 0;     // velocity / value

    static MidiEvent noteOn (int ch, int note, int vel) { return { Type::NoteOn, ch, note, vel }; }
    static MidiEvent noteOff (int ch, int note) { return { Type::NoteOff, ch, note, 0 }; }
    static MidiEvent cc (int ch, int num, int val) { return { Type::ControlChange, ch, num, val }; }
    static MidiEvent bend (int ch, int pitch) { return { Type::PitchBend, ch, pitch, 0 }; }
    static MidiEvent program (int ch, int prog) { return { Type::ProgramChange, ch, prog, 0 }; }
};

struct Slot
{
    int sid = 0, voice = 0;
    std::optional<int> note;   // assigned (sounding or releasing)
    bool gated = false;
    long long onTime = 0, offTime = 0;
    double offAt = 0;          // clock time of the note-off (seconds, for muteAfterRelease)
    double vel = 1.0;          // velocity scale of the note (velocity "sustain")
};

struct Part
{
    int index = 0;
    int channel = 0;           // 1-16, 0 = omni
    std::vector<int> sids;
    std::vector<Slot> slots;
    Patch patch;
    double bend = 0.0;
    bool pedal = false;
    std::set<int> heldByPedal;
    std::vector<int> monoStack;
    bool muted = false;
    std::array<std::optional<double>, kNumParams> norm {};   // fractional encoder positions
    double vel = 1.0;          // velocity scale of the last note (velocity "volume")
    int presetIndex = -1;      // last preset selected (preset_next / preset_prev)
};

class Engine
{
public:
    Engine (Flusher& fl, EngineConfig cfg, std::vector<Preset> presets = {});

    void initChip();
    void applyPatch (Part& part, const PatchUpdate& patch, bool force = false);
    const Preset* selectPreset (Part& part, int index);
    const Preset* selectPreset (Part& part, const std::string& name);   // throws std::out_of_range

    Part* partFor (int channel);
    /// A MIDI message from the notes port (keys) or the controls port (knobs / buttons).
    void handle (const MidiEvent& e, Port port = Port::Notes);

    void noteOn (int note, int velocity, Part& part);
    void noteOff (int note, Part& part);
    void pitchBend (int pitch, Part& part);
    /// A CC from the notes port (channel0 = 0-15, -1 = any).
    void controlChange (int cc, int value, Part& part, int channel0 = -1);
    void setPedal (Part& part, bool down);
    void programChange (int program, Part& part);
    /// Replace the controller profile while running (MIDI learn). Notes keep playing.
    void setProfile (const Profile& p);
    /// Next filter mode in filterCycle for the knob target parts; returns it.
    std::string cycleFilterMode();
    void panic();
    /// Mute voices whose release has run out (see engine.py). Call every ~20-50 ms.
    void service();
    /// Clock in seconds; replaceable (golden vectors use a fake one).
    std::function<double()> now;

    /// Change the velocity response at run time; sounding notes go back to full level.
    /// Throws std::invalid_argument for an unknown mode.
    void setVelocity (const std::string& mode, double amount);
    const std::string& velocityMode() const { return velMode; }
    double velocityAmount() const { return velAmount; }

    double getParam (const Part& part, Param p) const;
    void setParam (Part& part, Param p, double norm);
    void nudgeParam (Part& part, Param p, double delta);

    std::vector<Part> parts;
    std::string layout;
    const std::vector<Preset>& presets() const { return presetList; }
    const EngineConfig& config() const { return cfg; }
    std::recursive_mutex& mutex() { return engineMutex; }
    std::function<void (const std::string&)> log;

private:
    long long tick() { return ++clock; }
    void unmute (Part& part);
    Slot& alloc (Part& part, int note);
    void startVoice (Part& part, Slot& slot, int note, std::optional<int> velocity = {});
    void setVelocityFields (const std::string& mode, double amount);
    double velScale (int velocity) const { return 1.0 - velAmount * (1.0 - velocity / 127.0); }
    int sustainFor (const Part& part, int sidIndex, int voice) const;
    int volumeFor (const Part& part) const;
    void releaseVoice (Slot& slot);
    void info (const std::string& s) { if (log) log (s); }
    std::vector<const Control*> match (Port port, Control::Kind kind, int channel0, int number = 0) const;
    void applyControl (const Control& c, int value, const std::vector<Part*>& targets);
    void action (const std::string& name);
    std::vector<Part*> knobParts();

    Flusher& fl;
    EngineConfig cfg;
    std::vector<Preset> presetList;
    std::vector<int> knobPartIdx;   // part indices
    bool mono;
    std::string velMode = "off";
    double velAmount = 0.0;
    std::map<std::pair<int, int>, Slot*> slotAt;   // (sid, voice) -> slot
    long long clock = 0;
    std::recursive_mutex engineMutex;
};

} // namespace msid
