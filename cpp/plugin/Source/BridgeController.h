// BridgeController: everything between the plugin and the C64U.
//
// Threads
//   audio thread   -> pushHostMidi(): lock-free FIFO only (RT-safe)
//   MIDI threads   -> direct note / encoder ports (MidiInput callbacks) -> small locked queues
//   message thread -> setSettings(), commands (preset, panic), status() for the editor
//   bridge thread  -> owns the msid::Engine / Flusher / HttpTransport: connects, drains the
//                     queues into the engine, syncs plugin parameters <-> engine patch
//   flusher thread -> msid::Flusher's own sender (see core)
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "msid/Engine.h"
#include "msid/DmaSocketTransport.h"
#include "msid/Finder.h"
#include "msid/HttpTransport.h"

#include "AudioStreamReceiver.h"
#include "Profiles.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

struct BridgeSettings
{
    juce::String host, password;
    juce::String layout = "poly";        // poly | split
    juce::String mode = "poly";          // poly | mono
    juce::String bases = "D400,D420";    // SID base addresses (hex, comma separated)
    juce::String video = "PAL";          // PAL | NTSC
    juce::String transport = "dma";      // dma (Ultimate DMA Service, TCP 64; falls back to HTTP) | http
    juce::String notePort;               // direct MIDI input for notes ("" = host MIDI)
    juce::String encoderPort;            // direct MIDI input for knobs / buttons ("" = the profile's controls port, if present)
    juce::String profile = "generic";    // controller profile id (Profiles)
    juce::String presetsFile;            // presets .toml ("" = built-in presets)
    int knobsTarget = 0;                 // controls-port knobs and actions change: 0 = all parts, n = part n
    bool setupDone = false;              // connected successfully once (the window then opens on Play); not compared
    bool enabled = true;
    bool hearC64 = false;                // play the C64U's audio stream out of the plugin
    juce::ValueTree toValueTree() const;
    static BridgeSettings fromValueTree (const juce::ValueTree&);
    bool operator== (const BridgeSettings&) const;
};

struct VoiceView
{
    uint16_t base = 0;
    int voice = 0;
    int note = -1;
    bool gated = false;
};

struct BridgeStatus
{
    enum class State { idle, lockedOut, connected, retrying } state = State::idle;
    juce::String message;
    juce::String via;                    // "DMA" or "HTTP ..."
    juce::String audio;                  // C64 audio stream state ("" = off)
    juce::String notice;                 // a problem with the profile / presets file ("" = none)
    juce::String profileName;
    msid::Profile profile;               // the profile in use (for "what controls this" badges)
    juce::String learnTarget;            // MIDI learn waiting for a control ("" = not learning)
    juce::String learnMessage;           // last learn result, e.g. "Cutoff is now on CC20"
    double latencyMs = 0;
    double writesPerSec = 0;
    int errors = 0;
    juce::String presetName;
    juce::String layout;
    std::vector<VoiceView> voices;
};

/// Plugin parameters the bridge keeps in sync with the engine's patch.
struct BridgeParams
{
    std::array<juce::RangedAudioParameter*, msid::kNumParams> ints {};   // indexed by msid::Param
    juce::RangedAudioParameter* waveform = nullptr;                       // choice: msid::waveformNames()
    juce::RangedAudioParameter* filterMode = nullptr;                     // choice: msid::filterModeNames()
    juce::RangedAudioParameter* velocity = nullptr;                       // choice: velocityModes()
    juce::RangedAudioParameter* velocityAmount = nullptr;                 // int 0..100 (%)
};

/// Velocity response choices, in parameter order (msid::Engine::setVelocity names).
inline const juce::StringArray& velocityModes()
{
    static const juce::StringArray m { "off", "sustain", "volume" };
    return m;
}

class BridgeController : private juce::Thread, private juce::MidiInputCallback
{
    JUCE_DECLARE_WEAK_REFERENCEABLE (BridgeController)
public:
    BridgeController (BridgeParams params, std::vector<msid::Preset> presets);
    ~BridgeController() override;

    void setSettings (const BridgeSettings&);
    BridgeSettings getSettings() const;

    /// Audio thread. Returns false if the event FIFO is full (event dropped).
    bool pushHostMidi (const juce::MidiMessage&) noexcept;
    /// True when a direct note port is in use: host MIDI is then ignored (avoids doubled notes).
    bool usingDirectNotes() const noexcept { return directNotes.load(); }

    /// Standalone app: there is no host MIDI, so an empty notes port means "auto": the profile's
    /// notes port (or the first MIDI input that isn't the controls port).
    void setStandalone (bool s) { standaloneApp = s; }
    bool isStandalone() const noexcept { return standaloneApp.load(); }
    /// The notes port actually opened ("" = none / host MIDI).
    juce::String openNotesPort() const;

    void selectPreset (int index);
    void panic();
    BridgeStatus status() const;
    /// The presets in use (built-in, or the presets file). Any thread.
    std::vector<msid::Preset> presets() const;
    /// Changes whenever presets() does (the editor rebuilds its list).
    int presetsVersion() const noexcept { return presetsGen.load(); }

    static juce::File lockFileFor (const juce::String& host);

    // MIDI learn. target = "param:<name>" or "action:<name>". The next control moved is mapped
    // to it in the user's own copy of the profile (written to the profiles folder) and used at once.
    void startLearn (const juce::String& target);
    void cancelLearn();
    void forget (const juce::String& target);
    static juce::String targetName (const juce::String& target);   // "Cutoff", "Next preset", ...

    // Finding the C64U on the network (in the background).
    struct FindState
    {
        bool running = false, finished = false;
        int progress = 0;
        std::vector<msid::FoundDevice> found;
    };
    void startFind();
    FindState findState() const;
    /// Tests: replace the network search.
    static std::function<std::vector<msid::FoundDevice> (const msid::FindOptions&)> findOverride;

    /// The C64U audio stream (started on connect when hearC64 is set). render() is RT-safe.
    AudioStreamReceiver& audioStream() noexcept { return audio; }

    /// Message thread (processor timer): push engine-side changes (MIDI CCs, knobs, presets)
    /// back into the plugin parameters so the host / GUI show them.
    void applyWritebacks();

private:
    void run() override;
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

    bool connect();
    void disconnect();
    void drainEvents();
    void syncParams();
    void route (const msid::MidiEvent& e, msid::Port port);
    bool learnFeed (const msid::MidiEvent& e, msid::Port port);
    void learnTick (double now);
    void applyLearned (const msid::Control& c);
    void saveProfile (msid::Profile p, const juce::String& message);
    void currentVelocity (std::string& mode, double& amount) const;
    void discardHostMidi();
    void publishStatus();
    void setStatus (BridgeStatus::State, const juce::String&);
    static std::optional<msid::MidiEvent> toEvent (const juce::MidiMessage&);
    bool acquireLock (const juce::String& host, juce::String& holder);
    void releaseLock();

    BridgeParams params;
    void loadPresetsFile (const juce::String& path);
    std::vector<msid::Preset> builtinPresets;
    mutable std::mutex presetsMutex;
    std::vector<msid::Preset> presetList;
    juce::String presetsNotice;
    std::atomic<int> presetsGen { 0 };
    juce::String profileNotice;

    struct LearnState
    {
        std::string target;   // "param:cutoff" / "action:preset_next"
        bool isParam = true;
        double started = 0, capturedAt = 0;
        bool captured = false;
        msid::Control trigger;
        std::vector<int> values;
    };
    std::optional<LearnState> learn;   // bridge thread only

    mutable std::mutex findMutex;
    FindState finding;
    std::thread findThread;
    std::atomic<bool> findCancel { false };

    mutable std::mutex settingsMutex;
    BridgeSettings settings;
    std::atomic<bool> reconnectRequested { true };
    std::atomic<bool> directNotes { false };
    std::atomic<bool> standaloneApp { false };

    // host MIDI: lock-free single producer (audio) / single consumer (bridge thread)
    static constexpr int kFifoSize = 4096;
    juce::AbstractFifo fifo { kFifoSize };
    std::array<msid::MidiEvent, kFifoSize> fifoData {};

    // direct ports: JUCE opens / lists MIDI devices on the message thread only, so the bridge
    // thread asks (requestDirectPorts) and openDirectPorts() does it there.
    void requestDirectPorts (const juce::String& notes, const juce::String& controls);
    void openDirectPorts();
    std::unique_ptr<juce::MidiInput> noteInput, encoderInput;            // message thread only
    std::atomic<juce::MidiInput*> notePtr { nullptr }, encoderPtr { nullptr };   // for the MIDI callback
    juce::String wantNotes, wantControls;                                 // guarded by directMutex
    std::atomic<int> portRequests { 0 };
    std::mutex directMutex;
    std::vector<msid::MidiEvent> directNoteEvents, directEncoderEvents;

    // commands from the message thread
    std::mutex cmdMutex;
    std::vector<std::function<void (msid::Engine&)>> commands;

    // live objects (bridge thread only)
    std::unique_ptr<msid::Transport> transport;
    msid::HttpTransport* http = nullptr;   // set when transport is HTTP (latency stats)
    juce::String via;
    AudioStreamReceiver audio;
    juce::String audioError;
    std::unique_ptr<msid::Flusher> flusher;
    std::unique_ptr<msid::Engine> engine;
    // per slot: kNumParams ints, then waveform, then filter mode
    std::array<int, msid::kNumParams + 2> lastParam {};   // engine value last seen
    std::array<int, msid::kNumParams + 2> seenParam {};   // parameter value last seen
    int lockFd = -1;
    double lastRetry = 0;
    double rateT0 = 0;
    int rateW0 = 0;
    double rate = 0;

    std::array<juce::RangedAudioParameter*, msid::kNumParams + 2> writebackParams {};
    std::array<std::atomic<float>, msid::kNumParams + 2> writeback;   // plain value, -1 = none
    std::array<std::vector<int>, msid::kNumParams + 2> echoes;        // values posted, not yet seen back

    mutable std::mutex statusMutex;
    BridgeStatus st;
};
