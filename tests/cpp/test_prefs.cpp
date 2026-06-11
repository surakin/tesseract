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

TEST_CASE("Prefs parse open_rooms array")
{
    auto p = parse(R"({"last_room":"!a:h","open_rooms":["!a:h","!b:h","!c:h"]})");
    CHECK(p.last_room == "!a:h");
    REQUIRE(p.open_rooms.size() == 3);
    CHECK(p.open_rooms[0] == "!a:h");
    CHECK(p.open_rooms[1] == "!b:h");
    CHECK(p.open_rooms[2] == "!c:h");
}

TEST_CASE("Prefs backward compat: last_room only populates open_rooms")
{
    auto p = parse(R"({"last_room":"!r:host"})");
    CHECK(p.last_room == "!r:host");
    REQUIRE(p.open_rooms.size() == 1);
    CHECK(p.open_rooms[0] == "!r:host");
}

TEST_CASE("Prefs serialize open_rooms round-trips")
{
    PrefsData p;
    p.last_room  = "!a:h";
    p.open_rooms = {"!a:h", "!b:h", "!c:h"};
    auto json = serialize(p);
    auto p2   = parse(json);
    CHECK(p2.last_room == "!a:h");
    REQUIRE(p2.open_rooms.size() == 3);
    CHECK(p2.open_rooms[0] == "!a:h");
    CHECK(p2.open_rooms[1] == "!b:h");
    CHECK(p2.open_rooms[2] == "!c:h");
}

TEST_CASE("Prefs room_layout builds last_room + open tabs in order")
{
    auto p = tesseract::Prefs::room_layout("!a:h", {"!a:h", "!b:h", "!c:h"});
    CHECK(p.last_room == "!a:h");
    REQUIRE(p.open_rooms.size() == 3);
    CHECK(p.open_rooms[0] == "!a:h");
    CHECK(p.open_rooms[1] == "!b:h");
    CHECK(p.open_rooms[2] == "!c:h");
}

TEST_CASE("Prefs room_layout falls back to the active room when no tabs are open")
{
    auto p = tesseract::Prefs::room_layout("!solo:h", {});
    CHECK(p.last_room == "!solo:h");
    REQUIRE(p.open_rooms.size() == 1);
    CHECK(p.open_rooms[0] == "!solo:h");
}

TEST_CASE("Prefs room_layout with no active room and no tabs stays empty")
{
    auto p = tesseract::Prefs::room_layout("", {});
    CHECK(p.last_room.empty());
    CHECK(p.open_rooms.empty());
}

TEST_CASE("Prefs serialize single tab produces open_rooms of size 1")
{
    PrefsData p;
    p.last_room  = "!only:host";
    p.open_rooms = {"!only:host"};
    auto json = serialize(p);
    auto p2   = parse(json);
    CHECK(p2.last_room == "!only:host");
    REQUIRE(p2.open_rooms.size() == 1);
    CHECK(p2.open_rooms[0] == "!only:host");
}
