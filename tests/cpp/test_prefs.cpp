#include <catch2/catch_test_macros.hpp>
#include <tesseract/prefs.h>

using tesseract::PrefsData;
using tesseract::Prefs::parse;
using tesseract::Prefs::serialize;

TEST_CASE("Prefs parse empty object gives empty last_room")
{
    auto p = parse("{}");
    CHECK(p.last_room.empty());
}

TEST_CASE("Prefs parse last_room extracts room ID")
{
    auto p = parse("{\"last_room\":\"!abc:example.com\"}");
    CHECK(p.last_room == "!abc:example.com");
}

TEST_CASE("Prefs parse ignores unknown keys")
{
    auto p = parse("{\"unknown\":\"value\",\"last_room\":\"!r:host\"}");
    CHECK(p.last_room == "!r:host");
}

TEST_CASE("Prefs parse missing key gives empty string")
{
    auto p = parse("{\"other\":\"stuff\"}");
    CHECK(p.last_room.empty());
}

TEST_CASE("Prefs serialize round-trips through parse")
{
    PrefsData p;
    p.last_room = "!xyz:matrix.org";
    auto json = serialize(p);
    auto p2 = parse(json);
    CHECK(p2.last_room == p.last_room);
}

TEST_CASE("Prefs serialize empty last_room produces valid JSON")
{
    PrefsData p;
    auto json = serialize(p);
    CHECK(!json.empty());
    // Round-trip: parse back and confirm last_room is still empty.
    auto p2 = parse(json);
    CHECK(p2.last_room.empty());
}
