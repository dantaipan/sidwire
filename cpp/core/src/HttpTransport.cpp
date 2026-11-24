#include "msid/HttpTransport.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace msid {

namespace {

struct SocketError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

timeval toTimeval (double s)
{
    timeval tv;
    tv.tv_sec = (time_t) s;
    tv.tv_usec = (suseconds_t) ((s - (double) tv.tv_sec) * 1e6);
    return tv;
}

sockaddr_in resolve (const std::string& host, int port)
{
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons ((uint16_t) port);
    if (inet_pton (AF_INET, host.c_str(), &a.sin_addr) == 1)
        return a;
    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo (host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr)
        throw SocketError ("cannot resolve " + host);
    a.sin_addr = ((sockaddr_in*) res->ai_addr)->sin_addr;
    freeaddrinfo (res);
    return a;
}

int newSocket()
{
    int fd = ::socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        throw SocketError (std::string ("socket: ") + std::strerror (errno));
    int one = 1;
    setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

void setBlocking (int fd, bool blocking)
{
    int fl = fcntl (fd, F_GETFL, 0);
    fcntl (fd, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
}

void setTimeouts (int fd, double s)
{
    auto tv = toTimeval (s);
    setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

bool waitFd (int fd, bool forWrite, double seconds)
{
    fd_set set;
    FD_ZERO (&set);
    FD_SET (fd, &set);
    auto tv = toTimeval (std::max (0.0, seconds));
    int r = forWrite ? select (fd + 1, nullptr, &set, nullptr, &tv)
                     : select (fd + 1, &set, nullptr, nullptr, &tv);
    return r > 0;
}

void sendAll (int fd, const std::string& s)
{
    size_t off = 0;
    while (off < s.size())
    {
        auto n = ::send (fd, s.data() + off, s.size() - off, 0);
        if (n <= 0)
            throw SocketError (std::string ("send: ") + std::strerror (errno));
        off += (size_t) n;
    }
}

/// Returns (status, body). Throws SocketError on EOF / timeout.
std::pair<int, std::string> readResponse (int fd)
{
    std::string buf;
    char tmp[4096];
    size_t headEnd;
    while ((headEnd = buf.find ("\r\n\r\n")) == std::string::npos)
    {
        auto n = ::recv (fd, tmp, sizeof tmp, 0);
        if (n <= 0)
            throw SocketError (n == 0 ? "connection closed before response headers"
                                      : std::string ("recv: ") + std::strerror (errno));
        buf.append (tmp, (size_t) n);
    }
    std::string head = buf.substr (0, headEnd), body = buf.substr (headEnd + 4);
    int status = 0;
    if (std::sscanf (head.c_str(), "HTTP/%*s %d", &status) != 1)
        throw SocketError ("bad status line");
    long length = -1;
    std::string lower = head;
    std::transform (lower.begin(), lower.end(), lower.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    if (auto p = lower.find ("\r\ncontent-length:"); p != std::string::npos)
        length = std::strtol (lower.c_str() + p + 17, nullptr, 10);
    while (length < 0 || (long) body.size() < length)
    {
        auto n = ::recv (fd, tmp, sizeof tmp, 0);
        if (n <= 0)
            break;
        body.append (tmp, (size_t) n);
    }
    return { status, body };
}

/// True if the JSON body has a non-empty "errors" array.
std::string apiErrors (const std::string& body)
{
    auto p = body.find ("\"errors\"");
    if (p == std::string::npos)
        return {};
    auto lb = body.find ('[', p);
    if (lb == std::string::npos)
        return {};
    auto q = body.find_first_not_of (" \t\r\n", lb + 1);
    if (q == std::string::npos || body[q] == ']')
        return {};
    auto rb = body.find (']', lb);
    return body.substr (lb, rb == std::string::npos ? std::string::npos : rb - lb + 1);
}

} // namespace

HttpTransport::HttpTransport (std::string h, Options opts) : host (std::move (h)), o (std::move (opts))
{
    if (o.preconnect)
        startConnect();
}

HttpTransport::~HttpTransport() { close(); }

void HttpTransport::close() { dropSpare(); }

void HttpTransport::dropSpare()
{
    if (spareFd >= 0)
    {
        ::close (spareFd);
        spareFd = -1;
    }
}

void HttpTransport::startConnect()
{
    dropSpare();
    sockaddr_in addr;
    try { addr = resolve (host, o.port); } catch (...) { return; }
    int fd;
    try { fd = newSocket(); } catch (...) { return; }
    setBlocking (fd, false);
    int rc = ::connect (fd, (sockaddr*) &addr, sizeof addr);
    if (rc != 0 && errno != EINPROGRESS)
    {
        ::close (fd);
        return;
    }
    spareFd = fd;
    spareTime = std::chrono::steady_clock::now();
}

int HttpTransport::connectBlocking()
{
    auto addr = resolve (host, o.port);
    int fd = newSocket();
    setBlocking (fd, false);
    int rc = ::connect (fd, (sockaddr*) &addr, sizeof addr);
    if (rc != 0 && errno != EINPROGRESS)
    {
        ::close (fd);
        throw SocketError (std::string ("connect: ") + std::strerror (errno));
    }
    int err = 0;
    socklen_t len = sizeof err;
    if (! waitFd (fd, true, o.connectTimeout) || getsockopt (fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)
    {
        ::close (fd);
        throw SocketError (err ? std::string ("connect: ") + std::strerror (err) : "connect timed out");
    }
    setBlocking (fd, true);
    setTimeouts (fd, o.timeout);
    return fd;
}

int HttpTransport::takeSocket()
{
    if (spareFd >= 0)
    {
        int fd = std::exchange (spareFd, -1);
        double age = std::chrono::duration<double> (std::chrono::steady_clock::now() - spareTime).count();
        if (age <= o.maxIdle)
        {
            int err = 0;
            socklen_t len = sizeof err;
            if (waitFd (fd, true, std::max (0.0, o.connectTimeout - age))
                && getsockopt (fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0
                && ! waitFd (fd, false, 0.0))   // readable now would mean the server closed it
            {
                setBlocking (fd, true);
                setTimeouts (fd, o.timeout);
                ++spareHits;
                return fd;
            }
        }
        ::close (fd);
        ++spareMisses;
    }
    return connectBlocking();
}

void HttpTransport::request (uint16_t address, const uint8_t* data, size_t len)
{
    static const char* hex = "0123456789ABCDEF";
    std::string payload;
    payload.reserve (len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        payload += hex[data[i] >> 4];
        payload += hex[data[i] & 0xF];
    }
    char addr[8];
    std::snprintf (addr, sizeof addr, "%04X", address);
    std::string req = "PUT /v1/machine:writemem?address=" + std::string (addr) + "&data=" + payload
                    + " HTTP/1.1\r\nHost: " + host + "\r\n"
                    + (o.password.empty() ? "" : "X-Password: " + o.password + "\r\n")
                    + "Content-Length: 0\r\n\r\n";
    int fd = takeSocket();
    std::pair<int, std::string> resp;
    try
    {
        sendAll (fd, req);
        resp = readResponse (fd);
    }
    catch (...)
    {
        ::close (fd);
        if (o.preconnect)
            startConnect();
        throw;
    }
    ::close (fd);
    if (o.preconnect)
        startConnect();
    auto [status, body] = resp;
    if (status == 403)
        throw TransportError ("403 Forbidden: wrong or missing network password");
    if (status != 200)
        throw TransportError ("HTTP " + std::to_string (status) + ": " + body.substr (0, 200));
    if (auto e = apiErrors (body); ! e.empty())
        throw TransportError ("API errors: " + e);
}

void HttpTransport::write (uint16_t address, const std::vector<uint8_t>& data)
{
    for (size_t i = 0; i < data.size(); i += kMaxWrite)
        writeOne ((uint16_t) (address + i), data.data() + i, std::min (kMaxWrite, data.size() - i));
}

void HttpTransport::writeOne (uint16_t address, const uint8_t* data, size_t len)
{
    std::string last;
    for (int attempt = 0; attempt <= o.retries; ++attempt)
    {
        auto t0 = std::chrono::steady_clock::now();
        try
        {
            request (address, data, len);
            double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
            std::lock_guard g (latMutex);
            lat.push_back (ms);
            if (lat.size() > 2000)
                lat.pop_front();
            return;
        }
        catch (const TransportError&)
        {
            throw;
        }
        catch (const std::exception& e)   // socket errors: writes are idempotent, retry
        {
            last = e.what();
            ++errors;
            dropSpare();
        }
    }
    char addr[8];
    std::snprintf (addr, sizeof addr, "%04X", address);
    throw TransportError ("write $" + std::string (addr) + " failed after "
                          + std::to_string (o.retries + 1) + " attempts: " + last);
}

std::vector<double> HttpTransport::latenciesMs() const
{
    std::lock_guard g (latMutex);
    return { lat.begin(), lat.end() };
}

double HttpTransport::medianLatencyMs (size_t n) const
{
    std::vector<double> v;
    {
        std::lock_guard g (latMutex);
        auto start = lat.size() > n ? lat.end() - (long) n : lat.begin();
        v.assign (start, lat.end());
    }
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace msid
