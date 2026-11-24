// Minimal fake C64U HTTP server for tests: answers PUT /v1/machine:writemem like the real
// device (Connection: close on every reply) and keeps the written memory. GET readmem returns it.
#pragma once

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace msid::test {

class FakeC64U
{
public:
    explicit FakeC64U (int failFirstN = 0, int statusCode = 200, std::string replyBody = "{ \"errors\" : [  ] }")
        : failFirst (failFirstN), status (statusCode), body (std::move (replyBody))
    {
        fd = ::socket (AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind (fd, (sockaddr*) &a, sizeof a);
        socklen_t len = sizeof a;
        getsockname (fd, (sockaddr*) &a, &len);
        port = ntohs (a.sin_port);
        ::listen (fd, 8);
        mem[0x0314] = 0x31;   // KERNAL IRQ vector $EA31: idle at BASIC
        mem[0x0315] = 0xEA;
        th = std::thread ([this] { serve(); });
    }
    ~FakeC64U()
    {
        stop = true;
        ::shutdown (fd, SHUT_RDWR);
        ::close (fd);
        th.join();
        {
            std::lock_guard g (connMutex);
            for (int c : clientFds)
                ::shutdown (c, SHUT_RDWR);   // e.g. a pre-opened socket nobody used
        }
        for (auto& t : clients)
            t.join();
    }

    int port = 0;
    std::vector<std::string> requests;
    // GET /v1/info (C64U Finder); set before the first request
    std::string infoBody = "{ \"product\" : \"C64 Ultimate\", \"firmware_version\" : \"1.2.1\", "
                           "\"hostname\" : \"C64-Ultimate-FAKE\", \"errors\" : [  ] }";
    int infoStatus = 200;
    std::map<int, uint8_t> mem;
    std::mutex m;

    uint8_t at (int addr) { std::lock_guard g (m); auto it = mem.find (addr); return it == mem.end() ? 0 : it->second; }

private:
    void serve()
    {
        // one thread per connection, like the real device: an idle pre-opened socket must not
        // block other clients
        while (! stop)
        {
            int c = ::accept (fd, nullptr, nullptr);
            if (c < 0)
                return;
            std::lock_guard g (connMutex);
            clientFds.push_back (c);
            clients.emplace_back ([this, c] { handle (c); });
        }
    }

    void handle (int c)
    {
        {
            std::string buf;
            char tmp[4096];
            while (buf.find ("\r\n\r\n") == std::string::npos)
            {
                auto n = ::recv (c, tmp, sizeof tmp, 0);
                if (n <= 0)
                    break;
                buf.append (tmp, (size_t) n);
            }
            if (buf.empty()) { closeClient (c); return; }   // pre-opened socket closed unused
            auto line = buf.substr (0, buf.find ("\r\n"));
            {
                std::lock_guard g (m);
                requests.push_back (line);
            }
            if (failFirst > 0) { --failFirst; closeClient (c); return; }
            auto q = line.find ("address=");
            std::string reply = body;
            int code = status;
            if (line.rfind ("GET /v1/info", 0) == 0)
            {
                std::lock_guard g (m);
                reply = infoBody;
                code = infoStatus;
            }
            else if (status == 200 && q != std::string::npos && line.rfind ("GET", 0) == 0)
            {
                int addr = std::stoi (line.substr (q + 8, 4), nullptr, 16);
                int len = std::stoi (line.substr (line.find ("length=") + 7));
                std::lock_guard g (m);
                reply.clear();
                for (int i = 0; i < len; ++i)
                    reply += (char) mem[addr + i];
            }
            else if (status == 200 && q != std::string::npos)
            {
                int addr = std::stoi (line.substr (q + 8, 4), nullptr, 16);
                auto d = line.find ("data=") + 5;
                auto e = line.find (' ', d);
                std::lock_guard g (m);
                for (size_t i = d; i + 1 < e; i += 2)
                    mem[addr++] = (uint8_t) std::stoi (line.substr (i, 2), nullptr, 16);
            }
            auto resp = "HTTP/1.1 " + std::to_string (code) + " X\r\nConnection: close\r\n"
                        "Content-Type: application/json\r\nContent-Length: " + std::to_string (reply.size())
                        + "\r\n\r\n" + reply;
            ::send (c, resp.data(), resp.size(), 0);
            closeClient (c);
        }
    }

    void closeClient (int c)
    {
        std::lock_guard g (connMutex);
        clientFds.erase (std::remove (clientFds.begin(), clientFds.end(), c), clientFds.end());
        ::close (c);
    }

    int fd = -1;
    std::atomic<int> failFirst;
    int status;
    std::string body;
    std::atomic<bool> stop { false };
    std::thread th;
    std::mutex connMutex;
    std::vector<int> clientFds;
    std::vector<std::thread> clients;
};

} // namespace msid::test
