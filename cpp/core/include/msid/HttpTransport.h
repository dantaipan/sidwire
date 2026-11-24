// PUT /v1/machine:writemem over raw HTTP/1.1 sockets (port of transport.py HttpTransport).
// The C64U closes every connection (Connection: close), so one socket is pre-opened
// for the next request; see NOTES.md for the measurements behind this.
#pragma once

#include "msid/Flusher.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace msid {

class HttpTransport : public Transport
{
public:
    struct Options
    {
        int port = 80;
        std::string password;
        double timeout = 1.0;
        double connectTimeout = 0.3;
        bool preconnect = true;
        double maxIdle = 5.0;
        int retries = 2;
    };

    HttpTransport (std::string host, Options opts);
    explicit HttpTransport (std::string hostName) : HttpTransport (std::move (hostName), Options {}) {}
    ~HttpTransport() override;

    void write (uint16_t address, const std::vector<uint8_t>& data) override;
    void close() override;

    /// Median of the last n request latencies in ms (0 if none).
    double medianLatencyMs (size_t n = 50) const;
    std::vector<double> latenciesMs() const;
    int errors = 0, spareHits = 0, spareMisses = 0;

    static constexpr size_t kMaxWrite = 128;

private:
    void writeOne (uint16_t address, const uint8_t* data, size_t len);
    void request (uint16_t address, const uint8_t* data, size_t len);
    int takeSocket();
    void startConnect();
    void dropSpare();
    int connectBlocking();

    std::string host;
    Options o;
    int spareFd = -1;
    std::chrono::steady_clock::time_point spareTime;
    mutable std::mutex latMutex;
    std::deque<double> lat;
};

} // namespace msid
