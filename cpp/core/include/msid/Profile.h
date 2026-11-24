// Controller profiles: how one MIDI keyboard / controller talks to the bridge.
// C++ port of profiles.py; reads the same files (profiles/*.toml, format in generic.toml).
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace msid {

/// Where a message came from: the keys' input, or a second input for knobs / buttons.
enum class Port { Notes, Controls };

struct ProfileError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Control
{
    enum class Kind { CC, Note, PitchBend };
    Kind kind = Kind::CC;
    int number = 0;                  // CC / note number (0 for pitch bend)
    Port port = Port::Notes;
    int channel = 0;                 // 1-16, 0 = any
    std::string param;               // a sound parameter ...
    std::string action;              // ... or an action
    std::string mode = "absolute";   // CC params: absolute | relative-signed | relative-twos | relative-offset
    double steps = 128.0;            // relative modes: ticks for a full sweep

    /// channel0 = MIDI channel 0-15 of the message, -1 = any.
    bool matches (Port p, Kind k, int channel0, int num) const
    {
        return port == p && kind == k && number == num && (channel == 0 || channel0 < 0 || channel == channel0 + 1);
    }
    int decodeRelative (int value) const;
    std::string describe() const;
    bool operator== (const Control& o) const
    {
        return kind == o.kind && number == o.number && port == o.port && channel == o.channel && param == o.param
            && action == o.action && mode == o.mode && ! (steps < o.steps) && ! (o.steps < steps);
    }
};

struct Profile
{
    std::string name;
    std::string description;
    std::string notesPort, controlsPort;   // MIDI input names (substring); bridge.py / "auto" in the plugin
    int channel = 0;                       // keys: 1-16, 0 = any
    double bendRange = 2.0;
    std::vector<int> ignoreChannels;       // notes on these channels never play the SID
    std::vector<Control> controls;
    bool operator== (const Profile& o) const
    {
        return name == o.name && description == o.description && notesPort == o.notesPort && controlsPort == o.controlsPort
            && channel == o.channel && ! (bendRange < o.bendRange) && ! (o.bendRange < bendRange)
            && ignoreChannels == o.ignoreChannels && controls == o.controls;
    }
};

const std::vector<std::string>& profileParams();
const std::vector<std::string>& profileActions();
const std::vector<std::string>& profileModes();

/// Throws ProfileError with "source: ..." messages (the same rules as profiles.py).
Profile parseProfile (const std::string& tomlText, const std::string& source = "profile");
Profile loadProfile (const std::string& path);   // throws ProfileError if missing or invalid
/// The profile as a TOML file that parseProfile reads back to an equal Profile.
std::string profileToToml (const Profile& p, const std::string& headerComment = {});
/// Short label for what triggers a control, for the UI ("CC74", "CC16 ctl", "Bend ch1 ctl", "Note 40 ch10").
std::string shortTrigger (const Control& c);
/// The built-in default (identical to profiles/generic.toml; a test checks).
const Profile& genericProfile();

} // namespace msid
