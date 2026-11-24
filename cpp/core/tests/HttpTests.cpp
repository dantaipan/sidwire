// HttpTransport against a fake C64U (Connection: close on every reply), like
// tests/test_transport.py's FakeC64U.
#include "msid/HttpTransport.h"
#include "FakeC64U.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <map>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace msid;
using Catch::Matchers::ContainsSubstring;
using msid::test::FakeC64U;

namespace {

HttpTransport::Options opts (int port, bool pre = true)
{
    HttpTransport::Options o;
    o.port = port;
    o.preconnect = pre;
    return o;
}

} // namespace

TEST_CASE ("http: PUT format and memory", "[http]")
{
    for (bool pre : { true, false })
    {
        FakeC64U f;
        HttpTransport t ("127.0.0.1", opts (f.port, pre));
        for (uint8_t i = 0; i < 5; ++i)
            t.write (0xD400, { i, 0xAB });
        t.close();
        std::lock_guard g (f.m);
        CHECK (f.requests.back() == "PUT /v1/machine:writemem?address=D400&data=04AB HTTP/1.1");
        CHECK (f.mem[0xD400] == 4);
        CHECK (f.mem[0xD401] == 0xAB);
        CHECK (t.latenciesMs().size() == 5);
        if (pre)
            CHECK (t.spareHits >= 4);
    }
}

TEST_CASE ("http: >128 bytes are chunked", "[http]")
{
    FakeC64U f;
    HttpTransport t ("127.0.0.1", opts (f.port));
    std::vector<uint8_t> payload (200);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = (uint8_t) i;
    t.write (0x1000, payload);
    t.close();
    std::lock_guard g (f.m);
    REQUIRE (f.requests.size() == 2);
    CHECK_THAT (f.requests[1], ContainsSubstring ("address=1080"));
    CHECK (f.mem[0x1000 + 199] == 199);
}

TEST_CASE ("http: dropped connection is retried", "[http]")
{
    FakeC64U f (1);
    HttpTransport t ("127.0.0.1", opts (f.port));
    t.write (0xD418, { 0x0F });
    t.close();
    std::lock_guard g (f.m);
    CHECK (f.mem[0xD418] == 0x0F);
    CHECK (t.errors == 1);
}

TEST_CASE ("http: API errors and 403 raise", "[http]")
{
    {
        FakeC64U f (0, 200, "{\"errors\": [\"bad address\"]}");
        HttpTransport t ("127.0.0.1", opts (f.port));
        CHECK_THROWS_WITH (t.write (0xD418, { 1 }), ContainsSubstring ("bad address"));
    }
    {
        FakeC64U f (0, 403, "");
        HttpTransport t ("127.0.0.1", opts (f.port));
        CHECK_THROWS_WITH (t.write (0xD418, { 1 }), ContainsSubstring ("403"));
    }
}

TEST_CASE ("http: unreachable host fails after retries", "[http]")
{
    HttpTransport::Options o;
    o.port = 1;   // nothing listens on port 1 locally -> refused
    o.connectTimeout = 0.1;
    HttpTransport t ("127.0.0.1", o);
    CHECK_THROWS_WITH (t.write (0xD418, { 1 }), ContainsSubstring ("failed after 3 attempts"));
}
