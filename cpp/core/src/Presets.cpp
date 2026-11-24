// Minimal TOML-subset reader for presets.toml: [[preset]] tables with
// `key = "string" | integer | true/false` lines and # comments.
// Mirrors engine.py load_presets(): unknown keys / waveforms / filter modes are errors.
#include "msid/Engine.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace msid {

namespace {

std::string trim (const std::string& s)
{
    size_t a = s.find_first_not_of (" \t\r\n");
    if (a == std::string::npos)
        return {};
    size_t b = s.find_last_not_of (" \t\r\n");
    return s.substr (a, b - a + 1);
}

/// Strip a # comment that is not inside a "string".
std::string stripComment (const std::string& line)
{
    bool inStr = false;
    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];
        if (c == '"' && (i == 0 || line[i - 1] != '\\'))
            inStr = ! inStr;
        else if (c == '#' && ! inStr)
            return line.substr (0, i);
    }
    return line;
}

struct Value
{
    enum Kind { Str, Int, Bool } kind;
    std::string s;
    long long i = 0;
    bool b = false;
};

Value parseValue (const std::string& raw, const std::string& where)
{
    auto v = trim (raw);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        return { Value::Str, v.substr (1, v.size() - 2) };
    if (v == "true" || v == "false")
        return { Value::Bool, {}, 0, v == "true" };
    try
    {
        size_t used = 0;
        long long n = std::stoll (v, &used, 0);   // accepts 0x...
        if (used == v.size())
            return { Value::Int, {}, n };
    }
    catch (...) {}
    throw std::runtime_error (where + ": cannot parse value '" + v + "'");
}

} // namespace

std::vector<Preset> parsePresets (const std::string& text, const std::string& src)
{
    std::vector<Preset> out;
    std::istringstream in (text);
    std::string line;
    int lineNo = 0;
    bool inPreset = false;
    while (std::getline (in, line))
    {
        ++lineNo;
        auto l = trim (stripComment (line));
        if (l.empty())
            continue;
        if (l == "[[preset]]")
        {
            out.emplace_back();
            inPreset = true;
            continue;
        }
        if (l.front() == '[')   // some other table: ignore its keys
        {
            inPreset = false;
            continue;
        }
        if (! inPreset)
            continue;
        auto eq = l.find ('=');
        if (eq == std::string::npos)
            throw std::runtime_error (src + ":" + std::to_string (lineNo) + ": expected key = value");
        auto key = trim (l.substr (0, eq));
        auto where = src + ":" + std::to_string (lineNo);
        auto val = parseValue (l.substr (eq + 1), where);
        auto& p = out.back();

        auto needStr = [&] { if (val.kind != Value::Str) throw std::runtime_error (where + ": '" + key + "' must be a string"); return val.s; };
        auto needInt = [&] { if (val.kind != Value::Int) throw std::runtime_error (where + ": '" + key + "' must be an integer"); return (int) val.i; };

        if (key == "name") p.name = needStr();
        else if (key == "waveform")
        {
            auto w = needStr();
            if (! waveformBits (w))
                throw std::runtime_error (where + ": unknown waveform '" + w + "'");
            p.waveform = w;
        }
        else if (key == "filter_mode")
        {
            auto m = needStr();
            if (! filterModeBits (m))
                throw std::runtime_error (where + ": unknown filter_mode '" + m + "'");
            p.filterMode = m;
        }
        else if (key == "filter_voices")
        {
            if (val.kind != Value::Bool)
                throw std::runtime_error (where + ": 'filter_voices' must be true/false");
            p.filterVoices = val.b;
        }
        else if (key == "pulse_width") p.pulseWidth = needInt();
        else if (key == "attack") p.attack = needInt();
        else if (key == "decay") p.decay = needInt();
        else if (key == "sustain") p.sustain = needInt();
        else if (key == "release") p.release = needInt();
        else if (key == "cutoff") p.cutoff = needInt();
        else if (key == "resonance") p.resonance = needInt();
        else if (key == "volume") p.volume = needInt();
        else
            throw std::runtime_error (where + ": preset has unknown key '" + key + "'");
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (! out[i].name)
            out[i].name = "preset " + std::to_string (i);
    return out;
}

std::vector<Preset> loadPresets (const std::string& path)
{
    std::ifstream f (path);
    if (! f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return parsePresets (ss.str(), path);
}

} // namespace msid
