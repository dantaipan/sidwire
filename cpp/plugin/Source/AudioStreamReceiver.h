// AudioStreamReceiver: plays the C64U's network audio stream out of the plugin.
//
// The C64U sends its audio (what the real SIDs play) as UDP packets once started with
// PUT /v1/streams/audio:start?ip=<us>:<port>: u16 LE sequence number + 192 stereo frames of
// s16 LE at ~47983 Hz (PAL; format from PS64's audiostream.h, rate measured in NOTES.md).
//
//   network thread -> lock-free ring buffer -> audio thread (render): resampled to the host
//   rate with a slowly adapting ratio that holds the buffer at its target level, so the two
//   clocks never drift into dropouts. Underrun -> silence until the buffer refills.
//
// The C64U has a single audio-stream destination: starting here takes the stream away from
// anything else (e.g. PS64) until stopped.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

class AudioStreamReceiver
{
public:
    static constexpr int kPreferredPort = 11003;   // PS64 uses 11001, listen.py 11002
    static constexpr double kNominalRate = 47983.0;

    AudioStreamReceiver();
    ~AudioStreamReceiver();

    /// Binds the UDP port, starts the receive thread and asks the C64U to stream to us.
    /// Blocking (HTTP). Returns false with a message on failure.
    bool start (const std::string& host, int httpPort, const std::string& password, juce::String& error);
    /// Stops receiving; tellC64 also sends audio:stop.
    void stop (bool tellC64 = true);
    bool isActive() const noexcept { return active.load(); }

    /// Before playback (not concurrently with render).
    void prepare (double hostRate, int maxBlockSize);
    /// Audio thread: adds the stream into the buffer (mono buses get L+R / 2). RT-safe.
    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept;

    struct Stats
    {
        bool receiving = false;   // a packet in the last half second
        double bufferMs = 0;      // smoothed buffer level
        uint64_t packets = 0, lost = 0, underruns = 0;
        double ratio = 0;         // current resampling ratio / nominal
        int port = 0;
    };
    Stats stats() const;

    double targetMs = 15.0;       // buffer level the rate control holds (plus one host block)

    static std::string localIpFor (const std::string& host);

private:
    void receiveLoop();
    bool http (const std::string& method, const std::string& path, juce::String& error);
    uint64_t available() const noexcept { return writePos.load (std::memory_order_acquire) - readPos.load (std::memory_order_relaxed); }

    // ring of interleaved stereo float frames; positions count frames and only grow
    static constexpr size_t kRingFrames = 1 << 15;   // ~680 ms
    std::vector<float> ring = std::vector<float> (kRingFrames * 2);
    std::atomic<uint64_t> writePos { 0 }, readPos { 0 };

    int fd = -1;
    int boundPort = 0;
    std::thread thread;
    std::atomic<bool> running { false }, active { false };
    std::string host, password;
    int httpPort = 80;

    std::atomic<uint64_t> packets { 0 }, lost { 0 }, underruns { 0 };
    std::atomic<double> lastPacketMs { 0 }, levelMs { 0 }, ratioNow { 1 };

    // audio thread state
    double hostRate = 48000.0;
    int maxBlock = 512;
    bool primed = false;
    double smoothedFill = 0, integral = 0;
    juce::LagrangeInterpolator interp[2];
    std::vector<float> scratch[2], out[2];
};
