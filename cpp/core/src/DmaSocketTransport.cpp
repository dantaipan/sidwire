#include "msid/DmaSocketTransport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace msid {

namespace {
struct SocketError : std::runtime_error   // retryable connection trouble (vs TransportError)
{
    using std::runtime_error::runtime_error;
};
SocketError sockErr (const char* what) { return SocketError (std::string ("DMA service ") + what + ": " + std::strerror (errno)); }
} // namespace

DmaSocketTransport::DmaSocketTransport (std::string h, int p, std::string pw, double t, int r, int mif, double ka)
    : host (std::move (h)), password (std::move (pw)), port (p), timeout (t), retries (r), maxInFlight (mif), keepalive (ka)
{
}

DmaSocketTransport::~DmaSocketTransport() { close(); }

std::vector<uint8_t> DmaSocketTransport::frame (uint16_t cmd, const std::vector<uint8_t>& payload)
{
    if (payload.size() > 0xFFFF)
        throw std::invalid_argument ("DMA payload too long");
    std::vector<uint8_t> f { (uint8_t) (cmd & 0xFF), (uint8_t) (cmd >> 8),
                             (uint8_t) (payload.size() & 0xFF), (uint8_t) (payload.size() >> 8) };
    f.insert (f.end(), payload.begin(), payload.end());
    return f;
}

void DmaSocketTransport::close()
{
    if (fd >= 0)
    {
        ::close (fd);
        fd = -1;
    }
}

void DmaSocketTransport::connectNow()
{
    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo (host.c_str(), std::to_string (port).c_str(), &hints, &res) != 0 || res == nullptr)
        throw TransportError ("DMA service: cannot resolve " + host);
    int s = ::socket (AF_INET, SOCK_STREAM, 0);
    timeval tv { (time_t) timeout, (suseconds_t) ((timeout - (double) (time_t) timeout) * 1e6) };
    setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt (s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt (s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int rc = ::connect (s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo (res);
    if (rc != 0)
    {
        auto e = sockErr ("connect");
        ::close (s);
        throw e;
    }
    fd = s;
    rx.clear();
    acks.clear();
    unacked = false;
    lastSend = Clock::now();
    if (! password.empty())
    {
        sendAll (frame (kCmdAuthenticate, std::vector<uint8_t> (password.begin(), password.end())));
        uint8_t ok = 0;
        if (::recv (fd, &ok, 1, 0) != 1 || ok != 1)
        {
            close();
            throw TransportError ("DMA service: authentication failed");
        }
    }
}

void DmaSocketTransport::sendAll (const std::vector<uint8_t>& b)
{
    size_t off = 0;
    while (off < b.size())
    {
        auto n = ::send (fd, b.data() + off, b.size() - off, 0);
        if (n <= 0)
            throw sockErr ("send");
        off += (size_t) n;
    }
}

void DmaSocketTransport::sendCmd (const std::vector<uint8_t>& f)
{
    sendAll (f);
    lastSend = Clock::now();
}

void DmaSocketTransport::pump (double wait)
{
    pollfd p { fd, POLLIN, 0 };
    int r = ::poll (&p, 1, (int) (wait * 1000.0));
    if (r < 0)
        throw sockErr ("poll");
    if (r == 0)
        return;
    uint8_t buf[512];
    auto n = ::recv (fd, buf, sizeof buf, 0);
    if (n < 0)
        throw sockErr ("recv");
    if (n == 0)
    {
        bool wasLossy = lossy();
        close();
        if (wasLossy)
            throw TransportError ("DMA service closed the connection");
        return;   // idle close; nothing was lost
    }
    rx.insert (rx.end(), buf, buf + n);
    while (! rx.empty() && rx.size() >= 1u + rx[0])
    {
        size_t len = rx[0];
        lastReply.assign (rx.begin() + 1, rx.begin() + 1 + (long) len);
        rx.erase (rx.begin(), rx.begin() + 1 + (long) len);
        if (! acks.empty())
            acks.pop_front();
    }
}

void DmaSocketTransport::ensure()
{
    if (fd >= 0)
    {
        if (! lossy() && since (lastSend) > stale)
            close();   // the service is about to drop it
        else
            pump();    // notices an idle close
    }
    if (fd < 0)
    {
        connectNow();
        ++reconnects;
    }
}

void DmaSocketTransport::waitAcks (size_t limit)
{
    auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration> (std::chrono::duration<double> (timeout));
    while (fd >= 0 && acks.size() > limit)
    {
        double left = std::chrono::duration<double> (deadline - Clock::now()).count();
        if (left <= 0)
        {
            close();
            throw TransportError ("DMA service: no reply for " + std::to_string (timeout).substr (0, 3) + " s");
        }
        pump (left);
    }
}

void DmaSocketTransport::write (uint16_t address, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> payload { (uint8_t) (address & 0xFF), (uint8_t) (address >> 8) };
    payload.insert (payload.end(), data.begin(), data.end());
    auto f = frame (kCmdDmaWrite, payload);
    for (int attempt = 0; attempt <= retries; ++attempt)
    {
        bool wasLossy = fd >= 0 && lossy();
        try
        {
            ensure();
            sendCmd (f);
            unacked = true;
            return;
        }
        catch (const SocketError& e)
        {
            ++errors;
            close();
            // retrying is only safe if nothing earlier can have been lost with the socket
            if (attempt == retries || wasLossy)
                throw TransportError (e.what());
        }
    }
}

void DmaSocketTransport::beginFlush()
{
    if (fd < 0)
        return;
    try
    {
        pump();
        waitAcks ((size_t) std::max (0, maxInFlight - 1));
    }
    catch (const SocketError& e)
    {
        close();
        throw TransportError (e.what());
    }
}

void DmaSocketTransport::endFlush()
{
    if (fd < 0 || ! unacked)
        return;
    try
    {
        sendCmd (frame (kCmdIdentify, {}));
    }
    catch (const SocketError& e)
    {
        close();
        throw TransportError (e.what());
    }
    acks.push_back (Clock::now());
    unacked = false;
}

void DmaSocketTransport::idle()
{
    if (fd < 0)
        return;
    try
    {
        pump();
        if (fd < 0)
            return;
        if (! acks.empty() && since (acks.front()) > timeout)
        {
            close();
            throw TransportError ("DMA service: no reply for " + std::to_string (timeout).substr (0, 3) + " s");
        }
        if (acks.empty() && since (lastSend) > keepalive)
        {
            sendCmd (frame (kCmdIdentify, {}));
            acks.push_back (Clock::now());
            ++keepalivesSent;
        }
    }
    catch (const SocketError& e)
    {
        close();
        throw TransportError (e.what());
    }
}

std::string DmaSocketTransport::identify()
{
    try
    {
        ensure();
        waitAcks (0);
        endFlush();
        waitAcks (0);
        sendCmd (frame (kCmdIdentify, {}));
        acks.push_back (Clock::now());
        waitAcks (0);
    }
    catch (const SocketError& e)
    {
        close();
        throw TransportError (e.what());
    }
    return lastReply;
}

} // namespace msid
