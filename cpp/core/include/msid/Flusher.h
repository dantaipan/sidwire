// Transport interface + Flusher (batches SID register writes from a background thread).
// C++ port of transport.py's Transport / DryRunTransport / Flusher.
#pragma once

#include "msid/Sid.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace msid {

struct TransportError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class Transport
{
public:
    virtual ~Transport() = default;
    virtual void write (uint16_t address, const std::vector<uint8_t>& data) = 0;
    // Flusher hooks; no-ops unless a transport needs pacing or keepalive (DmaSocketTransport).
    virtual void beginFlush() {}   // before each flush takes its snapshot; may block (backpressure)
    virtual void endFlush() {}     // after a flush has sent its writes
    virtual void idle() {}         // from the flush thread about every 100 ms while nothing is pending
    virtual void close() {}
};

/// Records writes (tests, --dry-run).
class DryRunTransport : public Transport
{
public:
    void write (uint16_t address, const std::vector<uint8_t>& data) override
    {
        std::lock_guard g (m);
        writes.push_back ({ address, data });
    }
    std::vector<Write> take()
    {
        std::lock_guard g (m);
        return std::exchange (writes, {});
    }
    std::vector<Write> writes;
    std::mutex m;
};

class Flusher
{
public:
    Flusher (Transport& t, std::vector<SidState> states, double intervalMs = 5.0, bool strictOrder = true);
    ~Flusher();

    Transport& transport;
    std::vector<SidState> states;

    /// RAII edit scope: holds the state lock, wakes the flush thread when it ends.
    class Edit
    {
    public:
        explicit Edit (Flusher& f) : fl (f), lk (f.lock) {}
        ~Edit() { lk.unlock(); fl.wake(); }
        std::vector<SidState>& states() { return fl.states; }
    private:
        Flusher& fl;
        std::unique_lock<std::recursive_mutex> lk;
    };
    Edit edit() { return Edit (*this); }

    void gate (int sidIndex, int voice, bool on);
    void barrier();
    int flush();   // throws on transport failure (after marking everything for resync)
    void idle();   // lets the transport keep its connection alive; throws like flush()

    void start();
    void stop (bool finalFlush = true);
    void wake();

    std::recursive_mutex lock;   // guards states + queue
    std::atomic<int> batchesSent { 0 }, writesSent { 0 }, errors { 0 };
    std::atomic<bool> connected { true };
    std::string lastError() const { std::lock_guard g (errMutex); return lastErr; }
    std::function<void (const std::string&)> onError;
    std::function<void()> onReconnect;
    double backoffMin = 0.25, backoffMax = 2.0;

private:
    void commitLocked();
    void failed (const std::exception&);
    bool pending();
    void run();

    double interval;
    bool strictOrder;
    std::mutex io;               // serialises sends; always taken before `lock`
    std::vector<Batch> queue;
    std::thread thread;
    std::mutex wakeMutex;
    std::condition_variable wakeCv;
    bool wakeFlag = false;
    std::atomic<bool> stopFlag { false };
    mutable std::mutex errMutex;
    std::string lastErr;
};

} // namespace msid
