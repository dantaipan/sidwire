#include "msid/DmaSocketTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace msid;

namespace {
/// Speaks socket_dma.cc's framing; DMAWRITE stores into mem, IDENTIFY replies.
struct FakeDma
{
    int fd = -1, port = 0;
    std::thread th;
    std::mutex m;
    std::vector<uint8_t> mem = std::vector<uint8_t> (65536);
    std::atomic<int> connections { 0 };
    std::atomic<bool> frozen { false };   // link up, but nothing is processed
    double idleClose = 0, delay = 0;      // C64U: drops a connection after 1.0 s without a command

    explicit FakeDma (double idleCloseS = 0, double delayS = 0) : idleClose (idleCloseS), delay (delayS)
    {
        fd = ::socket (AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        ::bind (fd, (sockaddr*) &a, sizeof a);
        socklen_t len = sizeof a;
        getsockname (fd, (sockaddr*) &a, &len);
        port = ntohs (a.sin_port);
        ::listen (fd, 4);
        th = std::thread ([this] {
            for (;;)
            {
                int c = ::accept (fd, nullptr, nullptr);
                if (c < 0)
                    return;
                ++connections;
                if (idleClose > 0)
                {
                    timeval tv { 0, (suseconds_t) (idleClose * 1e6) };
                    setsockopt (c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                }
                auto readN = [c] (uint8_t* b, size_t n) {
                    size_t got = 0;
                    while (got < n) { auto r = ::recv (c, b + got, n - got, 0); if (r <= 0) return false; got += (size_t) r; }
                    return true;
                };
                uint8_t h[4];
                while (readN (h, 4))
                {
                    int cmd = h[0] | h[1] << 8, n = h[2] | h[3] << 8;
                    std::vector<uint8_t> p ((size_t) n);
                    if (n && ! readN (p.data(), (size_t) n)) break;
                    if (delay > 0) std::this_thread::sleep_for (std::chrono::duration<double> (delay));
                    if (frozen) continue;
                    if (cmd == 0xFF06) { std::lock_guard g (m); int a = p[0] | p[1] << 8; for (int i = 2; i < n; ++i) mem[(size_t) (a + i - 2)] = p[(size_t) i]; }
                    else if (cmd == 0xFF0E) { const char* t = "C64U-FAKE"; uint8_t l = 9; ::send (c, &l, 1, 0); ::send (c, t, 9, 0); }
                }
                ::close (c);
            }
        });
    }
    ~FakeDma() { ::shutdown (fd, SHUT_RDWR); ::close (fd); th.join(); }
};
} // namespace

TEST_CASE ("dma: frame layout", "[dma]")
{
    CHECK (DmaSocketTransport::frame (0xFF06, { 0x18, 0xD4, 0x0F })
           == std::vector<uint8_t> { 0x06, 0xFF, 0x03, 0x00, 0x18, 0xD4, 0x0F });
}

TEST_CASE ("dma: writes over one connection, in order", "[dma]")
{
    FakeDma f;
    DmaSocketTransport t ("127.0.0.1", f.port);
    for (uint8_t i = 0; i < 50; ++i)
        t.write (0xD400, { i, 0x11 });
    t.write (0xD418, { 0x0F });
    CHECK (t.identify() == "C64U-FAKE");   // reply comes after the writes were processed
    std::lock_guard g (f.m);
    CHECK (f.mem[0xD400] == 49);
    CHECK (f.mem[0xD418] == 0x0F);
    CHECK (f.connections == 1);
}

TEST_CASE ("dma: unreachable service fails cleanly", "[dma]")
{
    DmaSocketTransport t ("127.0.0.1", 1, {}, 0.2, 1);
    CHECK_THROWS_AS (t.write (0xD418, { 1 }), TransportError);
}

namespace {
uint8_t memAt (FakeDma& f, size_t a) { std::lock_guard g (f.m); return f.mem[a]; }
template <typename Pred> bool waitFor (Pred p, double s)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (s));
    while (std::chrono::steady_clock::now() < end)
    {
        if (p()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    return p();
}
} // namespace

TEST_CASE ("dma: keepalive holds the connection across the idle close", "[dma]")
{
    FakeDma f (0.3);
    DmaSocketTransport t ("127.0.0.1", f.port, {}, 1.0, 1, 1, 0.1);
    Flusher fl (t, { SidState() }, 1.0);
    fl.start();
    { auto e = fl.edit(); e.states()[0].setVolume (5); }
    std::this_thread::sleep_for (std::chrono::milliseconds (1000));
    { auto e = fl.edit(); e.states()[0].setVolume (9); }
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    fl.stop();
    CHECK ((memAt (f, 0xD418) & 0x0F) == 9);
    CHECK (f.connections == 1);
    CHECK (t.keepalivesSent >= 3);
    CHECK (fl.errors == 0);
}

TEST_CASE ("dma: an idle-closed connection is reopened without losing the write", "[dma]")
{
    FakeDma f (0.1);
    DmaSocketTransport t ("127.0.0.1", f.port);
    t.write (0xD418, { 3 });
    t.endFlush();
    t.beginFlush();
    std::this_thread::sleep_for (std::chrono::milliseconds (300));   // dropped before `stale`
    t.write (0xD418, { 4 });
    CHECK (t.identify() == "C64U-FAKE");
    CHECK (memAt (f, 0xD418) == 4);
    CHECK (f.connections == 2);
    t.stale = 0.2;
    std::this_thread::sleep_for (std::chrono::milliseconds (250));   // past `stale`: reopened first
    t.write (0xD418, { 6 });
    CHECK (t.identify() == "C64U-FAKE");
    CHECK (memAt (f, 0xD418) == 6);
    CHECK (f.connections == 3);
}

TEST_CASE ("dma: unacknowledged writes are never silently retried", "[dma]")
{
    FakeDma f (0.1);
    DmaSocketTransport t ("127.0.0.1", f.port);
    t.write (0xD418, { 9 });   // not acknowledged yet ...
    std::this_thread::sleep_for (std::chrono::milliseconds (300));   // ... and the service drops us
    CHECK_THROWS_AS (t.write (0xD418, { 10 }), TransportError);      // so the Flusher must resync
    t.write (0xD418, { 11 });  // the transport itself recovers on the next call
    CHECK (t.identify() == "C64U-FAKE");
    CHECK (memAt (f, 0xD418) == 11);
}

TEST_CASE ("dma: backpressure bounds the backlog and coalesces edits", "[dma]")
{
    FakeDma f (0, 0.01);   // 10 ms per command: slower than the edits
    DmaSocketTransport t ("127.0.0.1", f.port);
    Flusher fl (t, { SidState() }, 1.0);
    fl.start();
    for (int i = 0; i < 200; ++i)
    {
        { auto e = fl.edit(); e.states()[0].setCutoff (i * 10); }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    auto t0 = std::chrono::steady_clock::now();
    bool ok = waitFor ([&] { return ((memAt (f, 0xD415) & 7) | (memAt (f, 0xD416) << 3)) == 1990; }, 1.0);
    double settle = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    fl.stop();
    CHECK (ok);
    CHECK (settle < 0.06);
    CHECK (fl.batchesSent < 120);
}

TEST_CASE ("dma: a dead link is detected and everything resyncs after", "[dma]")
{
    FakeDma f;
    DmaSocketTransport t ("127.0.0.1", f.port, {}, 0.2);
    Flusher fl (t, { SidState() }, 1.0);
    fl.start();
    { auto e = fl.edit(); e.states()[0].setVolume (1); }
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    f.frozen = true;
    { auto e = fl.edit(); e.states()[0].setVolume (2); }
    std::this_thread::sleep_for (std::chrono::milliseconds (600));
    CHECK (fl.errors >= 1);
    CHECK (fl.lastError().find ("no reply") != std::string::npos);
    f.frozen = false;
    CHECK (waitFor ([&] { return (memAt (f, 0xD418) & 0x0F) == 2; }, 3.0));
    fl.stop (false);
}
