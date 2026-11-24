#include "BridgeController.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using namespace msid;

// ---------------------------------------------------------------- settings

juce::ValueTree BridgeSettings::toValueTree() const
{
    juce::ValueTree t ("Bridge");
    t.setProperty ("host", host, nullptr);
    t.setProperty ("password", password, nullptr);
    t.setProperty ("layout", layout, nullptr);
    t.setProperty ("mode", mode, nullptr);
    t.setProperty ("bases", bases, nullptr);
    t.setProperty ("video", video, nullptr);
    t.setProperty ("transport", transport, nullptr);
    t.setProperty ("notePort", notePort, nullptr);
    t.setProperty ("encoderPort", encoderPort, nullptr);
    t.setProperty ("enabled", enabled, nullptr);
    t.setProperty ("hearC64", hearC64, nullptr);
    t.setProperty ("profile", profile, nullptr);
    t.setProperty ("presetsFile", presetsFile, nullptr);
    t.setProperty ("knobsTarget", knobsTarget, nullptr);
    t.setProperty ("setupDone", setupDone, nullptr);
    return t;
}

BridgeSettings BridgeSettings::fromValueTree (const juce::ValueTree& t)
{
    BridgeSettings s;
    if (! t.isValid())
        return s;
    s.host = t.getProperty ("host", s.host);
    s.password = t.getProperty ("password", s.password);
    s.layout = t.getProperty ("layout", s.layout);
    s.mode = t.getProperty ("mode", s.mode);
    s.bases = t.getProperty ("bases", s.bases);
    s.video = t.getProperty ("video", s.video);
    s.transport = t.getProperty ("transport", s.transport);
    s.notePort = t.getProperty ("notePort", s.notePort);
    s.encoderPort = t.getProperty ("encoderPort", s.encoderPort);
    s.enabled = t.getProperty ("enabled", s.enabled);
    s.hearC64 = t.getProperty ("hearC64", s.hearC64);
    // Saved before profiles existed: the plugin then behaved exactly like the Oxygen profile
    // (Mackie knobs CC16-23, pad note 40 on channel 10), so keep that for old projects.
    s.profile = t.getProperty ("profile", t.hasProperty ("host") ? juce::String ("oxygen-49-mkv") : s.profile);
    s.presetsFile = t.getProperty ("presetsFile", s.presetsFile);
    s.knobsTarget = t.getProperty ("knobsTarget", s.knobsTarget);
    // saved before setupDone existed: set up already if it has an address
    s.setupDone = t.getProperty ("setupDone", s.host.trim().isNotEmpty());
    return s;
}

bool BridgeSettings::operator== (const BridgeSettings& o) const
{
    return host == o.host && password == o.password && layout == o.layout && mode == o.mode
        && bases == o.bases && video == o.video && transport == o.transport && notePort == o.notePort
        && encoderPort == o.encoderPort && enabled == o.enabled && hearC64 == o.hearC64
        && profile == o.profile && presetsFile == o.presetsFile && knobsTarget == o.knobsTarget;
}

// ---------------------------------------------------------------- lifecycle

BridgeController::BridgeController (BridgeParams p, std::vector<Preset> presets)
    : juce::Thread ("SID bridge"), params (p), builtinPresets (std::move (presets))
{
    presetList = builtinPresets;
    lastParam.fill (-1);
    seenParam.fill (-1);
    for (size_t i = 0; i < (size_t) kNumParams; ++i)
        writebackParams[i] = params.ints[i];
    writebackParams[kNumParams] = params.waveform;
    writebackParams[kNumParams + 1] = params.filterMode;
    for (auto& w : writeback)
        w.store (-1.0f);
    startThread (juce::Thread::Priority::high);
}

std::function<std::vector<FoundDevice> (const FindOptions&)> BridgeController::findOverride;

BridgeController::~BridgeController()
{
    findCancel = true;
    if (findThread.joinable())
        findThread.join();
    stopThread (3000);
}

// ---------------------------------------------------------------- finding the C64U

void BridgeController::startFind()
{
    findCancel = true;
    if (findThread.joinable())
        findThread.join();
    findCancel = false;
    {
        std::lock_guard g (findMutex);
        finding = {};
        finding.running = true;
    }
    FindOptions o;
    auto s = getSettings();
    o.alsoNear = s.host.trim().upToFirstOccurrenceOf (":", false, false).toStdString();
    o.password = s.password.toStdString();
    findThread = std::thread ([this, o] {
        auto progress = [this] (int p) { std::lock_guard g (findMutex); finding.progress = p; };
        auto found = findOverride ? findOverride (o) : findUltimates (o, progress, &findCancel);
        std::lock_guard g (findMutex);
        finding.found = found;
        finding.progress = 100;
        finding.running = false;
        finding.finished = true;
    });
}

BridgeController::FindState BridgeController::findState() const
{
    std::lock_guard g (findMutex);
    return finding;
}

// ---------------------------------------------------------------- MIDI learn

juce::String BridgeController::targetName (const juce::String& target)
{
    static const std::map<juce::String, juce::String> names {
        { "param:pulse_width", "Pulse width" }, { "param:cutoff", "Cutoff" }, { "param:resonance", "Resonance" },
        { "param:attack", "Attack" }, { "param:decay", "Decay" }, { "param:sustain", "Sustain" },
        { "param:release", "Release" }, { "param:volume", "Volume" },
        { "action:filter_mode_next", "Next filter mode" }, { "action:preset_next", "Next preset" },
        { "action:preset_prev", "Previous preset" }, { "action:waveform_next", "Next waveform" },
        { "action:panic", "Silence" }, { "action:sustain_pedal", "Sustain pedal" } };
    auto it = names.find (target);
    return it != names.end() ? it->second : target.fromFirstOccurrenceOf (":", false, false);
}

void BridgeController::startLearn (const juce::String& target)
{
    std::lock_guard g (cmdMutex);
    commands.push_back ([this, t = target.toStdString()] (Engine&) {
        LearnState l;
        l.target = t;
        l.isParam = t.rfind ("param:", 0) == 0;
        l.started = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        learn = l;
        std::lock_guard sg (statusMutex);
        st.learnTarget = juce::String (t);
        st.learnMessage = {};
    });
}

void BridgeController::cancelLearn()
{
    std::lock_guard g (cmdMutex);
    commands.push_back ([this] (Engine&) {
        learn.reset();
        std::lock_guard sg (statusMutex);
        st.learnTarget = {};
    });
}

void BridgeController::forget (const juce::String& target)
{
    std::lock_guard g (cmdMutex);
    commands.push_back ([this, t = target.toStdString()] (Engine& e) {
        auto p = e.config().profile;
        bool isParam = t.rfind ("param:", 0) == 0;
        auto name = t.substr (t.find (':') + 1);
        p.controls.erase (std::remove_if (p.controls.begin(), p.controls.end(), [&] (const Control& c) {
            return isParam ? c.param == name : c.action == name;
        }), p.controls.end());
        saveProfile (p, targetName (juce::String (t)) + " is no longer controlled by MIDI");
    });
}

bool BridgeController::learnFeed (const MidiEvent& e, Port port)
{
    if (! learn)
        return false;
    using T = MidiEvent::Type;
    auto& l = *learn;
    double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (l.isParam)
    {
        if (e.type != T::ControlChange && e.type != T::PitchBend)
            return false;   // notes keep playing while learning a knob
        Control c;
        c.kind = e.type == T::ControlChange ? Control::Kind::CC : Control::Kind::PitchBend;
        c.number = e.type == T::ControlChange ? e.data1 : 0;
        c.port = port;
        // a fader sent as pitch bend on a controls port is per channel (Mackie); a CC knob is not
        c.channel = (c.kind == Control::Kind::PitchBend && port == Port::Controls) ? e.channel + 1 : 0;
        if (! l.captured)
        {
            if (c.kind == Control::Kind::CC && (e.data1 == 120 || e.data1 == 123 || e.data1 == 64))
                return false;   // panic / pedal: not a knob
            l.captured = true;
            l.trigger = c;
            l.capturedAt = now;
        }
        if (c.kind == l.trigger.kind && c.number == l.trigger.number && c.port == l.trigger.port && c.channel == l.trigger.channel)
            l.values.push_back (c.kind == Control::Kind::CC ? e.data2 : 64);
        return true;   // nothing else moves while learning
    }
    // an action: a pad / key (note on) or a button (CC pressed)
    if (l.captured)
        return e.type == T::NoteOff || (e.type == T::NoteOn && e.data2 == 0);   // its release
    Control c;
    c.port = port;
    if (e.type == T::NoteOn && e.data2 > 0)
    {
        c.kind = Control::Kind::Note;
        c.number = e.data1;
        c.channel = e.channel + 1;   // pads usually have their own channel
    }
    else if (e.type == T::ControlChange && e.data2 >= 64 && e.data1 != 120 && e.data1 != 123)
    {
        c.kind = Control::Kind::CC;
        c.number = e.data1;
    }
    else
        return false;
    l.captured = true;
    l.trigger = c;
    applyLearned (c);
    return true;
}

void BridgeController::learnTick (double now)
{
    if (! learn)
        return;
    auto& l = *learn;
    if (l.isParam && l.captured && now - l.capturedAt > 0.4)
    {
        auto c = l.trigger;
        if (c.kind == Control::Kind::CC)
        {
            // An absolute knob only sends when its value changes; an endless (relative) one
            // repeats the same small step value while it turns. Turned clockwise: 1..15 is the
            // Mackie style (65.. = down), 65..79 is the "64 = no change" style.
            bool repeats = false, low = true, offset = true;
            for (size_t i = 0; i < l.values.size(); ++i)
            {
                int v = l.values[i];
                repeats |= i > 0 && v == l.values[i - 1];
                low &= v >= 1 && v <= 15;
                offset &= v >= 65 && v <= 79;
            }
            if (repeats && low) c.mode = "relative-signed";
            else if (repeats && offset) c.mode = "relative-offset";
        }
        applyLearned (c);
    }
    else if (now - l.started > 20.0)
    {
        learn.reset();
        std::lock_guard g (statusMutex);
        st.learnTarget = {};
        st.learnMessage = "MIDI learn stopped: nothing was received";
    }
}

void BridgeController::applyLearned (const Control& trigger)
{
    auto target = learn->target;
    learn.reset();
    bool isParam = target.rfind ("param:", 0) == 0;
    auto name = target.substr (target.find (':') + 1);
    auto c = trigger;
    if (isParam) c.param = name;
    else         c.action = name;
    auto p = engine->config().profile;
    // the control now does only this, and this has only the new control
    p.controls.erase (std::remove_if (p.controls.begin(), p.controls.end(), [&] (const Control& o) {
        bool sameTrigger = o.kind == c.kind && o.number == c.number && o.port == c.port && o.channel == c.channel;
        return sameTrigger || (isParam ? o.param == name : o.action == name);
    }), p.controls.end());
    p.controls.push_back (c);
    saveProfile (p, targetName (juce::String (target)) + " is now on " + juce::String (shortTrigger (c)));
}

void BridgeController::saveProfile (Profile p, const juce::String& message)
{
    // write to the user's own copy: a built-in profile gets copied to "<id>-mine"
    auto s = getSettings();
    juce::String id = s.profile;
    bool userFile = false;
    for (auto& e : Profiles::list())
        if (e.id == id)
            userFile = e.origin != "built-in";
    if (! userFile)
    {
        if (! id.endsWith ("-mine"))
            id += "-mine";
        if (! juce::String (p.name).endsWith ("(mine)"))
            p.name += " (mine)";
    }
    auto dir = Profiles::userDir();
    dir.createDirectory();
    auto file = dir.getChildFile (id + ".toml");
    bool ok = file.replaceWithText (juce::String (profileToToml (p, "Controller profile written by SID Bridge (MIDI learn).\n"
                                                                   "Format: see generic.toml.example in this folder.")));
    engine->setProfile (p);
    {
        std::lock_guard g (settingsMutex);
        settings.profile = id;   // no reconnect: the engine already has it
    }
    std::lock_guard g (statusMutex);
    st.learnTarget = {};
    st.learnMessage = ok ? message : "Could not save " + file.getFullPathName();
    st.profile = p;
    st.profileName = juce::String (p.name);
}

void BridgeController::setSettings (const BridgeSettings& s)
{
    bool presetsChanged;
    {
        std::lock_guard g (settingsMutex);
        if (s == settings)
            return;
        presetsChanged = s.presetsFile != settings.presetsFile;
        settings = s;
    }
    if (presetsChanged)
        loadPresetsFile (s.presetsFile);
    reconnectRequested = true;
    notify();
}

void BridgeController::loadPresetsFile (const juce::String& path)
{
    std::vector<Preset> loaded = builtinPresets;
    juce::String notice;
    if (path.isNotEmpty())
    {
        try
        {
            auto file = loadPresets (path.toStdString());
            if (file.empty())
                notice = "Presets file not found or empty: " + path + " (using the built-in presets)";
            else
                loaded = std::move (file);
        }
        catch (const std::exception& e)
        {
            notice = juce::String (e.what()) + " (using the built-in presets)";
        }
    }
    {
        std::lock_guard g (presetsMutex);
        presetList = std::move (loaded);
        presetsNotice = notice;
    }
    ++presetsGen;
}

std::vector<Preset> BridgeController::presets() const
{
    std::lock_guard g (presetsMutex);
    return presetList;
}

BridgeSettings BridgeController::getSettings() const
{
    std::lock_guard g (settingsMutex);
    return settings;
}

void BridgeController::selectPreset (int index)
{
    std::lock_guard g (cmdMutex);
    commands.push_back ([index] (Engine& e) {
        for (auto& part : e.parts)
            e.selectPreset (part, index);
    });
}

void BridgeController::panic()
{
    std::lock_guard g (cmdMutex);
    commands.push_back ([] (Engine& e) { e.panic(); });
}

BridgeStatus BridgeController::status() const
{
    std::lock_guard g (statusMutex);
    return st;
}

void BridgeController::setStatus (BridgeStatus::State s, const juce::String& msg)
{
    std::lock_guard g (statusMutex);
    st.state = s;
    st.message = msg;
    if (s != BridgeStatus::State::connected && s != BridgeStatus::State::retrying)
        st.voices.clear();
}

// ---------------------------------------------------------------- MIDI in

std::optional<MidiEvent> BridgeController::toEvent (const juce::MidiMessage& m)
{
    int ch = juce::jmax (0, m.getChannel() - 1);
    if (m.isNoteOn (true))   return MidiEvent::noteOn (ch, m.getNoteNumber(), m.getVelocity());
    if (m.isNoteOff (false)) return MidiEvent::noteOff (ch, m.getNoteNumber());
    if (m.isController())    return MidiEvent::cc (ch, m.getControllerNumber(), m.getControllerValue());
    if (m.isPitchWheel())    return MidiEvent::bend (ch, m.getPitchWheelValue() - 8192);
    if (m.isProgramChange()) return MidiEvent::program (ch, m.getProgramChangeNumber());
    return std::nullopt;
}

bool BridgeController::pushHostMidi (const juce::MidiMessage& m) noexcept
{
    auto e = toEvent (m);
    if (! e)
        return true;
    auto w = fifo.write (1);
    if (w.blockSize1 + w.blockSize2 == 0)
        return false;
    fifoData[(size_t) (w.blockSize1 > 0 ? w.startIndex1 : w.startIndex2)] = *e;
    return true;
}

void BridgeController::handleIncomingMidiMessage (juce::MidiInput* src, const juce::MidiMessage& m)
{
    auto e = toEvent (m);
    if (! e)
        return;
    std::lock_guard g (directMutex);
    if (src == notePtr.load())
        directNoteEvents.push_back (*e);
    else if (src == encoderPtr.load())
        directEncoderEvents.push_back (*e);
}

// ---------------------------------------------------------------- lock (shared with bridge.py)

juce::File BridgeController::lockFileFor (const juce::String& host)
{
    return juce::File ("/tmp").getChildFile ("midi-sid-bridge-" + host.replaceCharacters (":/", "__") + ".lock");
}

bool BridgeController::acquireLock (const juce::String& host, juce::String& holder)
{
    releaseLock();
    auto path = lockFileFor (host).getFullPathName();
    int fd = ::open (path.toRawUTF8(), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return true;   // can't lock (odd /tmp): don't block playing
    if (::flock (fd, LOCK_EX | LOCK_NB) != 0)
    {
        char buf[32] = {};
        auto n = ::pread (fd, buf, sizeof buf - 1, 0);
        holder = n > 0 ? juce::String (buf).trim() : "?";
        ::close (fd);
        return false;
    }
    auto pid = juce::String ((int) ::getpid());
    ::ftruncate (fd, 0);
    ::pwrite (fd, pid.toRawUTF8(), (size_t) pid.getNumBytesAsUTF8(), 0);
    lockFd = fd;
    return true;
}

void BridgeController::releaseLock()
{
    if (lockFd >= 0)
    {
        ::flock (lockFd, LOCK_UN);
        ::close (lockFd);
        lockFd = -1;
    }
}

// ---------------------------------------------------------------- connect / disconnect

static juce::String findInputIdentifier (const juce::String& name)
{
    for (auto& d : juce::MidiInput::getAvailableDevices())
        if (d.name == name)
            return d.identifier;
    for (auto& d : juce::MidiInput::getAvailableDevices())
        if (d.name.containsIgnoreCase (name))
            return d.identifier;
    return {};
}

bool BridgeController::connect()
{
    auto s = getSettings();
    if (! s.enabled)
    {
        setStatus (BridgeStatus::State::idle, "Disconnected");
        return false;
    }
    if (s.host.trim().isEmpty())
    {
        setStatus (BridgeStatus::State::idle, "Enter the C64U's IP address");
        return false;
    }
    juce::String holder;
    if (! acquireLock (s.host.trim(), holder))
    {
        setStatus (BridgeStatus::State::lockedOut,
                   "Another bridge (pid " + holder + ") is driving " + s.host.trim() + " - quit it first");
        return false;
    }

    std::vector<SidState> states;
    int clock = s.video.equalsIgnoreCase ("NTSC") ? kNtscClock : kPalClock;
    for (auto& b : juce::StringArray::fromTokens (s.bases, ", ;", ""))
        if (b.trim().isNotEmpty())
            states.emplace_back ((uint16_t) b.trim().removeCharacters ("$x").getHexValue32(), clock);
    if (states.empty())
        states.emplace_back (0xD400, clock);

    HttpTransport::Options o;
    o.password = s.password.toStdString();
    auto hostOnly = s.host.trim();
    if (hostOnly.containsChar (':'))   // host:port (tests, port forwarding)
    {
        o.port = hostOnly.fromLastOccurrenceOf (":", false, false).getIntValue();
        hostOnly = hostOnly.upToLastOccurrenceOf (":", false, false);
    }
    bool dma = s.transport == "dma";
    via = {};
    http = nullptr;
    if (dma)
    {
        int dmaPort = s.host.trim().containsChar (':') ? o.port : 64;
        try
        {
            {   // probe on its own connection with 2.5 s: the service serves one connection at a
                // time and drops an idle one only after 1 s
                DmaSocketTransport probe (hostOnly.toStdString(), dmaPort, o.password, 2.5);
                probe.identify();
            }
            transport = std::make_unique<DmaSocketTransport> (hostOnly.toStdString(), dmaPort, o.password);
            via = "DMA";
        }
        catch (const std::exception&)   // service switched off (or C64U away): use the REST API
        {
            dma = false;
            via = "HTTP (DMA service not reachable)";
        }
    }
    if (! transport)
    {
        auto h = std::make_unique<HttpTransport> (hostOnly.toStdString(), o);
        http = h.get();
        transport = std::move (h);
        if (via.isEmpty())
            via = "HTTP";
    }
    flusher = std::make_unique<Flusher> (*transport, std::move (states));

    EngineConfig cfg;
    juce::String profileError;
    cfg.profile = Profiles::load (s.profile, profileError);
    profileNotice = profileError.isEmpty() ? juce::String() : profileError + " (using the generic profile)";
    cfg.layout = s.layout.toStdString();
    cfg.knobsTarget = s.knobsTarget;
    cfg.mode = s.mode.toStdString();
    // start from the plugin's current parameter values
    for (int i = 0; i < kNumParams; ++i)
        if (auto* p = params.ints[(size_t) i])
            cfg.patch.get ((Param) i) = (int) std::lround (p->convertFrom0to1 (p->getValue()));
    if (params.waveform)
        cfg.patch.waveform = waveformNames()[(size_t) std::lround (params.waveform->convertFrom0to1 (params.waveform->getValue()))];
    if (params.filterMode)
        cfg.patch.filterMode = filterModeNames()[(size_t) std::lround (params.filterMode->convertFrom0to1 (params.filterMode->getValue()))];
    currentVelocity (cfg.velocity, cfg.velocityAmount);
    engine = std::make_unique<Engine> (*flusher, cfg, presets());
    for (int i = 0; i < kNumParams; ++i)
        lastParam[(size_t) i] = seenParam[(size_t) i] = cfg.patch.get ((Param) i);
    for (auto& e : echoes)
        e.clear();
    auto wi = (int) (std::find (waveformNames().begin(), waveformNames().end(), cfg.patch.waveform) - waveformNames().begin());
    auto fi = (int) (std::find (filterModeNames().begin(), filterModeNames().end(), cfg.patch.filterMode) - filterModeNames().begin());
    lastParam[(size_t) kNumParams] = seenParam[(size_t) kNumParams] = wi;
    lastParam[(size_t) kNumParams + 1] = seenParam[(size_t) kNumParams + 1] = fi;

    audioError = {};
    if (s.hearC64 && ! audio.start (hostOnly.toStdString(), o.port, o.password, audioError))
        audioError = "C64 audio not started: " + audioError;

    discardHostMidi();   // drop host MIDI that piled up while disconnected (before "connected" shows)
    engine->initChip();
    try
    {
        flusher->flush();
        setStatus (BridgeStatus::State::connected, "Connected to " + s.host.trim() + " via " + via);
    }
    catch (const std::exception& e)
    {
        setStatus (BridgeStatus::State::retrying, "Cannot reach " + s.host.trim() + ": " + e.what());
    }
    flusher->start();

    auto controlsPort = s.encoderPort.isNotEmpty() ? s.encoderPort : juce::String (cfg.profile.controlsPort);
    auto notesPort = s.notePort;
    if (notesPort.isEmpty() && standaloneApp)
        notesPort = cfg.profile.notesPort.empty() ? juce::String ("*") : juce::String (cfg.profile.notesPort);   // "*" = first input
    requestDirectPorts (notesPort, controlsPort != notesPort ? controlsPort : juce::String());
    return true;
}

void BridgeController::requestDirectPorts (const juce::String& notes, const juce::String& controls)
{
    {
        std::lock_guard g (directMutex);
        if (notes == wantNotes && controls == wantControls)
            return;
        wantNotes = notes;
        wantControls = controls;
    }
    ++portRequests;
    juce::MessageManager::callAsync ([weak = juce::WeakReference<BridgeController> (this)] {
        if (auto* self = weak.get())
            self->openDirectPorts();
    });
}

void BridgeController::openDirectPorts()
{
    JUCE_ASSERT_MESSAGE_THREAD
    juce::String notes, controls;
    {
        std::lock_guard g (directMutex);
        notes = wantNotes;
        controls = wantControls;
    }
    auto reopen = [this] (std::unique_ptr<juce::MidiInput>& input, std::atomic<juce::MidiInput*>& ptr, const juce::String& name) {
        if (input != nullptr && name.isNotEmpty() && input->getName() == name)
            return;   // already open
        ptr = nullptr;
        if (input != nullptr)
            input->stop();
        input.reset();
        if (name.isEmpty())
            return;
        if (auto id = findInputIdentifier (name); id.isNotEmpty())
            if ((input = juce::MidiInput::openDevice (id, this)))
            {
                ptr = input.get();
                input->start();
            }
    };
    if (notes == "*")   // standalone, no port named anywhere: the first input that isn't the controls port
    {
        notes = {};
        for (auto& d : juce::MidiInput::getAvailableDevices())
            if (controls.isEmpty() || ! d.name.containsIgnoreCase (controls))
            {
                notes = d.name;
                break;
            }
    }
    reopen (noteInput, notePtr, notes);
    reopen (encoderInput, encoderPtr, controls);
    directNotes = noteInput != nullptr;
}

juce::String BridgeController::openNotesPort() const
{
    auto* in = notePtr.load();
    return in != nullptr ? in->getName() : juce::String();
}

void BridgeController::disconnect()
{
    audio.stop (true);
    requestDirectPorts ({}, {});
    {
        std::lock_guard g (directMutex);
        directNoteEvents.clear();
        directEncoderEvents.clear();
    }
    if (engine)
        engine->panic();
    if (flusher)
        flusher->stop (true);
    engine.reset();
    flusher.reset();
    if (transport)
        transport->close();
    http = nullptr;
    transport.reset();
    releaseLock();
}

// ---------------------------------------------------------------- bridge thread

void BridgeController::discardHostMidi()
{
    auto r = fifo.read (fifo.getNumReady());   // consumer side only: safe with the audio thread
    juce::ignoreUnused (r);
}

void BridgeController::applyWritebacks()
{
    for (size_t i = 0; i < writebackParams.size(); ++i)
    {
        float v = writeback[i].exchange (-1.0f);
        if (v >= 0.0f && writebackParams[i] != nullptr)
            writebackParams[i]->setValueNotifyingHost (writebackParams[i]->convertTo0to1 (v));
    }
}

void BridgeController::route (const MidiEvent& e, Port port)
{
    if (! learnFeed (e, port))
        engine->handle (e, port);
}

void BridgeController::drainEvents()
{
    // host MIDI (ignored while a direct note port is open: avoids doubled notes)
    auto r = fifo.read (fifo.getNumReady());
    if (! directNotes)
    {
        for (int i = 0; i < r.blockSize1; ++i) route (fifoData[(size_t) (r.startIndex1 + i)], Port::Notes);
        for (int i = 0; i < r.blockSize2; ++i) route (fifoData[(size_t) (r.startIndex2 + i)], Port::Notes);
    }
    std::vector<MidiEvent> notes, enc;
    {
        std::lock_guard g (directMutex);
        notes.swap (directNoteEvents);
        enc.swap (directEncoderEvents);
    }
    for (auto& e : notes) route (e, Port::Notes);
    for (auto& e : enc) route (e, Port::Controls);

    std::vector<std::function<void (Engine&)>> cmds;
    {
        std::lock_guard g (cmdMutex);
        cmds.swap (commands);
    }
    for (auto& c : cmds) c (*engine);
}

void BridgeController::currentVelocity (std::string& mode, double& amount) const
{
    if (params.velocity)
    {
        auto i = (int) std::lround (params.velocity->convertFrom0to1 (params.velocity->getValue()));
        mode = velocityModes()[juce::jlimit (0, velocityModes().size() - 1, i)].toStdString();
    }
    if (params.velocityAmount)
        amount = (double) std::lround (params.velocityAmount->convertFrom0to1 (params.velocityAmount->getValue())) / 100.0;
}

void BridgeController::syncParams()
{
    // velocity response: host/GUI -> engine only (nothing on the MIDI side changes it)
    {
        std::string mode = engine->velocityMode();
        double amount = engine->velocityAmount();
        currentVelocity (mode, amount);
        if (mode != engine->velocityMode() || std::lround (amount * 100) != std::lround (engine->velocityAmount() * 100))
            engine->setVelocity (mode, amount);
    }

    // Two sources of truth meet here: the plugin parameters (host automation, GUI) and the
    // engine's patch (MIDI CCs, encoders, presets). Rules, per parameter slot:
    //   * the parameter moved since we last looked (v != seen) and it is not one of our own
    //     write-backs landing (echo)  -> a host/GUI change: push it into the engine;
    //   * the engine's value differs from what we last knew (ev != last) -> post a write-back.
    // Write-backs land on the message thread a little later; until then the parameter keeps
    // showing an older value, which must not count as a host change (it would drag the engine
    // back mid-sweep).
    auto& part0 = engine->parts[0];
    auto post = [this] (size_t slot, int plain) {
        writeback[slot].store ((float) plain);
        auto& e = echoes[slot];
        e.push_back (plain);
        if (e.size() > 64)
            e.erase (e.begin());
    };
    auto isEcho = [this] (size_t slot, int v) {
        auto& e = echoes[slot];
        auto it = std::find (e.begin(), e.end(), v);
        if (it == e.end())
            return false;
        e.erase (e.begin(), it + 1);   // this and anything older has now landed
        return true;
    };

    for (int i = 0; i < kNumParams; ++i)
    {
        auto slot = (size_t) i;
        auto* p = params.ints[slot];
        if (p == nullptr)
            continue;
        int v = (int) std::lround (p->convertFrom0to1 (p->getValue()));
        if (v != seenParam[slot])
        {
            seenParam[slot] = v;
            if (! isEcho (slot, v))
            {
                echoes[slot].clear();
                for (auto& part : engine->parts)
                    engine->setParam (part, (Param) i, v / (double) paramMax ((Param) i));
            }
        }
        int ev = part0.patch.get ((Param) i);
        if (ev != lastParam[slot])
        {
            lastParam[slot] = ev;
            if (ev != seenParam[slot])
                post (slot, ev);
        }
    }

    auto syncChoice = [&] (size_t slot, juce::RangedAudioParameter* p, const std::vector<std::string>& names,
                           const std::string& engineValue, bool wave) {
        if (p == nullptr)
            return;
        int v = (int) std::lround (p->convertFrom0to1 (p->getValue()));
        if (v != seenParam[slot])
        {
            seenParam[slot] = v;
            if (! isEcho (slot, v) && v >= 0 && v < (int) names.size())
            {
                echoes[slot].clear();
                PatchUpdate u;
                if (wave) u.waveform = names[(size_t) v];
                else      u.filterMode = names[(size_t) v];
                for (auto& part : engine->parts)
                    engine->applyPatch (part, u);
            }
        }
        int ev = (int) (std::find (names.begin(), names.end(), engineValue) - names.begin());
        if (ev < (int) names.size() && ev != lastParam[slot])
        {
            lastParam[slot] = ev;
            if (ev != seenParam[slot])
                post (slot, ev);
        }
    };
    syncChoice ((size_t) kNumParams, params.waveform, waveformNames(), part0.patch.waveform, true);
    syncChoice ((size_t) kNumParams + 1, params.filterMode, filterModeNames(), part0.patch.filterMode, false);
}

void BridgeController::publishStatus()
{
    double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (now - rateT0 >= 1.0)
    {
        rate = (flusher->writesSent - rateW0) / (now - rateT0);
        rateT0 = now;
        rateW0 = flusher->writesSent;
    }
    std::lock_guard g (statusMutex);
    auto host = getSettings().host.trim();
    if (flusher->connected)
    {
        {
            std::lock_guard sg (settingsMutex);
            settings.setupDone = true;   // the window opens on Play from now on
        }
        st.state = BridgeStatus::State::connected;
        st.message = "Connected to " + host + " via " + via;
    }
    else
    {
        st.state = BridgeStatus::State::retrying;
        st.message = "Retrying " + host + ": " + juce::String (flusher->lastError());
    }
    st.latencyMs = http ? http->medianLatencyMs() : 0.0;   // DMA writes have no reply to time
    st.writesPerSec = rate;
    if (audio.isActive())
    {
        auto a = audio.stats();
        st.audio = a.receiving ? "C64 audio " + juce::String (juce::roundToInt (a.bufferMs)) + " ms buffer, "
                                     + juce::String ((juce::int64) a.lost) + " lost"
                               : juce::String ("C64 audio: no packets yet (firewall?)");
    }
    else
        st.audio = audioError;
    st.via = via;
    st.errors = flusher->errors;
    st.presetName = engine->parts[0].patch.name;
    st.profileName = engine->config().profile.name;
    st.profile = engine->config().profile;
    {
        std::lock_guard pg (presetsMutex);
        st.notice = profileNotice;
        if (presetsNotice.isNotEmpty())
            st.notice << (st.notice.isEmpty() ? "" : "; ") << presetsNotice;
    }
    st.layout = engine->layout;
    st.voices.clear();
    for (auto& part : engine->parts)
        for (auto& s : part.slots)
            st.voices.push_back ({ flusher->states[(size_t) s.sid].base, s.voice, s.note.value_or (-1), s.gated });
}

void BridgeController::run()
{
    double lastPublish = 0, lastService = 0;
    while (! threadShouldExit())
    {
        double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        if (reconnectRequested.exchange (false))
        {
            disconnect();
            connect();
            lastRetry = now;
        }
        else if (! engine && now - lastRetry > 2.0)
        {
            // locked out / no host yet: try again (e.g. after bridge.py quits)
            lastRetry = now;
            connect();
        }
        if (engine)
        {
            drainEvents();
            learnTick (now);
            syncParams();
            if (now - lastService > 0.02)
            {
                engine->service();   // mute released voices (6581 leak hum)
                lastService = now;
            }
            if (now - lastPublish > 0.05)
            {
                publishStatus();
                lastPublish = now;
            }
        }
        else
        {
            discardHostMidi();   // nothing to play into
            std::lock_guard g (cmdMutex);
            commands.clear();
        }
        wait (1);
    }
    disconnect();
}
