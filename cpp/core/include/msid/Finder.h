// Finder: look for C64 Ultimates on the local network, the way PS64 does (src/discover.c there):
// try port 80 on every address of the local /24, 64 at a time with a short connect window, then
// ask each host that answered for GET /v1/info and keep the ones whose product names an Ultimate.
// A 403 means a network password is set: listed as locked.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace msid {

struct FoundDevice
{
    std::string ip, hostname, product, firmware;
    bool locked = false;   // answered 403: network password set
    bool operator== (const FoundDevice&) const = default;
};

struct FindOptions
{
    std::vector<std::string> hosts;   // probe exactly these (tests); empty = the local /24 (+ alsoNear's)
    std::string alsoNear;             // e.g. the last address used: its /24 is searched too
    std::string password;             // sent as X-Password
    int port = 80;
    int connectMs = 600;
    int batch = 64;
};

/// Blocking (about 3 s for a /24). progress gets 0-100; set cancel to stop early.
std::vector<FoundDevice> findUltimates (const FindOptions& options,
                                        const std::function<void (int)>& progress = {},
                                        const std::atomic<bool>* cancel = nullptr);

/// Fill d from a /v1/info body; true if the product is an Ultimate.
bool parseUltimateInfo (const std::string& body, FoundDevice& d);

/// The address this machine uses on its LAN ("" if none).
std::string localAddress();

} // namespace msid
