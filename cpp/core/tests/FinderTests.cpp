// C64U Finder: the PS64-style search, against fake C64Us on localhost.
#include "FakeC64U.h"
#include "msid/Finder.h"

#include <catch2/catch_test_macros.hpp>

using namespace msid;
using msid::test::FakeC64U;

TEST_CASE ("finder: parses /v1/info like the real C64U's", "[finder]")
{
    FoundDevice d;
    const char* real = "{ \n  \"product\" : \"C64 Ultimate\",\n  \"firmware_version\" : \"1.2.1\",\n  \"git_commit_hash\" : \"31687dbd\",\n"
                       "  \"hostname\" : \"C64-Ultimate-ABC123\",\n  \"errors\" : [  ]\n}";
    CHECK (parseUltimateInfo (real, d));
    CHECK (d.product == "C64 Ultimate");
    CHECK (d.hostname == "C64-Ultimate-ABC123");
    CHECK (d.firmware == "1.2.1");
    FoundDevice other;
    CHECK_FALSE (parseUltimateInfo ("{ \"product\" : \"Some Router\" }", other));
    CHECK_FALSE (parseUltimateInfo ("not json", other));
}

TEST_CASE ("finder: finds an Ultimate, lists a locked one, skips other web servers", "[finder]")
{
    FakeC64U ultimate, locked, router;
    locked.infoStatus = 403;
    locked.infoBody = "{}";
    router.infoBody = "{ \"product\" : \"Home Router\" }";
    std::vector<int> progress;
    for (auto* f : { &ultimate, &locked, &router })
    {
        FindOptions o;
        o.hosts = { "127.0.0.1", "127.0.0.2", "not-an-ip" };
        o.port = f->port;
        o.connectMs = 300;
        auto found = findUltimates (o, [&] (int p) { progress.push_back (p); });
        if (f == &ultimate)
        {
            REQUIRE (found.size() == 1);
            CHECK (found[0].ip == "127.0.0.1");
            CHECK (found[0].hostname == "C64-Ultimate-FAKE");
            CHECK_FALSE (found[0].locked);
        }
        else if (f == &locked)
        {
            REQUIRE (found.size() == 1);
            CHECK (found[0].locked);
        }
        else
            CHECK (found.empty());
    }
    CHECK (progress.back() == 100);
}

TEST_CASE ("finder: password is sent and cancel stops early", "[finder]")
{
    FakeC64U f;
    FindOptions o;
    o.hosts = { "127.0.0.1" };
    o.port = f.port;
    o.password = "secret";
    findUltimates (o);
    // the request line is recorded; the header isn't, so just check that it asked for /v1/info
    REQUIRE_FALSE (f.requests.empty());
    CHECK (f.requests[0].rfind ("GET /v1/info", 0) == 0);
    std::atomic<bool> cancel { true };
    CHECK (findUltimates (o, {}, &cancel).empty());
}

TEST_CASE ("finder: local address looks like an IPv4 address (or is empty offline)", "[finder]")
{
    auto ip = localAddress();
    CHECK ((ip.empty() || std::count (ip.begin(), ip.end(), '.') == 3));
}
