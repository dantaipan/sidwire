#include "msid/Engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace msid;
using Catch::Matchers::ContainsSubstring;

TEST_CASE ("parse presets subset of TOML", "[presets]")
{
    auto p = parsePresets (R"(
# comment
[[preset]]      # 0
name = "A # not a comment"
waveform = "saw"
cutoff = 600
filter_voices = false

[[preset]]
waveform = "tri"
)");
    REQUIRE (p.size() == 2);
    CHECK (*p[0].name == "A # not a comment");
    CHECK (*p[0].waveform == "saw");
    CHECK (*p[0].cutoff == 600);
    CHECK (*p[0].filterVoices == false);
    CHECK_FALSE (p[0].attack.has_value());
    CHECK (*p[1].name == "preset 1");
}

TEST_CASE ("presets reject bad keys and values", "[presets]")
{
    CHECK_THROWS_WITH (parsePresets ("[[preset]]\nwavefrom = \"saw\"\n"), ContainsSubstring ("wavefrom"));
    CHECK_THROWS_WITH (parsePresets ("[[preset]]\nwaveform = \"square\"\n"), ContainsSubstring ("square"));
    CHECK_THROWS_WITH (parsePresets ("[[preset]]\nfilter_mode = \"notch\"\n"), ContainsSubstring ("notch"));
    CHECK_THROWS (parsePresets ("[[preset]]\ncutoff = \"high\"\n"));
    CHECK (loadPresets ("/nonexistent/presets.toml").empty());
}

TEST_CASE ("shipped presets.toml loads", "[presets]")
{
    auto p = loadPresets (MSID_PRESETS_FILE);
    REQUIRE (p.size() >= 8);
    CHECK (*p[0].name == "Square Lead");
    CHECK (*p[7].filterMode == "off");
}
