#include "msid/Flusher.h"

#include <chrono>

namespace msid {

using clock_t_ = std::chrono::steady_clock;

Flusher::Flusher (Transport& t, std::vector<SidState> s, double intervalMs, bool strict)
    : transport (t), states (std::move (s)), interval (intervalMs / 1000.0), strictOrder (strict)
{
}

Flusher::~Flusher() { stop (false); }

void Flusher::wake()
{
    {
        std::lock_guard g (wakeMutex);
        wakeFlag = true;
    }
    wakeCv.notify_one();
}

void Flusher::gate (int sidIndex, int voice, bool on)
{
    {
        std::lock_guard g (lock);
        auto& st = states[(size_t) sidIndex];
        if (on && st.gateOffPending (voice))
            commitLocked();
        st.setGate (voice, on);
    }
    wake();
}

void Flusher::barrier()
{
    {
        std::lock_guard g (lock);
        commitLocked();
    }
    wake();
}

void Flusher::commitLocked()
{
    Batch batch;
    for (auto& st : states)
    {
        auto w = st.snapshot (strictOrder);
        batch.insert (batch.end(), w.begin(), w.end());
    }
    if (! batch.empty())
        queue.push_back (std::move (batch));
}

bool Flusher::pending()
{
    std::lock_guard g (lock);
    if (! queue.empty())
        return true;
    for (auto& st : states)
        if (st.dirty)
            return true;
    return false;
}

int Flusher::flush()
{
    std::lock_guard ioGuard (io);
    int n = 0;
    try
    {
        transport.beginFlush();
        std::vector<Batch> batches;
        {
            std::lock_guard g (lock);
            commitLocked();
            batches = std::exchange (queue, {});
        }
        for (auto& batch : batches)
        {
            for (auto& w : batch)
            {
                transport.write (w.address, w.data);
                ++n;
            }
            ++batchesSent;
        }
        if (n > 0)
            transport.endFlush();
    }
    catch (const std::exception& e)
    {
        writesSent += n;
        failed (e);
        throw;
    }
    writesSent += n;
    if (! connected && n > 0)
    {
        connected = true;
        if (onReconnect)
            onReconnect();
    }
    return n;
}

void Flusher::failed (const std::exception& e)
{
    ++errors;
    {
        std::lock_guard g (errMutex);
        lastErr = e.what();
    }
    connected = false;
    {
        std::lock_guard g (lock);
        for (auto& st : states)
            st.markAllDirty();
    }
    if (onError)
        onError (e.what());
}

void Flusher::idle()
{
    std::lock_guard ioGuard (io);
    try
    {
        transport.idle();
    }
    catch (const std::exception& e)
    {
        failed (e);
        throw;
    }
}

void Flusher::run()
{
    auto last = clock_t_::now() - std::chrono::hours (1);
    double backoff = backoffMin;
    while (! stopFlag)
    {
        {
            std::unique_lock lk (wakeMutex);
            wakeCv.wait_for (lk, std::chrono::milliseconds (100), [this] { return wakeFlag || stopFlag.load(); });
            wakeFlag = false;
        }
        if (stopFlag)
            continue;
        if (! pending())
        {
            try
            {
                idle();
            }
            catch (const std::exception&)   // recorded by failed(); the next flush resyncs
            {
                std::unique_lock lk (wakeMutex);
                wakeCv.wait_for (lk, std::chrono::duration<double> (backoff), [this] { return stopFlag.load(); });
                backoff = std::min (backoffMax, backoff * 2);
            }
            continue;
        }
        auto since = std::chrono::duration<double> (clock_t_::now() - last).count();
        if (since < interval)
            std::this_thread::sleep_for (std::chrono::duration<double> (interval - since));
        last = clock_t_::now();
        try
        {
            flush();
            backoff = backoffMin;
        }
        catch (const std::exception&)
        {
            std::unique_lock lk (wakeMutex);
            wakeCv.wait_for (lk, std::chrono::duration<double> (backoff), [this] { return stopFlag.load(); });
            backoff = std::min (backoffMax, backoff * 2);
        }
    }
}

void Flusher::start()
{
    if (! thread.joinable())
    {
        stopFlag = false;
        thread = std::thread ([this] { run(); });
    }
}

void Flusher::stop (bool finalFlush)
{
    stopFlag = true;
    wake();
    if (thread.joinable())
        thread.join();
    if (finalFlush)
    {
        try { flush(); } catch (...) {}
    }
}

} // namespace msid
