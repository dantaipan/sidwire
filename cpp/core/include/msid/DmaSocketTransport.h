// Ultimate DMA Service (TCP 64) transport: one persistent connection, binary commands.
// Port of transport.py DmaSocketTransport. Framing (firmware software/network/socket_dma.cc):
// u16 LE command (0xFFxx), u16 LE length, payload. DMAWRITE (0xFF06) = u16 LE address + data,
// no reply. Needs "Ultimate DMA Service" enabled on the C64U.
//
// Measured on the C64U (NOTES.md, Phase 4):
//  * DMAWRITE is fire-and-forget, so IDENTIFY (reply: length byte + text) serves as an in-order
//    acknowledgement: endFlush() sends one after each flush and beginFlush() waits until at most
//    maxInFlight flushes are unacknowledged. That bounds the backlog (bursts run ~0.8 ms/command,
//    sustained load beyond ~150 commands/s drops to ~5 ms/command) and detects dead links.
//  * The service closes a connection after 1.0 s without a command; a write sent into the closed
//    socket is silently lost. idle() sends an IDENTIFY keepalive, and a connection that went
//    quiet anyway is reopened before writing.
#pragma once

#include "msid/Flusher.h"

#include <chrono>
#include <deque>
#include <string>

namespace msid {

class DmaSocketTransport : public Transport
{
public:
    static constexpr uint16_t kCmdDmaWrite = 0xFF06, kCmdIdentify = 0xFF0E, kCmdAuthenticate = 0xFF1F;

    DmaSocketTransport (std::string host, int port = 64, std::string password = {}, double timeout = 1.0,
                        int retries = 1, int maxInFlight = 1, double keepalive = 0.4);
    ~DmaSocketTransport() override;

    void write (uint16_t address, const std::vector<uint8_t>& data) override;
    void beginFlush() override;
    void endFlush() override;
    void idle() override;
    void close() override;
    std::string identify();

    int errors = 0, reconnects = 0, keepalivesSent = 0;
    double stale = 0.9;   // s without a command after which the service has (nearly) closed on us
    int inFlight() const { return (int) acks.size(); }

    static std::vector<uint8_t> frame (uint16_t cmd, const std::vector<uint8_t>& payload);

private:
    using Clock = std::chrono::steady_clock;
    void connectNow();
    void sendAll (const std::vector<uint8_t>&);
    void sendCmd (const std::vector<uint8_t>&);
    void pump (double wait = 0.0);
    void ensure();
    void waitAcks (size_t limit);
    bool lossy() const { return unacked || ! acks.empty(); }
    double since (Clock::time_point t) const { return std::chrono::duration<double> (Clock::now() - t).count(); }

    std::string host, password;
    int port;
    double timeout;
    int retries, maxInFlight;
    double keepalive;
    int fd = -1;
    std::vector<uint8_t> rx;
    std::deque<Clock::time_point> acks;   // send time of each unanswered IDENTIFY
    bool unacked = false;                 // writes sent since the last IDENTIFY
    Clock::time_point lastSend = Clock::now();
    std::string lastReply;
};

} // namespace msid
