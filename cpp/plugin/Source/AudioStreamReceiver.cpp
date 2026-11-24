#include "AudioStreamReceiver.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
} // namespace

AudioStreamReceiver::AudioStreamReceiver() = default;

AudioStreamReceiver::~AudioStreamReceiver() { stop (true); }

std::string AudioStreamReceiver::localIpFor (const std::string& h)
{
    // the address the OS would use to reach the C64U (no packet is sent)
    int s = ::socket (AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons (80);
    std::string ip = "127.0.0.1";
    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo (h.c_str(), nullptr, &hints, &res) == 0 && res != nullptr)
    {
        sockaddr_in resolved {};
        std::memcpy (&resolved, res->ai_addr, sizeof resolved);
        a.sin_addr = resolved.sin_addr;
        freeaddrinfo (res);
        if (::connect (s, (sockaddr*) &a, sizeof a) == 0)
        {
            sockaddr_in me {};
            socklen_t len = sizeof me;
            if (getsockname (s, (sockaddr*) &me, &len) == 0)
            {
                char buf[INET_ADDRSTRLEN] {};
                inet_ntop (AF_INET, &me.sin_addr, buf, sizeof buf);
                ip = buf;
            }
        }
    }
    ::close (s);
    return ip;
}

bool AudioStreamReceiver::http (const std::string& method, const std::string& path, juce::String& error)
{
    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo (host.c_str(), std::to_string (httpPort).c_str(), &hints, &res) != 0 || res == nullptr)
    {
        error = "cannot resolve " + juce::String (host);
        return false;
    }
    int s = ::socket (AF_INET, SOCK_STREAM, 0);
    timeval tv { 2, 0 };
    setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt (s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int rc = ::connect (s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo (res);
    if (rc != 0)
    {
        error = "C64U not reachable";
        ::close (s);
        return false;
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
    if (! password.empty())
        req += "X-Password: " + password + "\r\n";
    req += "Content-Length: 0\r\nConnection: close\r\n\r\n";
    ::send (s, req.data(), req.size(), 0);
    std::string resp;
    char buf[1024];
    for (ssize_t n; (n = ::recv (s, buf, sizeof buf, 0)) > 0;)
        resp.append (buf, (size_t) n);
    ::close (s);
    if (resp.size() < 12)
    {
        error = "no reply from the C64U";
        return false;
    }
    int status = std::atoi (resp.substr (9, 3).c_str());
    if (status >= 400)
    {
        error = "C64U answered HTTP " + juce::String (status);
        return false;
    }
    return true;
}

bool AudioStreamReceiver::start (const std::string& h, int port, const std::string& pw, juce::String& error)
{
    stop (false);
    host = h;
    httpPort = port;
    password = pw;

    fd = ::socket (AF_INET, SOCK_DGRAM, 0);
    int big = 1 << 20;
    setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    timeval tv { 0, 200000 };
    setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl (INADDR_ANY);
    a.sin_port = htons (kPreferredPort);
    if (::bind (fd, (sockaddr*) &a, sizeof a) != 0)
    {
        a.sin_port = 0;   // preferred port busy (another instance / tool): any free port
        if (::bind (fd, (sockaddr*) &a, sizeof a) != 0)
        {
            error = "cannot open a UDP port";
            ::close (fd);
            fd = -1;
            return false;
        }
    }
    socklen_t len = sizeof a;
    getsockname (fd, (sockaddr*) &a, &len);
    boundPort = ntohs (a.sin_port);

    packets = 0;
    lost = 0;
    underruns = 0;
    lastPacketMs = 0;
    running = true;
    thread = std::thread ([this] { receiveLoop(); });

    auto path = "/v1/streams/audio:start?ip=" + localIpFor (host) + ":" + std::to_string (boundPort);
    if (! http ("PUT", path, error))
    {
        stop (false);
        return false;
    }
    active = true;
    return true;
}

void AudioStreamReceiver::stop (bool tellC64)
{
    bool wasActive = active.exchange (false);
    running = false;
    if (thread.joinable())
        thread.join();
    if (fd >= 0)
    {
        ::close (fd);
        fd = -1;
    }
    if (wasActive && tellC64)
    {
        juce::String ignored;
        http ("PUT", "/v1/streams/audio:stop", ignored);
    }
}

void AudioStreamReceiver::receiveLoop()
{
    uint8_t pkt[2048];
    bool haveSeq = false;
    uint16_t nextSeq = 0;
    while (running)
    {
        auto n = ::recv (fd, pkt, sizeof pkt, 0);
        if (n < 6 || (n - 2) % 4 != 0)
            continue;   // timeout or malformed
        uint16_t seq = (uint16_t) (pkt[0] | pkt[1] << 8);
        if (haveSeq && seq != nextSeq)
            lost += (uint16_t) (seq - nextSeq);
        haveSeq = true;
        nextSeq = (uint16_t) (seq + 1);
        ++packets;
        lastPacketMs = nowMs();

        size_t frames = (size_t) (n - 2) / 4;
        uint64_t w = writePos.load (std::memory_order_relaxed);
        if (w + frames - readPos.load (std::memory_order_acquire) > kRingFrames)
            continue;   // nobody is reading (audio stopped): drop
        for (size_t i = 0; i < frames; ++i)
        {
            auto* p = pkt + 2 + i * 4;
            size_t at = (size_t) ((w + i) % kRingFrames) * 2;
            ring[at] = (int16_t) (p[0] | p[1] << 8) / 32768.0f;
            ring[at + 1] = (int16_t) (p[2] | p[3] << 8) / 32768.0f;
        }
        writePos.store (w + frames, std::memory_order_release);
    }
}

void AudioStreamReceiver::prepare (double rate, int block)
{
    hostRate = rate > 0 ? rate : 48000.0;
    maxBlock = juce::jmax (32, block);
    auto need = (size_t) (maxBlock * (kNominalRate / hostRate) * 1.1) + 16;
    for (int c = 0; c < 2; ++c)
    {
        scratch[c].assign (need, 0.0f);
        out[c].assign ((size_t) maxBlock, 0.0f);
        interp[c].reset();
    }
    primed = false;
    integral = 0;
    smoothedFill = 0;
}

void AudioStreamReceiver::render (juce::AudioBuffer<float>& buffer, int start, int num) noexcept
{
    if (! active.load() || scratch[0].empty())
        return;
    const double srcPerMs = kNominalRate / 1000.0;
    const double target = targetMs * srcPerMs + maxBlock * kNominalRate / hostRate;

    for (int done = 0; done < num;)
    {
        int n = juce::jmin (num - done, maxBlock);
        auto avail = (double) available();
        if (! primed)
        {
            if (avail < target)
                return;   // still filling (silence)
            primed = true;
            smoothedFill = avail;
            integral = 0;
            for (auto& i : interp)
                i.reset();
        }
        if (avail > target * 4 + 2048)   // way behind (audio was paused): jump to the target level
        {
            readPos.store (writePos.load() - (uint64_t) target, std::memory_order_release);
            avail = target;
            smoothedFill = target;
        }
        // rate control: hold the buffer at `target` (PI on the smoothed fill level)
        double dt = n / hostRate;
        smoothedFill += (avail - smoothedFill) * juce::jmin (1.0, dt / 0.5);
        double err = (smoothedFill - target) / target;
        integral = juce::jlimit (-0.004, 0.004, integral + err * 0.002 * dt);
        double adj = juce::jlimit (-0.005, 0.005, err * 0.002 + integral);
        double ratio = kNominalRate / hostRate * (1.0 + adj);
        ratioNow = 1.0 + adj;
        levelMs = smoothedFill / srcPerMs;

        auto need = (size_t) std::ceil (n * ratio) + 4;
        if (avail < (double) need || need > scratch[0].size())
        {
            ++underruns;
            primed = false;   // silence until refilled
            return;
        }
        uint64_t r = readPos.load (std::memory_order_relaxed);
        for (size_t i = 0; i < need; ++i)
        {
            size_t at = (size_t) ((r + i) % kRingFrames) * 2;
            scratch[0][i] = ring[at];
            scratch[1][i] = ring[at + 1];
        }
        int used = interp[0].process (ratio, scratch[0].data(), out[0].data(), n);
        interp[1].process (ratio, scratch[1].data(), out[1].data(), n);
        readPos.store (r + (uint64_t) used, std::memory_order_release);

        if (buffer.getNumChannels() >= 2)
        {
            buffer.addFrom (0, start + done, out[0].data(), n);
            buffer.addFrom (1, start + done, out[1].data(), n);
        }
        else if (buffer.getNumChannels() == 1)
        {
            buffer.addFrom (0, start + done, out[0].data(), n, 0.5f);
            buffer.addFrom (0, start + done, out[1].data(), n, 0.5f);
        }
        done += n;
    }
}

AudioStreamReceiver::Stats AudioStreamReceiver::stats() const
{
    Stats s;
    s.receiving = active.load() && nowMs() - lastPacketMs.load() < 500.0;
    s.bufferMs = levelMs.load();
    s.packets = packets.load();
    s.lost = lost.load();
    s.underruns = underruns.load();
    s.ratio = ratioNow.load();
    s.port = boundPort;
    return s;
}
