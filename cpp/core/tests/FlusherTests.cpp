// Ports of the Flusher parts of tests/test_transport.py and test_phase5.py
#include "msid/Flusher.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace msid;
using namespace std::chrono_literals;

TEST_CASE ("knob sweep coalesces into one write", "[flusher]")
{
    DryRunTransport t;
    Flusher fl (t, { SidState() });
    {
        auto e = fl.edit();
        for (int fc = 0; fc < 2048; fc += 7)
            fl.states[0].setCutoff (fc);
    }
    CHECK (fl.flush() == 1);
    auto w = t.take();
    REQUIRE (w.size() == 1);
    CHECK (w[0] == Write { 0xD415, { 2044 & 7, 2044 >> 3 } });
    CHECK (fl.flush() == 0);
}

TEST_CASE ("retrigger inside one window is not lost", "[flusher]")
{
    DryRunTransport t;
    Flusher fl (t, { SidState() });
    {
        auto e = fl.edit();
        fl.states[0].setWaveform (0, TRI);
    }
    fl.gate (0, 0, true);
    fl.flush();
    t.take();
    fl.gate (0, 0, false);
    fl.gate (0, 0, true);
    fl.flush();
    auto w = t.take();
    REQUIRE (w.size() == 2);
    CHECK (w[0] == Write { 0xD404, { 0x10 } });
    CHECK (w[1] == Write { 0xD404, { 0x11 } });
}

TEST_CASE ("background thread sends", "[flusher]")
{
    DryRunTransport t;
    Flusher fl (t, { SidState() });
    fl.start();
    {
        auto e = fl.edit();
        fl.states[0].setVolume (9);
    }
    for (int i = 0; i < 200 && fl.writesSent == 0; ++i)
        std::this_thread::sleep_for (5ms);
    fl.stop();
    auto w = t.take();
    REQUIRE (w.size() == 1);
    CHECK (w[0] == Write { 0xD418, { 0x09 } });
}

namespace {
struct Flaky : Transport
{
    std::atomic<bool> down { false };
    DryRunTransport inner;
    void write (uint16_t a, const std::vector<uint8_t>& d) override
    {
        if (down)
            throw TransportError ("unreachable");
        inner.write (a, d);
    }
};
} // namespace

TEST_CASE ("reconnect resyncs every register", "[flusher]")
{
    Flaky t;
    Flusher fl (t, { SidState() });
    fl.backoffMin = fl.backoffMax = 0.02;
    std::atomic<bool> reconnected { false };
    fl.onReconnect = [&] { reconnected = true; };
    fl.start();
    t.down = true;
    {
        auto e = fl.edit();
        fl.states[0].setVolume (9);
    }
    for (int i = 0; i < 200 && fl.connected; ++i)
        std::this_thread::sleep_for (5ms);
    REQUIRE_FALSE (fl.connected);
    {
        auto e = fl.edit();
        fl.states[0].setCutoff (100);
    }
    t.down = false;
    for (int i = 0; i < 200 && ! reconnected; ++i)
        std::this_thread::sleep_for (5ms);
    fl.stop (false);
    REQUIRE (reconnected);
    bool full = false;
    for (auto& w : t.inner.take())
        if (w.address == 0xD400 && w.data.size() == 25)
        {
            full = true;
            CHECK (w.data[0x18] == 9);
            CHECK ((w.data[0x16] << 3 | w.data[0x15]) == 100);
        }
    CHECK (full);
}
