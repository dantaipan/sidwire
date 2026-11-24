// Parity with the Python implementation: replay the golden vectors made by
// tools/gen_vectors.py and require identical register writes, flush by flush.
#include "msid/Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace msid;

namespace {

std::vector<uint8_t> unhex (const std::string& s)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back ((uint8_t) std::stoi (s.substr (i, 2), nullptr, 16));
    return out;
}

std::string hex (const std::vector<uint8_t>& d)
{
    std::string s;
    char b[3];
    for (auto x : d) { std::snprintf (b, sizeof b, "%02X", x); s += b; }
    return s;
}

std::string show (const std::vector<Write>& ws)
{
    std::string s;
    for (auto& w : ws)
    {
        char a[8];
        std::snprintf (a, sizeof a, "%04X", w.address);
        s += std::string ("  W ") + a + " " + hex (w.data) + "\n";
    }
    return s.empty() ? "  (none)\n" : s;
}

struct Runner
{
    std::map<std::string, std::string> conf;
    DryRunTransport t;
    std::unique_ptr<Flusher> fl;
    std::unique_ptr<Engine> e;
    double fakeTime = 0;

    explicit Runner (const std::string& configLine)
    {
        std::istringstream in (configLine);
        std::string kv;
        in >> kv;   // "config"
        while (in >> kv)
        {
            auto eq = kv.find ('=');
            conf[kv.substr (0, eq)] = kv.substr (eq + 1);
        }
        int nsids = std::stoi (conf["nsids"]);
        std::vector<SidState> states;
        for (int i = 0; i < nsids; ++i)
            states.emplace_back ((uint16_t) (0xD400 + 0x20 * i));
        fl = std::make_unique<Flusher> (t, std::move (states), 5.0, conf["strict"] == "1");
        EngineConfig c;
        c.layout = conf["layout"];
        c.mode = conf["mode"];
        c.hardRestart = conf["hard_restart"] == "1";
        c.legato = conf["legato"] == "1";
        c.knobsTarget = std::stoi (conf["target"]);
        auto prof = conf.count ("profile") ? conf["profile"] : std::string ("oxygen-49-mkv");
        std::string dir = prof == "test-all" ? MSID_TEST_PROFILES_DIR : MSID_PROFILES_DIR;
        c.profile = loadProfile (dir + "/" + prof + ".toml");
        c.profile.channel = std::stoi (conf["channel"]);
        if (conf.count ("vel")) c.velocity = conf["vel"];
        if (conf.count ("detune")) c.unisonDetune = std::stoi (conf["detune"]);
        if (conf.count ("arate")) c.arpRate = conf["arate"];
        if (conf.count ("apat"))
        {
            c.arpPattern = conf["apat"];
            std::replace (c.arpPattern.begin(), c.arpPattern.end(), '_', ' ');
        }
        if (conf.count ("aoct")) c.arpOctaves = std::stoi (conf["aoct"]);
        if (conf.count ("agate")) c.arpGate = std::stoi (conf["agate"]) / 100.0;
        if (conf.count ("bpm")) c.bpm = std::stoi (conf["bpm"]);
        if (conf.count ("vamt")) c.velocityAmount = std::stoi (conf["vamt"]) / 100.0;
        std::vector<Preset> presets;
        if (conf["presets"] == "1")
            presets = loadPresets (MSID_PRESETS_FILE);
        e = std::make_unique<Engine> (*fl, c, presets);
        e->now = [this] { return fakeTime; };
    }

    std::vector<Write> event (const std::string& line)
    {
        std::istringstream in (line);
        std::string k;
        in >> k;
        int a = 0, b = 0, c = 0, d = 0, f = 0;
        in >> a >> b >> c >> d >> f;
        if (k == "init")        e->initChip();
        else if (k == "flush")  { fl->flush(); return t.take(); }
        else if (k == "on")     e->handle (MidiEvent::noteOn (a, b, c));
        else if (k == "off")    e->handle (MidiEvent::noteOff (a, b));
        else if (k == "cc")     e->handle (MidiEvent::cc (a, b, c));
        else if (k == "bend")   e->handle (MidiEvent::bend (a, b));
        else if (k == "prog")   e->handle (MidiEvent::program (a, b));
        else if (k == "mcc")    e->handle (MidiEvent::cc (0, a, b), Port::Controls);
        else if (k == "mbend")  e->handle (MidiEvent::bend (a, b), Port::Controls);
        else if (k == "mnote")  e->handle (MidiEvent::noteOn (a, b, c), Port::Controls);
        else if (k == "panic")  e->panic();
        else if (k == "clock")  fakeTime = a / 1000.0;   // ms
        else if (k == "service") e->service();
        else if (k == "mclock")  e->handle (MidiEvent::clock());
        else if (k == "mstart")  e->handle (MidiEvent::start());
        else if (k == "vmode")   e->setVoiceMode (voiceModeNames().at ((size_t) a));
        else if (k == "arpset")  e->setArp (arpRateNames().at ((size_t) a), arpPatternNames().at ((size_t) b), c, d / 100.0, (double) f);
        else if (k == "velmode") e->setVelocity (std::vector<std::string> { "off", "sustain", "volume" }.at ((size_t) a), b / 100.0);
        else FAIL ("unknown vector event: " << line);
        return {};
    }
};

void runFile (const std::string& file)
{
    std::ifstream f (std::string (MSID_VECTORS_DIR) + "/" + file);
    REQUIRE (f.good());
    std::vector<std::string> lines;
    for (std::string l; std::getline (f, l);)
        if (! l.empty())
            lines.push_back (l);

    int scenarios = 0, writes = 0;
    for (size_t i = 0; i < lines.size();)
    {
        REQUIRE (lines[i].rfind ("scenario ", 0) == 0);
        std::string name = lines[i].substr (9);
        Runner r (lines[i + 1]);
        i += 2;
        INFO ("scenario " << name);
        int step = 0;
        while (lines[i] != "end")
        {
            const auto& l = lines[i];
            if (l.rfind ("regs ", 0) == 0)
            {
                std::istringstream in (l);
                std::string k, h;
                int idx;
                in >> k >> idx >> h;
                auto& regs = r.fl->states[(size_t) idx].regs;
                CHECK (hex ({ regs.begin(), regs.end() }) == h);
                ++i;
                continue;
            }
            ++step;
            auto got = r.event (l);
            ++i;
            std::vector<Write> want;
            while (lines[i].rfind ("W ", 0) == 0)
            {
                Write w;
                w.address = (uint16_t) std::stoi (lines[i].substr (2, 4), nullptr, 16);
                w.data = unhex (lines[i].substr (7));
                want.push_back (w);
                ++i;
            }
            writes += (int) want.size();
            if (got != want)
                FAIL ("step " << step << " (" << l << ") writes differ\nexpected:\n"
                              << show (want) << "got:\n" << show (got));
        }
        ++i;
        ++scenarios;
    }
    INFO (file << ": " << scenarios << " scenarios, " << writes << " writes");
    CHECK (scenarios > 0);
}

} // namespace

TEST_CASE ("parity: handwritten scenarios match Python", "[parity]") { runFile ("handwritten.txt"); }
TEST_CASE ("parity: randomized scenarios match Python", "[parity]") { runFile ("randomized.txt"); }
