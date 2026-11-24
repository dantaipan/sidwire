// SID Bridge plugin, headless: the real processor against a fake C64U on localhost.
// Checks host-parameter -> SID, MIDI -> SID, MIDI -> parameter write-back, programs,
// the shared instance lock and state round trips.
#include "FakeC64U.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <catch2/catch_session.hpp>
#include <set>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using msid::test::FakeC64U;
using Catch::Matchers::ContainsSubstring;

namespace {

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

/// Pump messages until cond() or timeout.
template <typename F>
bool waitFor (F cond, int timeoutMs = 3000)
{
    for (int t = 0; t < timeoutMs; t += 10)
    {
        if (cond())
            return true;
        pump (10);
    }
    return cond();
}

int cutoffAt (FakeC64U& f, int base = 0xD400) { return (f.at (base + 0x15) & 7) | (f.at (base + 0x16) << 3); }

struct Rig
{
    FakeC64U fake;
    SidBridgeProcessor proc;

    /// controlsPort "-" = none, so a test never opens a real controller's port.
    explicit Rig (const juce::String& layout = "poly", const juce::String& profile = "generic",
                  const juce::String& presetsFile = {}, const juce::String& controlsPort = "-")
    {
        BridgeSettings s;
        s.host = "127.0.0.1:" + juce::String (fake.port);
        s.transport = "http";   // the fake only speaks HTTP
        s.layout = layout;
        s.profile = profile;
        s.presetsFile = presetsFile;
        s.encoderPort = controlsPort;
        proc.bridge().setSettings (s);
        REQUIRE (waitFor ([&] { return proc.bridge().status().state == BridgeStatus::State::connected; }));
        REQUIRE (waitFor ([&] { return fake.at (0xD418) == 0x1F && fake.at (0xD438) == 0x1F; }));
    }

    void midi (const juce::MidiMessage& m)
    {
        juce::AudioBuffer<float> buf (2, 64);
        juce::MidiBuffer mb;
        mb.addEvent (m, 0);
        proc.processBlock (buf, mb);
        CHECK (buf.getMagnitude (0, 64) == 0.0f);   // the plugin itself is silent
    }

    float plain (const char* id)
    {
        auto* p = proc.apvts.getParameter (id);
        return p->convertFrom0to1 (p->getValue());
    }
    void setPlain (const char* id, float v)
    {
        auto* p = proc.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    }
};

/// A component anywhere in the window by its component ID.
juce::Component* findById (juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id)
        return &root;
    for (auto* c : root.getChildren())
        if (auto* f = findById (*c, id))
            return f;
    return nullptr;
}

/// A temporary user profiles folder for one test.
struct UserProfiles
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("msid-profiles-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
    UserProfiles() { dir.createDirectory(); Profiles::setUserDirForTests (dir); }
    ~UserProfiles() { Profiles::setUserDirForTests ({}); dir.deleteRecursively(); }
    void write (const juce::String& file, const juce::String& text) { dir.getChildFile (file).replaceWithText (text); }
};

} // namespace

TEST_CASE ("plugin connects and writes the init patch to both SIDs", "[plugin]")
{
    Rig r;
    CHECK (r.fake.at (0xD404) == 0x40);       // pulse, gate off
    CHECK (cutoffAt (r.fake) == 2047);
    CHECK (r.proc.bridge().status().voices.size() == 6);
}

TEST_CASE ("host parameter moves reach the SID", "[plugin]")
{
    Rig r;
    r.setPlain ("cutoff", 100.0f);
    CHECK (waitFor ([&] { return cutoffAt (r.fake) == 100 && cutoffAt (r.fake, 0xD420) == 100; }));
    r.setPlain ("waveform", 1.0f);            // saw
    CHECK (waitFor ([&] { return r.fake.at (0xD404) == 0x20; }));
    r.setPlain ("filter_mode", 0.0f);         // off -> voices un-routed
    CHECK (waitFor ([&] { return (r.fake.at (0xD417) & 7) == 0 && (r.fake.at (0xD418) & 0x70) == 0; }));
}

TEST_CASE ("velocity parameters: sustain by default, volume, amount", "[plugin]")
{
    Rig r;
    CHECK (r.plain ("velocity") == 1.0f);           // sustain
    CHECK (r.plain ("velocity_amount") == 100.0f);
    r.midi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 64));
    CHECK (waitFor ([&] { return r.fake.at (0xD404) == 0x41 && r.fake.at (0xD406) == 0x54; }));   // sustain 10*64/127 -> 5
    r.setPlain ("velocity", 2.0f);                  // volume: sustain back to the patch's
    CHECK (waitFor ([&] { return r.fake.at (0xD406) == 0xA4; }));
    r.midi (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 32));
    CHECK (waitFor ([&] { return (r.fake.at (0xD418) & 0x0F) == 4 && (r.fake.at (0xD438) & 0x0F) == 4; }));   // 15*32/127
    CHECK (r.fake.at (0xD40D) == 0xA4);
    r.setPlain ("velocity_amount", 0.0f);           // amount 0: every note at full level
    CHECK (waitFor ([&] { return (r.fake.at (0xD418) & 0x0F) == 15; }));
    r.midi (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 1));
    pump (100);
    CHECK ((r.fake.at (0xD418) & 0x0F) == 15);
}

TEST_CASE ("pad on channel 10 steps the filter mode and shows it in the parameter", "[plugin]")
{
    Rig r ("poly", "oxygen-49-mkv");
    auto modeIs = [&] (const char* m) {
        auto want = std::find (msid::filterModeNames().begin(), msid::filterModeNames().end(), m) - msid::filterModeNames().begin();
        return std::lround (r.plain ("filter_mode")) == want;
    };
    REQUIRE (modeIs ("lp"));
    r.midi (juce::MidiMessage::noteOn (10, 40, (juce::uint8) 100));
    CHECK (waitFor ([&] { return (r.fake.at (0xD418) & 0x70) == 0x20 && (r.fake.at (0xD438) & 0x70) == 0x20; }));   // BP
    CHECK (waitFor ([&] { return modeIs ("bp"); }));
    r.midi (juce::MidiMessage::noteOff (10, 40));
    r.midi (juce::MidiMessage::noteOn (10, 40, (juce::uint8) 100));
    CHECK (waitFor ([&] { return (r.fake.at (0xD418) & 0x70) == 0x40 && modeIs ("hp"); }));
    pump (100);
    CHECK ((r.fake.at (0xD404) & 1) == 0);    // pads never gate a voice
}

TEST_CASE ("MIDI notes, CCs and bend play the SID; CCs write back to parameters", "[plugin]")
{
    Rig r;
    r.midi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
    CHECK (waitFor ([&] { return r.fake.at (0xD404) == 0x41; }));
    CHECK ((r.fake.at (0xD400) | r.fake.at (0xD401) << 8) == msid::noteToSid (60));
    r.midi (juce::MidiMessage::controllerEvent (1, 74, 0));
    CHECK (waitFor ([&] { return cutoffAt (r.fake) == 0; }));
    CHECK (waitFor ([&] { return r.plain ("cutoff") == 0.0f; }));      // write-back to the host
    pump (200);
    CHECK (cutoffAt (r.fake) == 0);                                    // ...and no echo fight
    r.midi (juce::MidiMessage::pitchWheel (1, 16383));
    CHECK (waitFor ([&] { return (r.fake.at (0xD400) | r.fake.at (0xD401) << 8) == msid::noteToSid (60, 2.0); }));
    r.midi (juce::MidiMessage::noteOff (1, 60));
    CHECK (waitFor ([&] { return r.fake.at (0xD404) == 0x40; }));
}

TEST_CASE ("a fast CC sweep is not dragged back by parameter write-backs", "[plugin]")
{
    Rig r;
    for (int v = 127; v >= 0; --v)
    {
        r.midi (juce::MidiMessage::controllerEvent (1, 74, v));
        pump (2);   // let write-backs land mid-sweep, like a real host would
    }
    CHECK (waitFor ([&] { return cutoffAt (r.fake) == 0 && r.plain ("cutoff") == 0.0f; }));
    pump (300);
    CHECK (cutoffAt (r.fake) == 0);
}

TEST_CASE ("host programs select presets and update parameters", "[plugin]")
{
    Rig r;
    REQUIRE (r.proc.getNumPrograms() >= 8);
    CHECK (r.proc.getProgramName (1) == "Saw Bass");
    r.proc.setCurrentProgram (1);
    CHECK (waitFor ([&] { return r.fake.at (0xD404) == 0x20 && cutoffAt (r.fake) == 600; }));
    CHECK (waitFor ([&] { return r.plain ("cutoff") == 600.0f && r.plain ("waveform") == 1.0f; }));
    CHECK (waitFor ([&] { return r.proc.bridge().status().presetName == "Saw Bass"; }));
    r.midi (juce::MidiMessage::programChange (1, 7));   // Organ via MIDI
    CHECK (waitFor ([&] { return r.fake.at (0xD404) == 0x50; }));
}

TEST_CASE ("split layout routes channels to SIDs", "[plugin]")
{
    Rig r ("split");
    r.midi (juce::MidiMessage::noteOn (2, 64, (juce::uint8) 100));
    CHECK (waitFor ([&] { return r.fake.at (0xD424) == 0x41; }));
    CHECK (r.fake.at (0xD404) == 0x40);
}

TEST_CASE ("a second bridge on the same C64U is locked out", "[plugin]")
{
    Rig r;
    SidBridgeProcessor second;
    BridgeSettings s;
    s.host = "127.0.0.1:" + juce::String (r.fake.port);
    s.transport = "http";
    second.bridge().setSettings (s);
    CHECK (waitFor ([&] { return second.bridge().status().state == BridgeStatus::State::lockedOut; }));
    CHECK_THAT (second.bridge().status().message.toStdString(), ContainsSubstring ("Another bridge"));
}

TEST_CASE ("state round trip keeps settings and parameters", "[plugin]")
{
    juce::MemoryBlock blob;
    {
        Rig r;
        r.setPlain ("resonance", 9.0f);
        auto s = r.proc.bridge().getSettings();
        s.notePort = "Some Port";
        r.proc.bridge().setSettings (s);
        r.proc.getStateInformation (blob);
    }
    SidBridgeProcessor p;
    p.setStateInformation (blob.getData(), (int) blob.getSize());
    auto s = p.bridge().getSettings();
    CHECK (s.host.startsWith ("127.0.0.1:"));
    CHECK (s.notePort == "Some Port");
    auto* res = p.apvts.getParameter ("resonance");
    CHECK (res->convertFrom0to1 (res->getValue()) == 9.0f);
}

TEST_CASE ("unreachable C64U shows retrying, not a hang", "[plugin]")
{
    SidBridgeProcessor p;
    BridgeSettings s;
    s.host = "127.0.0.1:1";   // refused
    p.bridge().setSettings (s);
    CHECK (waitFor ([&] { return p.bridge().status().state == BridgeStatus::State::retrying; }, 5000));
    juce::AudioBuffer<float> buf (2, 64);
    juce::MidiBuffer mb;
    mb.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    p.processBlock (buf, mb);   // must not block
}

// Not run by default: renders the editor to MSID_SNAPSHOT (PNG), connected to a fake C64U
// with a chord held, for docs.   sidbridge-tests "[.snapshot]"
TEST_CASE ("DMA is the default and falls back to HTTP when the service is off", "[plugin]")
{
    FakeC64U fake;   // HTTP only: its port doesn't speak the DMA protocol
    SidBridgeProcessor proc;
    BridgeSettings s;
    s.host = "127.0.0.1:" + juce::String (fake.port);
    CHECK (s.transport == "dma");
    proc.bridge().setSettings (s);
    REQUIRE (waitFor ([&] { return proc.bridge().status().state == BridgeStatus::State::connected; }, 5000));
    CHECK_THAT (proc.bridge().status().message.toStdString(), ContainsSubstring ("via HTTP (DMA service not reachable)"));
    CHECK (waitFor ([&] { return fake.at (0xD418) == 0x1F; }));
}

TEST_CASE ("editor lists MIDI inputs, including ones that appear after it opened", "[plugin]")
{
    SidBridgeProcessor proc;
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto* box = dynamic_cast<juce::ComboBox*> (findById (*ed, "notePort"));
    REQUIRE (box != nullptr);
    auto lists = [box] (const juce::String& name) {
        for (int i = 0; i < box->getNumItems(); ++i)
            if (box->getItemText (i) == name)
                return true;
        return false;
    };
    for (auto& d : juce::MidiInput::getAvailableDevices())   // everything present now is listed
        CHECK (lists (d.name));
    auto port = juce::MidiOutput::createNewDevice ("MSID Editor Port Test");   // a new MIDI source
    REQUIRE (port != nullptr);
    CHECK (waitFor ([&] { return lists ("MSID Editor Port Test"); }, 4000));
}

TEST_CASE ("C64 audio: the C64U's stream plays out of the plugin", "[plugin]")
{
    FakeC64U fake;
    SidBridgeProcessor proc;
    const double hostRate = 48000.0;
    const int block = 480;
    proc.prepareToPlay (hostRate, block);
    BridgeSettings s;
    s.host = "127.0.0.1:" + juce::String (fake.port);
    s.transport = "http";
    s.hearC64 = true;
    proc.bridge().setSettings (s);
    REQUIRE (waitFor ([&] { return proc.bridge().status().state == BridgeStatus::State::connected; }));

    int udpPort = 0;   // from the request the plugin sent to the "C64U"
    REQUIRE (waitFor ([&] {
        std::lock_guard g (fake.m);
        for (auto& r : fake.requests)
            if (auto q = r.find ("audio:start?ip=127.0.0.1:"); q != std::string::npos)
                udpPort = std::stoi (r.substr (q + 25));
        return udpPort > 0;
    }));

    // the "C64U": 1 kHz sine at 47983 Hz, 192 frames per packet, real-time paced
    std::atomic<bool> sending { true };
    std::thread sender ([&] {
        int sock = ::socket (AF_INET, SOCK_DGRAM, 0);
        sockaddr_in to {};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        to.sin_port = htons ((uint16_t) udpPort);
        uint16_t seq = 0;
        long long frame = 0;
        auto t0 = std::chrono::steady_clock::now();
        while (sending)
        {
            uint8_t pkt[2 + 192 * 4];
            pkt[0] = (uint8_t) (seq & 0xFF); pkt[1] = (uint8_t) (seq >> 8); ++seq;
            for (int i = 0; i < 192; ++i, ++frame)
            {
                auto v = (int16_t) (16000 * std::sin (2 * M_PI * 1000.0 * (double) frame / 47983.0));
                for (int c = 0; c < 2; ++c) { pkt[2 + i * 4 + c * 2] = (uint8_t) (v & 0xFF); pkt[3 + i * 4 + c * 2] = (uint8_t) ((uint16_t) v >> 8); }
            }
            ::sendto (sock, pkt, sizeof pkt, 0, (sockaddr*) &to, sizeof to);
            std::this_thread::sleep_until (t0 + std::chrono::microseconds ((long long) ((double) frame * 1e6 / 47983.0)));
        }
        ::close (sock);
    });

    // the "audio device": process blocks in real time, keep the last second
    std::vector<float> outL;
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < (int) (2.5 * hostRate / block); ++b)
    {
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer mb;
        proc.processBlock (buf, mb);
        if (b * block > 1.5 * hostRate)
            outL.insert (outL.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
        std::this_thread::sleep_until (t0 + std::chrono::microseconds ((long long) ((b + 1) * block * 1e6 / hostRate)));
        if (b % 20 == 0)
            pump (1);
    }
    sending = false;
    sender.join();

    double rms = 0;
    int crossings = 0;
    for (size_t i = 1; i < outL.size(); ++i)
    {
        rms += outL[i] * outL[i];
        if (outL[i - 1] < 0 && outL[i] >= 0)
            ++crossings;
    }
    rms = std::sqrt (rms / (double) outL.size());
    double freq = crossings / ((double) outL.size() / hostRate);
    INFO ("rms " << rms << " freq " << freq << " underruns " << proc.bridge().audioStream().stats().underruns);
    CHECK (rms > 0.3);                          // 16000/32768 sine: 0.345
    CHECK (std::abs (freq - 1000.0) < 10.0);    // resampled to the host rate at the right pitch
    CHECK (proc.bridge().audioStream().stats().lost == 0);
    CHECK (waitFor ([&] { return ! proc.bridge().status().audio.contains ("ms buffer"); }));   // stream stopped: no longer "receiving"

    // stream gone -> silence (no stale loop)
    for (int b = 0; b < 30; ++b)
    {
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer mb;
        proc.processBlock (buf, mb);
        if (b > 10)
            CHECK (buf.getMagnitude (0, block) == 0.0f);
    }

    s.hearC64 = false;   // switching it off tells the C64U to stop streaming
    proc.bridge().setSettings (s);
    CHECK (waitFor ([&] {
        std::lock_guard g (fake.m);
        for (auto& r : fake.requests)
            if (r.find ("audio:stop") != std::string::npos)
                return true;
        return false;
    }));
}

// Not run by default: the real C64U (MSID_HW_HOST) through the plugin with "C64 audio" on.
// Plays A4 / C5 and checks the plugin's own output: pitch, dropouts, buffer level.
TEST_CASE ("hardware: C64 audio through the plugin", "[.hardware]")
{
    auto hwHost = juce::SystemStats::getEnvironmentVariable ("MSID_HW_HOST", {});
    REQUIRE (hwHost.isNotEmpty());
    SidBridgeProcessor proc;
    const double hostRate = 48000.0;
    const int block = 512;
    proc.prepareToPlay (hostRate, block);
    BridgeSettings s;
    s.host = hwHost;
    s.hearC64 = true;
    proc.bridge().setSettings (s);
    REQUIRE (waitFor ([&] { return proc.bridge().status().state == BridgeStatus::State::connected; }, 8000));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<float> outL;
    std::vector<std::pair<int, int>> notes { { 69, 440 }, { 72, 523 } };   // MIDI note, Hz
    std::vector<double> measured;
    int b = 0;
    auto runFor = [&] (double seconds, bool keep) {
        int n = (int) (seconds * hostRate / block);
        for (int i = 0; i < n; ++i, ++b)
        {
            juce::AudioBuffer<float> buf (2, block);
            juce::MidiBuffer mb;
            proc.processBlock (buf, mb);
            if (keep)
                outL.insert (outL.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
            std::this_thread::sleep_until (t0 + std::chrono::microseconds ((long long) ((double) (b + 1) * block * 1e6 / hostRate)));
            if (i % 20 == 0)
                pump (1);
        }
    };
    runFor (1.5, false);   // stream starts, buffer primes
    for (auto [note, hz] : notes)
    {
        juce::MidiBuffer mb;
        mb.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
        juce::AudioBuffer<float> buf (2, block);
        proc.processBlock (buf, mb);
        ++b;
        runFor (0.5, false);
        outL.clear();
        runFor (1.0, true);
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        proc.processBlock (buf, off);
        ++b;
        // fundamental by autocorrelation over 300-700 Hz
        double best = 0; int bestLag = 0;
        for (int lag = (int) (hostRate / 700); lag <= (int) (hostRate / 300); ++lag)
        {
            double acc = 0;
            for (size_t i = 0; i + (size_t) lag < outL.size(); i += 2)
                acc += outL[i] * outL[i + (size_t) lag];
            if (acc > best) { best = acc; bestLag = lag; }
        }
        double rms = 0;
        for (auto v : outL) rms += v * v;
        rms = std::sqrt (rms / (double) outL.size());
        double f = bestLag ? hostRate / bestLag : 0;
        measured.push_back (f);
        std::printf ("note %d: plugin output %.1f Hz (expect ~%d), rms %.3f\n", note, f, hz, rms);
        CHECK (rms > 0.01);
        CHECK (std::abs (f - hz) / hz < 0.02);
        runFor (0.6, false);
    }
    runFor (6.0, false);   // a longer stretch to watch for dropouts
    auto st = proc.bridge().audioStream().stats();
    std::printf ("stream: %llu packets, %llu lost, %llu underruns, buffer %.1f ms, ratio %.5f; status: %s\n",
                 (unsigned long long) st.packets, (unsigned long long) st.lost, (unsigned long long) st.underruns,
                 st.bufferMs, st.ratio, proc.bridge().status().message.toRawUTF8());
    std::printf ("%s\n", proc.bridge().status().audio.toRawUTF8());
    CHECK (st.underruns <= 1);   // the first prime may count one
}

TEST_CASE ("editor snapshot", "[.snapshot]")
{
    Rig r ("poly", "oxygen-49-mkv");
    r.proc.setCurrentProgram (4);
    // the README shot: the arpeggiator running over a held chord
    r.setPlain ("voice_mode", 6.0f);
    r.setPlain ("arp_octaves", 2.0f);
    r.setPlain ("arp_gate", 100.0f);
    pump (100);
    for (int n : { 48, 52, 55 })
        r.midi (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100));
    REQUIRE (waitFor ([&] { return r.proc.bridge().status().presetName == "Resonant Sweep"; }));
    std::unique_ptr<juce::AudioProcessorEditor> ed (r.proc.createEditor());
    auto* editor = dynamic_cast<SidBridgeEditor*> (ed.get());
    REQUIRE (editor != nullptr);
    auto shoot = [&] (const juce::String& path) {
        pump (400);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        juce::File out (path);
        out.deleteFile();
        juce::FileOutputStream os (out);
        REQUIRE (juce::PNGImageFormat().writeImageToStream (img, os));
    };
    auto base = juce::SystemStats::getEnvironmentVariable ("MSID_SNAPSHOT", "/tmp/sidbridge.png");
    editor->showTab (SidBridgeEditor::Tab::play);
    shoot (base);
    r.setPlain ("voice_mode", 5.0f);   // chord
    shoot (base.replace (".png", "-chord.png"));
    r.setPlain ("voice_mode", 0.0f);
    editor->showTab (SidBridgeEditor::Tab::setup);
    shoot (base.replace (".png", "-setup.png"));
    editor->showAbout();
    shoot (base.replace (".png", "-about.png"));
    updates::UpdateChecker::fetchOverride = [] (juce::String&) {
        updates::Release rel;
        rel.version = "v0.2.0";
        rel.pageUrl = rel.downloadUrl = "https://github.com/dantaipan/sid-bridge/releases/tag/v0.2.0";
        rel.notes = "Faster knobs and a new pad action.\n\n## New\n\n- **MIDI learn** for pads.\n- Bigger envelope drawing.";
        return std::optional<updates::Release> (rel);
    };
    editor->checkForUpdates();
    REQUIRE (waitFor ([&] { return editor->isUpdateShowing(); }));
    shoot (base.replace (".png", "-update.png"));
    updates::UpdateChecker::fetchOverride = nullptr;
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    // never the user's real update preferences
    auto prefs = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("msid-test.updates");
    prefs.deleteFile();
    updates::UpdateChecker::prefsFileOverride = prefs;
    return Catch::Session().run (argc, argv);
}

// ---------------------------------------------------------------- profiles / presets file

TEST_CASE ("profiles: built-ins, the user's own files, overrides and invalid files", "[plugin]")
{
    UserProfiles up;
    auto ids = [] {
        juce::StringArray out;
        for (auto& e : Profiles::list())
            out.add (e.id + "=" + e.origin.fromLastOccurrenceOf ("/", false, false) + (e.error.isEmpty() ? "" : "!"));
        return out;
    };
    auto l = ids();
    CHECK (l[0] == "generic=built-in");
    CHECK (l.contains ("oxygen-49-mkv=built-in"));
    up.write ("mine.toml", "name = \"Mine\"\n");
    up.write ("oxygen-49-mkv.toml", "name = \"My Oxygen\"\n");
    up.write ("broken.toml", "name = \"x\"\n[[control]]\ncc = 999\nparam = \"cutoff\"\n");
    up.write ("notes.txt", "not a profile");
    l = ids();
    CHECK (l.contains ("mine=mine.toml"));
    CHECK (l.contains ("oxygen-49-mkv=oxygen-49-mkv.toml"));   // the user's copy replaces the built-in
    CHECK (l.contains ("broken=broken.toml!"));
    CHECK (l.size() == 4);
    juce::String err;
    CHECK (Profiles::load ("mine", err).name == "Mine");
    CHECK (err.isEmpty());
    CHECK (Profiles::load ("broken", err) == msid::genericProfile());
    CHECK (err.contains ("0-127"));
    CHECK (Profiles::load ("gone", err) == msid::genericProfile());
    CHECK (err.contains ("not found"));
    auto dir = Profiles::prepareUserDir();
    CHECK (dir.getChildFile ("README.txt").existsAsFile());
    CHECK (dir.getChildFile ("generic.toml.example").loadFileAsString().contains ("[[control]]"));
}

TEST_CASE ("profiles: a user profile maps the controller in the plugin", "[plugin]")
{
    UserProfiles up;
    up.write ("mine.toml", "name = \"Mine\"\n[[control]]\ncc = 20\nparam = \"cutoff\"\n"
                           "[[control]]\ncc = 21\naction = \"waveform_next\"\n");
    Rig r ("poly", "mine");
    CHECK (r.proc.bridge().status().profileName == "Mine");
    r.midi (juce::MidiMessage::controllerEvent (1, 20, 0));
    CHECK (waitFor ([&] { return cutoffAt (r.fake) == 0 && r.plain ("cutoff") == 0.0f; }));
    r.midi (juce::MidiMessage::controllerEvent (1, 74, 127));          // not in this profile
    pump (150);
    CHECK (cutoffAt (r.fake) == 0);
    r.midi (juce::MidiMessage::controllerEvent (1, 21, 127));          // pulse -> noise
    CHECK (waitFor ([&] { return r.fake.at (0xD404) == 0x80 && r.plain ("waveform") == 3.0f; }));
}

TEST_CASE ("profiles: a broken profile falls back to generic and says why", "[plugin]")
{
    UserProfiles up;
    up.write ("broken.toml", "name = \"x\"\n[[control]]\ncc = 999\nparam = \"cutoff\"\n");
    Rig r ("poly", "broken");
    CHECK (waitFor ([&] { return r.proc.bridge().status().notice.contains ("0-127"); }));
    CHECK (r.proc.bridge().status().notice.contains ("generic"));
    r.midi (juce::MidiMessage::controllerEvent (1, 74, 0));            // generic: CC74 = cutoff
    CHECK (waitFor ([&] { return cutoffAt (r.fake) == 0; }));
}

TEST_CASE ("profiles: the controls port comes from the profile when set to auto", "[plugin]")
{
    UserProfiles up;
    auto out = juce::MidiOutput::createNewDevice ("MSID Auto Controls");
    REQUIRE (out != nullptr);
    pump (300);
    up.write ("knobs.toml", "name = \"Knobs\"\n[ports]\ncontrols = \"MSID Auto Controls\"\n"
                            "[[control]]\nport = \"controls\"\ncc = 16\nmode = \"relative-signed\"\nsteps = 16\nparam = \"resonance\"\n");
    Rig r ("poly", "knobs", {}, {});   // controls port empty = auto (the profile's)
    for (int i = 0; i < 4; ++i)
        out->sendMessageNow (juce::MidiMessage::controllerEvent (1, 16, 1));   // +4/16 of the range
    CHECK (waitFor ([&] { return (r.fake.at (0xD417) >> 4) == 4; }));
}

TEST_CASE ("presets file: loads, shows as host programs, and a bad file keeps the built-ins", "[plugin]")
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("msid-presets-test.toml");
    file.replaceWithText ("[[preset]]\nname = \"Only Saw\"\nwaveform = \"saw\"\n\n[[preset]]\nname = \"Only Tri\"\nwaveform = \"tri\"\n");
    {
        Rig r ("poly", "generic", file.getFullPathName());
        CHECK (r.proc.bridge().presets().size() == 2);
        CHECK (r.proc.getNumPrograms() == 2);
        CHECK (r.proc.getProgramName (1) == "Only Tri");
        r.proc.setCurrentProgram (1);
        CHECK (waitFor ([&] { return r.fake.at (0xD404) == 0x10; }));
        CHECK (r.proc.bridge().status().notice.isEmpty());

        auto s = r.proc.bridge().getSettings();
        s.presetsFile = file.getFullPathName() + ".missing";
        r.proc.bridge().setSettings (s);
        CHECK (r.proc.bridge().presets().size() == SidBridgeProcessor::builtinPresets().size());
        CHECK (waitFor ([&] { return r.proc.bridge().status().notice.contains ("not found"); }));
    }
    file.replaceWithText ("[[preset]]\nname = \"Bad\"\nwaveform = \"square\"\n");
    Rig r ("poly", "generic", file.getFullPathName());
    CHECK (r.proc.bridge().presets().size() == SidBridgeProcessor::builtinPresets().size());
    CHECK (waitFor ([&] { return r.proc.bridge().status().notice.contains ("unknown waveform"); }));
    file.deleteFile();
}

TEST_CASE ("settings saved before profiles existed keep the old (Oxygen) behaviour", "[plugin]")
{
    juce::ValueTree old ("Bridge");
    old.setProperty ("host", "192.168.1.64", nullptr);
    old.setProperty ("encoderPort", "Oxygen 49 MKV Mackie/HUI", nullptr);
    CHECK (BridgeSettings::fromValueTree (old).profile == "oxygen-49-mkv");
    CHECK (BridgeSettings::fromValueTree (juce::ValueTree()).profile == "generic");   // new install
    auto now = BridgeSettings {}.toValueTree();
    CHECK (BridgeSettings::fromValueTree (now).profile == "generic");
}

TEST_CASE ("profile and presets file survive a state round trip; editor shows the profile", "[plugin]")
{
    juce::MemoryBlock blob;
    {
        Rig r ("poly", "oxygen-49-mkv");
        auto s = r.proc.bridge().getSettings();
        s.presetsFile = "/tmp/some-presets.toml";
        r.proc.bridge().setSettings (s);
        r.proc.getStateInformation (blob);
    }
    SidBridgeProcessor p;
    p.setStateInformation (blob.getData(), (int) blob.getSize());
    CHECK (p.bridge().getSettings().profile == "oxygen-49-mkv");
    CHECK (p.bridge().getSettings().presetsFile == "/tmp/some-presets.toml");
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    auto* box = dynamic_cast<juce::ComboBox*> (findById (*ed, "profile"));
    REQUIRE (box != nullptr);
    CHECK (box->getText() == "M-Audio Oxygen 49 MKV (DAW mode)");
}

// ---------------------------------------------------------------- the new window: tabs, Find, learn

TEST_CASE ("window opens on Setup until it has connected once, then on Play", "[plugin]")
{
    {
        SidBridgeProcessor fresh;
        std::unique_ptr<juce::AudioProcessorEditor> ed (fresh.createEditor());
        CHECK (dynamic_cast<SidBridgeEditor*> (ed.get())->currentTab() == SidBridgeEditor::Tab::setup);
    }
    Rig r;   // connects
    CHECK (waitFor ([&] { return r.proc.bridge().getSettings().setupDone; }));
    std::unique_ptr<juce::AudioProcessorEditor> ed (r.proc.createEditor());
    auto* editor = dynamic_cast<SidBridgeEditor*> (ed.get());
    CHECK (editor->currentTab() == SidBridgeEditor::Tab::play);
    // the status pill and "Start playing" switch tabs
    dynamic_cast<juce::Button*> (findById (*ed, "status"))->triggerClick();
    pump (50);
    CHECK (editor->currentTab() == SidBridgeEditor::Tab::setup);
    pump (200);
    auto* start = findById (*ed, "startPlaying");
    REQUIRE (start != nullptr);
    CHECK (waitFor ([&] { return start->isVisible(); }));
    dynamic_cast<juce::Button*> (start)->triggerClick();
    pump (50);
    CHECK (editor->currentTab() == SidBridgeEditor::Tab::play);
    // saved state from before setupDone existed counts as set up when it has an address
    juce::ValueTree old ("Bridge");
    old.setProperty ("host", "192.168.1.64", nullptr);
    CHECK (BridgeSettings::fromValueTree (old).setupDone);
    CHECK_FALSE (BridgeSettings::fromValueTree (juce::ValueTree()).setupDone);
}

TEST_CASE ("Find: first run searches by itself and uses the only C64U found", "[plugin]")
{
    FakeC64U fake;
    BridgeController::findOverride = [&fake] (const msid::FindOptions&) {
        msid::FoundDevice d;
        d.ip = "127.0.0.1:" + std::to_string (fake.port);   // host:port is accepted as an address
        d.hostname = "C64-Ultimate-FAKE";
        d.product = "C64 Ultimate";
        return std::vector<msid::FoundDevice> { d };
    };
    {
        SidBridgeProcessor proc;
        auto s = proc.bridge().getSettings();
        s.transport = "http";
        proc.bridge().setSettings (s);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());   // opens on Setup, searches
        CHECK (waitFor ([&] { return proc.bridge().getSettings().host == "127.0.0.1:" + juce::String (fake.port); }));
        CHECK (waitFor ([&] { return proc.bridge().status().state == BridgeStatus::State::connected; }));
        CHECK (findById (*ed, "found:127.0.0.1:" + juce::String (fake.port)) != nullptr);
    }
    BridgeController::findOverride = nullptr;
}

TEST_CASE ("MIDI learn: a knob, absolute and relative, saved to the user's copy", "[plugin]")
{
    UserProfiles up;
    Rig r ("poly", "oxygen-49-mkv");
    r.proc.bridge().startLearn ("param:cutoff");
    CHECK (waitFor ([&] { return r.proc.bridge().status().learnTarget == "param:cutoff"; }));
    for (int v : { 10, 11, 12, 14 })   // an ordinary knob: only changes
        r.midi (juce::MidiMessage::controllerEvent (1, 30, v));
    CHECK (waitFor ([&] { return r.proc.bridge().status().learnMessage.contains ("Cutoff is now on CC30"); }));
    CHECK (r.proc.bridge().getSettings().profile == "oxygen-49-mkv-mine");
    auto saved = up.dir.getChildFile ("oxygen-49-mkv-mine.toml");
    REQUIRE (saved.existsAsFile());
    auto p = msid::loadProfile (saved.getFullPathName().toStdString());
    CHECK (p.name == "M-Audio Oxygen 49 MKV (DAW mode) (mine)");
    int cutoffControls = 0;
    for (auto& c : p.controls)
        if (c.param == "cutoff")
        {
            ++cutoffControls;
            CHECK (c.number == 30);
            CHECK (c.mode == "absolute");
        }
    CHECK (cutoffControls == 1);           // CC74 and Mackie knob 1 no longer do cutoff
    pump (100);
    CHECK (cutoffAt (r.fake) == 2047);     // learning didn't move it
    r.midi (juce::MidiMessage::controllerEvent (1, 30, 0));
    CHECK (waitFor ([&] { return cutoffAt (r.fake) == 0; }));   // works at once, no reconnect

    r.proc.bridge().startLearn ("param:resonance");   // an endless knob: repeats its step value
    pump (100);
    for (int i = 0; i < 4; ++i)
        r.midi (juce::MidiMessage::controllerEvent (1, 31, 1));
    CHECK (waitFor ([&] { return r.proc.bridge().status().learnMessage.contains ("Resonance"); }));
    p = msid::loadProfile (saved.getFullPathName().toStdString());
    bool relative = false;
    for (auto& c : p.controls)
        relative |= c.param == "resonance" && c.number == 31 && c.mode == "relative-signed";
    CHECK (relative);

    r.proc.bridge().forget ("param:cutoff");
    CHECK (waitFor ([&] { return r.proc.bridge().status().learnMessage.contains ("no longer"); }));
    p = msid::loadProfile (saved.getFullPathName().toStdString());
    for (auto& c : p.controls)
        CHECK (c.param != "cutoff");
}

TEST_CASE ("MIDI learn: a pad action, and cancel", "[plugin]")
{
    UserProfiles up;
    Rig r;
    r.proc.bridge().startLearn ("action:preset_next");
    pump (100);
    r.midi (juce::MidiMessage::noteOn (10, 44, (juce::uint8) 100));
    r.midi (juce::MidiMessage::noteOff (10, 44));
    CHECK (waitFor ([&] { return r.proc.bridge().status().learnMessage.contains ("Next preset is now on Note 44 ch10"); }));
    CHECK (r.proc.bridge().getSettings().profile == "generic-mine");
    pump (100);
    CHECK ((r.fake.at (0xD404) & 1) == 0);   // the pad press didn't play a note
    r.midi (juce::MidiMessage::noteOn (10, 44, (juce::uint8) 100));
    CHECK (waitFor ([&] { return r.proc.bridge().status().presetName == "Square Lead"; }));
    r.proc.bridge().startLearn ("param:volume");
    CHECK (waitFor ([&] { return r.proc.bridge().status().learnTarget.isNotEmpty(); }));
    r.proc.bridge().cancelLearn();
    CHECK (waitFor ([&] { return r.proc.bridge().status().learnTarget.isEmpty(); }));
}

TEST_CASE ("header: Silence stops notes", "[plugin]")
{
    Rig r;
    std::unique_ptr<juce::AudioProcessorEditor> ed (r.proc.createEditor());
    r.midi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
    CHECK (waitFor ([&] { return (r.fake.at (0xD404) & 1) == 1; }));
    dynamic_cast<juce::Button*> (findById (*ed, "silence"))->triggerClick();
    CHECK (waitFor ([&] { return (r.fake.at (0xD404) & 1) == 0 && (r.fake.at (0xD418) & 0x0F) == 0; }));
}

TEST_CASE ("standalone: an empty notes port opens the profile's notes port by itself", "[plugin]")
{
    UserProfiles up;
    auto keys = juce::MidiOutput::createNewDevice ("MSID Standalone Keys");
    REQUIRE (keys != nullptr);
    pump (300);
    up.write ("keys.toml", "name = \"Keys\"\n[ports]\nnotes = \"MSID Standalone Keys\"\n");
    FakeC64U fake;
    SidBridgeProcessor proc;
    proc.bridge().setStandalone (true);
    BridgeSettings s;
    s.host = "127.0.0.1:" + juce::String (fake.port);
    s.transport = "http";
    s.profile = "keys";
    s.encoderPort = "-";
    proc.bridge().setSettings (s);
    REQUIRE (waitFor ([&] { return proc.bridge().status().state == BridgeStatus::State::connected; }));
    CHECK (waitFor ([&] { return proc.bridge().openNotesPort() == "MSID Standalone Keys"; }));
    keys->sendMessageNow (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
    CHECK (waitFor ([&] { return (fake.at (0xD404) & 1) == 1; }));
    // a plugin (not standalone) leaves an empty notes port to the host
    SidBridgeProcessor plugin;
    CHECK_FALSE (plugin.bridge().isStandalone());
}

TEST_CASE ("About: the title and the Setup footnote open it, Close shuts it", "[plugin]")
{
    SidBridgeProcessor proc;
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto* editor = dynamic_cast<SidBridgeEditor*> (ed.get());
    CHECK_FALSE (editor->isAboutShowing());
    dynamic_cast<juce::Button*> (findById (*ed, "title"))->triggerClick();
    pump (50);
    CHECK (editor->isAboutShowing());
    dynamic_cast<juce::Button*> (findById (*ed, "aboutClose"))->triggerClick();
    pump (50);
    CHECK_FALSE (editor->isAboutShowing());
    editor->showTab (SidBridgeEditor::Tab::setup);
    dynamic_cast<juce::Button*> (findById (*ed, "footnote"))->triggerClick();
    pump (50);
    CHECK (editor->isAboutShowing());
    CHECK (AboutPanel::version().isNotEmpty());
}

// ---------------------------------------------------------------- updates (as in RoughTrack)

namespace {
const char* const kLatestReleaseJson = R"({
  "html_url": "https://github.com/dantaipan/sid-bridge/releases/tag/v0.2.0",
  "tag_name": "v0.2.0",
  "draft": false,
  "prerelease": false,
  "assets": [
    { "name": "SID-Bridge-0.2.0.zip", "browser_download_url": "https://example.com/SID-Bridge-0.2.0.zip" },
    { "name": "SID-Bridge-0.2.0.dmg", "browser_download_url": "https://example.com/SID-Bridge-0.2.0.dmg" }
  ],
  "body": "Faster knobs.\n\n## Download\n\n**[SID-Bridge-0.2.0.dmg](https://example.com/x.dmg)**: open it.\n\n## New\n\n![Shot](https://example.com/a.png)\n\n- **MIDI learn** for [pads](https://example.com).\n- Snap to `bars`.\n\nSHA-256 of the disk image: `8e87`"
})";
}

TEST_CASE ("updates: versions compare by number, not as text", "[updates]")
{
    using updates::isNewerVersion;
    CHECK (isNewerVersion ("v0.10.0", "0.9.0"));
    CHECK (isNewerVersion ("0.9.1", "0.9.0"));
    CHECK (isNewerVersion ("v1.0", "0.9.9"));
    CHECK_FALSE (isNewerVersion ("v0.9.0", "0.9.0"));
    CHECK_FALSE (isNewerVersion ("0.9", "0.9.0"));
    CHECK_FALSE (isNewerVersion ("v0.8.1", "0.9.0"));
}

TEST_CASE ("updates: GitHub's answer gives the version, notes and download (disk image before zip)", "[updates]")
{
    auto release = updates::parseLatestRelease (juce::JSON::parse (kLatestReleaseJson));
    REQUIRE (release.has_value());
    CHECK (release->version == "v0.2.0");
    CHECK (release->downloadUrl == "https://example.com/SID-Bridge-0.2.0.dmg");
    auto json = juce::JSON::parse (kLatestReleaseJson);
    json.getDynamicObject()->setProperty ("assets", juce::Array<juce::var>());
    CHECK (updates::parseLatestRelease (json)->downloadUrl == release->pageUrl);
    CHECK_FALSE (updates::parseLatestRelease (juce::JSON::parse (R"({ "message": "Not Found" })")).has_value());
    CHECK_FALSE (updates::parseLatestRelease ({}).has_value());
    auto pre = juce::JSON::parse (kLatestReleaseJson);
    pre.getDynamicObject()->setProperty ("prerelease", true);
    CHECK_FALSE (updates::parseLatestRelease (pre).has_value());
}

TEST_CASE ("updates: skipped versions, daily checks, readable notes", "[updates]")
{
    updates::Release r;
    r.version = "v0.10.0";
    CHECK (updates::shouldOffer (r, "0.9.0", {}, false));
    CHECK_FALSE (updates::shouldOffer (r, "0.9.0", "v0.10.0", false));
    CHECK (updates::shouldOffer (r, "0.9.0", "v0.10.0", true));
    r.version = "v0.9.0";
    CHECK_FALSE (updates::shouldOffer (r, "0.9.0", {}, true));

    constexpr juce::int64 hour = 60 * 60 * 1000;
    const juce::int64 now = 1'800'000'000'000;
    CHECK (updates::isAutomaticCheckDue ({}, "0.1.0", now));
    CHECK_FALSE (updates::isAutomaticCheckDue (updates::lastCheckRecord ("0.1.0", now - hour), "0.1.0", now));
    CHECK (updates::isAutomaticCheckDue (updates::lastCheckRecord ("0.1.0", now - 24 * hour), "0.1.0", now));
    CHECK (updates::isAutomaticCheckDue (updates::lastCheckRecord ("0.2.0", now - hour), "0.1.0", now));
    CHECK (updates::isAutomaticCheckDue (updates::lastCheckRecord ("0.1.0", now + hour), "0.1.0", now));

    auto release = updates::parseLatestRelease (juce::JSON::parse (kLatestReleaseJson));
    auto text = updates::runsToText (updates::notesToRuns (release->notes));
    CHECK (text == juce::String::fromUTF8 ("Faster knobs.\n\nNew\n\xe2\x80\xa2 MIDI learn for pads.\n\xe2\x80\xa2 Snap to bars."));
}

TEST_CASE ("updates: a newer release shows over the window; Skip hides it from automatic checks", "[updates]")
{
    updates::UpdateChecker::fetchOverride = [] (juce::String&) {
        return updates::parseLatestRelease (juce::JSON::parse (kLatestReleaseJson));
    };
    SidBridgeProcessor proc;
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto* editor = dynamic_cast<SidBridgeEditor*> (ed.get());
    juce::SharedResourcePointer<updates::UpdateChecker> checker;
    checker->skip ({});
    editor->checkForUpdates();
    CHECK (waitFor ([&] { return editor->isUpdateShowing(); }));
    CHECK (editor->updateHeadline() == "SID Bridge 0.2.0 is available");
    dynamic_cast<juce::Button*> (findById (*ed, "updateSkip"))->triggerClick();
    pump (50);
    CHECK_FALSE (editor->isUpdateShowing());
    CHECK (checker->skippedVersion() == "v0.2.0");
    checker->check (false);   // automatic: the skipped version stays quiet
    pump (500);
    CHECK_FALSE (editor->isUpdateShowing());
    editor->checkForUpdates(); // asked for: shown anyway
    CHECK (waitFor ([&] { return editor->isUpdateShowing(); }));

    // nothing newer / no release yet: a manual check says so
    updates::UpdateChecker::fetchOverride = [] (juce::String& error) {
        error = "No SID Bridge release has been published yet.";
        return std::optional<updates::Release> {};
    };
    dynamic_cast<juce::Button*> (findById (*ed, "updateLater"))->triggerClick();
    pump (50);
    editor->checkForUpdates();
    CHECK (waitFor ([&] { return editor->isUpdateShowing() && editor->updateHeadline() == "Couldn't check for updates"; }));
    updates::UpdateChecker::fetchOverride = nullptr;
}

// ---------------------------------------------------------------- voice modes / arpeggiator

namespace {
int freqAt (FakeC64U& f, int voiceBase) { return f.at (voiceBase) | f.at (voiceBase + 1) << 8; }
int gatesOn (FakeC64U& f)
{
    int n = 0;
    for (int base : { 0xD400, 0xD420 })
        for (int v = 0; v < 3; ++v)
            n += f.at (base + 7 * v + 4) & 1;
    return n;
}
} // namespace

TEST_CASE ("voice modes: chord stacks three voices per note", "[plugin]")
{
    Rig r;
    r.setPlain ("voice_mode", 5.0f);   // chord
    pump (100);
    r.midi (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100));
    CHECK (waitFor ([&] { return gatesOn (r.fake) == 3; }));
    std::set<int> f;
    for (int v = 0; v < 3; ++v)
        f.insert (freqAt (r.fake, 0xD400 + 7 * v));
    CHECK (f == std::set<int> { msid::noteToSid (48), msid::noteToSid (55), msid::noteToSid (60) });
    r.midi (juce::MidiMessage::noteOff (1, 48));
    CHECK (waitFor ([&] { return gatesOn (r.fake) == 0; }));
}

TEST_CASE ("arpeggiator: steps in time at the tempo, and follows MIDI clock", "[plugin]")
{
    Rig r;
    r.setPlain ("voice_mode", 6.0f);   // arp
    r.setPlain ("arp_bpm", 240.0f);    // 1/16 at 240 BPM = 62.5 ms a step
    r.setPlain ("arp_gate", 100.0f);
    pump (100);
    r.midi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
    r.midi (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100));
    std::set<int> heard;
    for (int i = 0; i < 40; ++i)   // ~400 ms: several steps
    {
        pump (10);
        heard.insert (freqAt (r.fake, 0xD400));
    }
    CHECK (heard.count (msid::noteToSid (60)) == 1);
    CHECK (heard.count (msid::noteToSid (67)) == 1);
    CHECK (r.proc.bridge().tempoInUse() == 240.0);

    // host tempo wins over the parameter
    r.proc.bridge().setHostBpm (90.0);
    CHECK (waitFor ([&] { return r.proc.bridge().tempoInUse() == 90.0; }));
    r.proc.bridge().setHostBpm (0.0);

    // MIDI clock from the host: 6 pulses per 1/16 step
    r.midi (juce::MidiMessage::midiStart());
    int before = freqAt (r.fake, 0xD400);
    for (int i = 0; i < 6; ++i)
        r.midi (juce::MidiMessage::midiClock());
    pump (50);
    CHECK (freqAt (r.fake, 0xD400) != before);
    r.midi (juce::MidiMessage::noteOff (1, 60));
    r.midi (juce::MidiMessage::noteOff (1, 67));
    CHECK (waitFor ([&] { return gatesOn (r.fake) == 0; }));
}

TEST_CASE ("voice modes: a pad's next-voice-mode shows in the parameter", "[plugin]")
{
    UserProfiles up;
    up.write ("vm.toml", "name = \"VM\"\n[[control]]\ncc = 85\naction = \"voice_mode_next\"\n");
    Rig r ("poly", "vm");
    r.midi (juce::MidiMessage::controllerEvent (1, 85, 127));
    CHECK (waitFor ([&] { return r.plain ("voice_mode") == 1.0f; }));   // mono
    r.midi (juce::MidiMessage::controllerEvent (1, 85, 127));
    CHECK (waitFor ([&] { return r.plain ("voice_mode") == 2.0f; }));   // unison
}
