#include "msid/Profile.h"

#include "Toml.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace msid {

namespace {

const std::vector<std::string> kParams { "pulse_width", "cutoff", "resonance", "attack", "decay", "sustain", "release", "volume" };
const std::vector<std::string> kActions { "sustain_pedal", "filter_mode_next", "preset_next", "preset_prev", "waveform_next", "panic",
                                          "voice_mode_next" };
const std::vector<std::string> kModes { "absolute", "relative-signed", "relative-twos", "relative-offset" };

bool contains (const std::vector<std::string>& v, const std::string& s) { return std::find (v.begin(), v.end(), s) != v.end(); }

std::string join (const std::vector<std::string>& v)
{
    std::string out;
    for (auto& s : v)
        out += (out.empty() ? "" : ", ") + s;
    return out;
}

void allowKeys (const toml::Table& t, const std::set<std::string>& allowed, const std::string& where)
{
    std::vector<std::string> bad;
    for (auto& [k, v] : t)
        if (! allowed.count (k))
            bad.push_back (k);
    if (! bad.empty())
        throw ProfileError (where + ": unknown key(s) " + join (bad) + " (allowed: "
                            + join (std::vector<std::string> (allowed.begin(), allowed.end())) + ")");
}

int needInt (const toml::Value& v, const std::string& where, int lo, int hi)
{
    if (! v.isInt() || std::get<long long> (v.v) < lo || std::get<long long> (v.v) > hi)
        throw ProfileError (where + ": must be a whole number " + std::to_string (lo) + "-" + std::to_string (hi));
    return (int) std::get<long long> (v.v);
}

std::string needStr (const toml::Value& v, const std::string& where)
{
    if (! v.isString())
        throw ProfileError (where + ": must be a string");
    return std::get<std::string> (v.v);
}

Control parseControl (const toml::Table& t, const std::string& where)
{
    allowKeys (t, { "cc", "note", "pitchbend", "port", "channel", "param", "action", "mode", "steps" }, where);
    int triggers = (int) t.count ("cc") + (int) t.count ("note") + (int) t.count ("pitchbend");
    if (triggers != 1)
        throw ProfileError (where + ": needs exactly one of cc = N, note = N or pitchbend = true");
    Control c;
    if (t.count ("pitchbend"))
    {
        auto& v = t.at ("pitchbend");
        if (! v.isBool() || ! std::get<bool> (v.v))
            throw ProfileError (where + ": pitchbend must be true");
        c.kind = Control::Kind::PitchBend;
    }
    else if (t.count ("cc"))
    {
        c.kind = Control::Kind::CC;
        c.number = needInt (t.at ("cc"), where + ": cc", 0, 127);
    }
    else
    {
        c.kind = Control::Kind::Note;
        c.number = needInt (t.at ("note"), where + ": note", 0, 127);
    }
    if (t.count ("port"))
    {
        auto p = needStr (t.at ("port"), where + ": port");
        if (p != "notes" && p != "controls")
            throw ProfileError (where + ": port must be \"notes\" or \"controls\", not '" + p + "'");
        c.port = p == "notes" ? Port::Notes : Port::Controls;
    }
    if (t.count ("channel"))
        c.channel = needInt (t.at ("channel"), where + ": channel", 0, 16);
    if (t.count ("param") == t.count ("action"))
        throw ProfileError (where + ": needs exactly one of param = \"...\" or action = \"...\"");
    if (t.count ("param"))
    {
        c.param = needStr (t.at ("param"), where + ": param");
        if (! contains (kParams, c.param))
            throw ProfileError (where + ": unknown param '" + c.param + "' (one of " + join (kParams) + ")");
    }
    else
    {
        c.action = needStr (t.at ("action"), where + ": action");
        if (! contains (kActions, c.action))
            throw ProfileError (where + ": unknown action '" + c.action + "' (one of " + join (kActions) + ")");
    }
    if (t.count ("mode"))
    {
        c.mode = needStr (t.at ("mode"), where + ": mode");
        if (! contains (kModes, c.mode))
            throw ProfileError (where + ": unknown mode '" + c.mode + "' (one of " + join (kModes) + ")");
    }
    if (c.mode != "absolute" && ! (c.kind == Control::Kind::CC && ! c.param.empty()))
        throw ProfileError (where + ": relative modes are for CC knobs that set a param");
    if (t.count ("steps"))
    {
        auto& v = t.at ("steps");
        if (! v.isNumber() || v.number() <= 0)
            throw ProfileError (where + ": steps must be a positive number");
        if (c.mode == "absolute")
            throw ProfileError (where + ": steps only applies to relative modes");
        c.steps = v.number();
    }
    if (c.kind == Control::Kind::Note && ! c.param.empty())
        throw ProfileError (where + ": a note (pad / button) triggers an action, not a param");
    if (c.kind == Control::Kind::PitchBend && ! c.action.empty())
        throw ProfileError (where + ": pitch bend sets a param, not an action");
    if (c.action == "sustain_pedal" && c.kind != Control::Kind::CC)
        throw ProfileError (where + ": sustain_pedal needs a CC");
    return c;
}

} // namespace

const std::vector<std::string>& profileParams() { return kParams; }
const std::vector<std::string>& profileActions() { return kActions; }
const std::vector<std::string>& profileModes() { return kModes; }

int Control::decodeRelative (int value) const
{
    if (mode == "relative-signed")   // Mackie V-Pot: bit 6 = sign, bits 0-5 = ticks
        return (value & 0x40) ? -(value & 0x3F) : (value & 0x3F);
    if (mode == "relative-twos")     // two's complement: 1..63 up, 127..65 down
        return value < 64 ? value : value - 128;
    return value - 64;               // relative-offset: 64 = no change
}

std::string Control::describe() const
{
    std::string trig = kind == Kind::CC ? "CC" + std::to_string (number)
                     : kind == Kind::Note ? "note " + std::to_string (number) : "pitch bend";
    if (channel)
        trig += " ch" + std::to_string (channel);
    if (port == Port::Controls)
        trig += " (controls port)";
    std::string what = ! param.empty() ? param + (mode != "absolute" ? " [" + mode + "]" : "") : action;
    return trig + " -> " + what;
}

Profile parseProfile (const std::string& text, const std::string& source)
{
    toml::Document d;
    try
    {
        d = toml::parse (text, source);
    }
    catch (const toml::Error& e)
    {
        throw ProfileError (e.what());
    }
    allowKeys (d.root, { "name", "description" }, source);
    if (d.tables.count ("control"))
        throw ProfileError (source + ": use [[control]] (double brackets) for each control");
    for (auto& [name, t] : d.tables)
        if (name != "ports" && name != "keys")
            throw ProfileError (source + ": unknown table [" + name + "] (allowed: ports, keys, [[control]])");
    for (auto& [name, t] : d.arrays)
        if (name != "control")
            throw ProfileError (source + ": unknown table [[" + name + "]] (allowed: [[control]])");

    Profile p;
    if (! d.root.count ("name"))
        throw ProfileError (source + ": needs a name = \"...\"");
    p.name = needStr (d.root.at ("name"), source + ": name");
    if (d.root.count ("description"))
        p.description = needStr (d.root.at ("description"), source + ": description");

    if (d.tables.count ("ports"))
    {
        auto& t = d.tables.at ("ports");
        allowKeys (t, { "notes", "controls" }, source + " [ports]");
        if (t.count ("notes")) p.notesPort = needStr (t.at ("notes"), source + " [ports] notes");
        if (t.count ("controls")) p.controlsPort = needStr (t.at ("controls"), source + " [ports] controls");
    }
    if (d.tables.count ("keys"))
    {
        auto& t = d.tables.at ("keys");
        allowKeys (t, { "channel", "bend_range", "ignore_channels" }, source + " [keys]");
        if (t.count ("channel"))
            p.channel = needInt (t.at ("channel"), source + " [keys] channel", 0, 16);
        if (t.count ("bend_range"))
        {
            auto& v = t.at ("bend_range");
            if (! v.isNumber() || v.number() <= 0 || v.number() > 48)
                throw ProfileError (source + " [keys]: bend_range must be a number of semitones, 1-48");
            p.bendRange = v.number();
        }
        if (t.count ("ignore_channels"))
        {
            auto& v = t.at ("ignore_channels");
            if (! v.isArray())
                throw ProfileError (source + " [keys]: ignore_channels must be a list, e.g. [10]");
            for (auto& c : std::get<toml::Array> (v.v))
                p.ignoreChannels.push_back (needInt (c, source + " [keys] ignore_channels", 1, 16));
        }
    }
    if (d.arrays.count ("control"))
    {
        auto& list = d.arrays.at ("control");
        for (size_t i = 0; i < list.size(); ++i)
            p.controls.push_back (parseControl (list[i], source + " control #" + std::to_string (i + 1)));
    }
    return p;
}

Profile loadProfile (const std::string& path)
{
    std::ifstream f (path);
    if (! f)
        throw ProfileError ("cannot read controller profile " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parseProfile (ss.str(), path);
}

namespace {
std::string quote (const std::string& s)
{
    std::string out = "\"";
    for (char ch : s)
    {
        if (ch == '"' || ch == '\\')
            out += '\\';
        if (ch == '\n') { out += "\\n"; continue; }
        out += ch;
    }
    return out + "\"";
}

std::string number (double v)
{
    char buf[32];
    if (v == (double) (long long) v)
        std::snprintf (buf, sizeof buf, "%lld", (long long) v);
    else
        std::snprintf (buf, sizeof buf, "%.6g", v);
    return buf;
}
} // namespace

std::string profileToToml (const Profile& p, const std::string& headerComment)
{
    std::string out;
    if (! headerComment.empty())
    {
        std::istringstream in (headerComment);
        for (std::string line; std::getline (in, line);)
            out += "# " + line + "\n";
        out += "\n";
    }
    out += "name = " + quote (p.name) + "\n";
    if (! p.description.empty())
        out += "description = " + quote (p.description) + "\n";
    out += "\n[ports]\nnotes = " + quote (p.notesPort) + "\ncontrols = " + quote (p.controlsPort) + "\n";
    out += "\n[keys]\nchannel = " + std::to_string (p.channel) + "\nbend_range = " + number (p.bendRange) + "\nignore_channels = [";
    for (size_t i = 0; i < p.ignoreChannels.size(); ++i)
        out += (i ? ", " : "") + std::to_string (p.ignoreChannels[i]);
    out += "]\n";
    for (auto& c : p.controls)
    {
        out += "\n[[control]]\n";
        if (c.port == Port::Controls)
            out += "port = \"controls\"\n";
        if (c.kind == Control::Kind::CC) out += "cc = " + std::to_string (c.number) + "\n";
        else if (c.kind == Control::Kind::Note) out += "note = " + std::to_string (c.number) + "\n";
        else out += "pitchbend = true\n";
        if (c.channel)
            out += "channel = " + std::to_string (c.channel) + "\n";
        if (c.mode != "absolute")
            out += "mode = " + quote (c.mode) + "\n";
        if (c.mode != "absolute" && c.steps != 128.0)
            out += "steps = " + number (c.steps) + "\n";
        out += ! c.param.empty() ? "param = " + quote (c.param) + "\n" : "action = " + quote (c.action) + "\n";
    }
    return out;
}

std::string shortTrigger (const Control& c)
{
    std::string s = c.kind == Control::Kind::CC ? "CC" + std::to_string (c.number)
                  : c.kind == Control::Kind::Note ? "Note " + std::to_string (c.number) : std::string ("Bend");
    if (c.channel)
        s += " ch" + std::to_string (c.channel);
    if (c.port == Port::Controls)
        s += " ctl";
    return s;
}

const Profile& genericProfile()
{
    static const Profile g = [] {
        Profile p;
        p.name = "Generic MIDI keyboard";
        p.description = "Any MIDI keyboard: notes on any channel, pitch bend, mod wheel, sustain pedal and common sound CCs.";
        auto cc = [&p] (int n, const char* param, const char* action = "") {
            Control c;
            c.kind = Control::Kind::CC;
            c.number = n;
            c.param = param;
            c.action = action;
            p.controls.push_back (c);
        };
        cc (1, "pulse_width");
        cc (74, "cutoff");
        cc (71, "resonance");
        cc (73, "attack");
        cc (75, "decay");
        cc (76, "sustain");
        cc (72, "release");
        cc (7, "volume");
        cc (64, "", "sustain_pedal");
        return p;
    }();
    return g;
}

} // namespace msid
