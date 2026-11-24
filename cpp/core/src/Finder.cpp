#include "msid/Finder.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace msid {

namespace {

std::string jsonString (const std::string& body, const std::string& key)
{
    auto k = body.find ("\"" + key + "\"");
    if (k == std::string::npos)
        return {};
    auto colon = body.find (':', k);
    auto q1 = colon == std::string::npos ? colon : body.find ('"', colon);
    auto q2 = q1 == std::string::npos ? q1 : body.find ('"', q1 + 1);
    if (q2 == std::string::npos)
        return {};
    return body.substr (q1 + 1, q2 - q1 - 1);
}

/// GET /v1/info; returns the HTTP status (0 = no answer) and the body.
int getInfo (const std::string& ip, int port, const std::string& password, std::string& body)
{
    int s = ::socket (AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return 0;
    timeval tv { 2, 0 };
    setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt (s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons ((uint16_t) port);
    if (inet_pton (AF_INET, ip.c_str(), &a.sin_addr) != 1 || ::connect (s, (sockaddr*) &a, sizeof a) != 0)
    {
        ::close (s);
        return 0;
    }
    std::string req = "GET /v1/info HTTP/1.1\r\nHost: " + ip + "\r\n";
    if (! password.empty())
        req += "X-Password: " + password + "\r\n";
    req += "Connection: close\r\n\r\n";
    ::send (s, req.data(), req.size(), 0);
    std::string resp;
    char buf[2048];
    for (ssize_t n; (n = ::recv (s, buf, sizeof buf, 0)) > 0 && resp.size() < 65536;)
        resp.append (buf, (size_t) n);
    ::close (s);
    if (resp.size() < 12 || resp.rfind ("HTTP/", 0) != 0)
        return 0;
    auto hdrEnd = resp.find ("\r\n\r\n");
    body = hdrEnd == std::string::npos ? std::string() : resp.substr (hdrEnd + 4);
    return std::atoi (resp.substr (9, 3).c_str());
}

uint32_t net24 (const std::string& ip)
{
    in_addr a {};
    if (inet_pton (AF_INET, ip.c_str(), &a) != 1)
        return 0;
    return ntohl (a.s_addr) & 0xFFFFFF00u;
}

std::string ipOf (uint32_t hostOrder)
{
    in_addr a { htonl (hostOrder) };
    char buf[INET_ADDRSTRLEN] {};
    inet_ntop (AF_INET, &a, buf, sizeof buf);
    return buf;
}

/// Which of these hosts accept a TCP connection on port within connectMs (all at once).
std::vector<std::string> openHosts (const std::vector<std::string>& hosts, int port, int connectMs)
{
    std::vector<int> fds (hosts.size(), -1);
    std::vector<pollfd> pfd (hosts.size());
    for (size_t i = 0; i < hosts.size(); ++i)
    {
        pfd[i] = { -1, POLLOUT, 0 };
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons ((uint16_t) port);
        if (inet_pton (AF_INET, hosts[i].c_str(), &a.sin_addr) != 1)
            continue;
        int s = ::socket (AF_INET, SOCK_STREAM, 0);
        if (s < 0)
            continue;
        fds[i] = s;
        fcntl (s, F_SETFL, fcntl (s, F_GETFL) | O_NONBLOCK);
        int rc = ::connect (s, (sockaddr*) &a, sizeof a);
        if (rc == 0 || errno == EINPROGRESS)
            pfd[i].fd = s;
    }
    std::vector<std::string> open;
    size_t pending = 0;
    for (auto& p : pfd)
        pending += p.fd >= 0;
    auto t0 = std::chrono::steady_clock::now();
    // wait on the clock, not on poll(): refused connections return at once
    while (pending > 0)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= connectMs)
            break;
        int r = ::poll (pfd.data(), (nfds_t) pfd.size(), (int) (connectMs - elapsed));
        if (r <= 0)
            continue;
        for (size_t i = 0; i < pfd.size(); ++i)
        {
            if (pfd[i].fd < 0 || ! pfd[i].revents)
                continue;
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt (pfd[i].fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0 && (pfd[i].revents & POLLOUT))
                open.push_back (hosts[i]);
            pfd[i].fd = -1;
            --pending;
        }
    }
    for (int s : fds)
        if (s >= 0)
            ::close (s);
    return open;
}

} // namespace

bool parseUltimateInfo (const std::string& body, FoundDevice& d)
{
    d.product = jsonString (body, "product");
    d.hostname = jsonString (body, "hostname");
    d.firmware = jsonString (body, "firmware_version");
    return d.product.find ("Ultimate") != std::string::npos;
}

std::string localAddress()
{
    // a UDP "connect" picks the outgoing interface without sending anything
    int s = ::socket (AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return {};
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons (80);
    inet_pton (AF_INET, "8.8.8.8", &a.sin_addr);
    std::string ip;
    if (::connect (s, (sockaddr*) &a, sizeof a) == 0)
    {
        sockaddr_in me {};
        socklen_t len = sizeof me;
        if (getsockname (s, (sockaddr*) &me, &len) == 0)
            ip = ipOf (ntohl (me.sin_addr.s_addr));
    }
    ::close (s);
    return ip == "0.0.0.0" ? std::string() : ip;
}

std::vector<FoundDevice> findUltimates (const FindOptions& o, const std::function<void (int)>& progress,
                                        const std::atomic<bool>* cancel)
{
    auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };
    std::vector<std::string> hosts = o.hosts;
    if (hosts.empty())
    {
        std::vector<uint32_t> nets;
        if (auto me = net24 (localAddress()))
            nets.push_back (me);
        if (auto near = net24 (o.alsoNear); near && (nets.empty() || near != nets[0]))
            nets.push_back (near);
        for (auto n : nets)
            for (uint32_t h = 1; h <= 254; ++h)
                hosts.push_back (ipOf (n | h));
    }
    std::vector<FoundDevice> found;
    size_t step = (size_t) std::max (1, o.batch);
    for (size_t first = 0; first < hosts.size() && ! cancelled(); first += step)
    {
        std::vector<std::string> batch (hosts.begin() + (long) first, hosts.begin() + (long) std::min (hosts.size(), first + step));
        for (auto& ip : openHosts (batch, o.port, o.connectMs))
        {
            if (cancelled())
                break;
            std::string body;
            FoundDevice d;
            d.ip = ip;
            int status = getInfo (ip, o.port, o.password, body);
            if (status == 200 && parseUltimateInfo (body, d))
                found.push_back (d);
            else if (status == 403)
            {
                d.locked = true;
                d.product = "Password protected";
                found.push_back (d);
            }
        }
        if (progress)
            progress ((int) (std::min (hosts.size(), first + step) * 100 / hosts.size()));
    }
    return found;
}

} // namespace msid
