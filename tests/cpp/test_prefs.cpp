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
    p.last_room = "!a:h";
    p.open_rooms = {"!a:h", "!b:h", "!c:h"};
    auto json = serialize(p);
    auto p2 = parse(json);
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
    p.last_room = "!only:host";
    p.open_rooms = {"!only:host"};
    auto json = serialize(p);
    auto p2 = parse(json);
    CHECK(p2.last_room == "!only:host");
    REQUIRE(p2.open_rooms.size() == 1);
    CHECK(p2.open_rooms[0] == "!only:host");
}

TEST_CASE("Prefs serialize writes bridge overrides and parse reads them back")
{
    PrefsData p;
    p.bridge_not_bridged_overrides = {"!a:x", "!b:x"};
    auto back = parse(serialize(p));
    CHECK(back.bridge_not_bridged_overrides == std::vector<std::string>{"!a:x", "!b:x"});

    // Clearing the last override removes it from the stored event too.
    p.bridge_not_bridged_overrides.clear();
    auto cleared = parse(serialize(p, R"({"bridge_overrides":["!a:x"]})"));
    CHECK(cleared.bridge_not_bridged_overrides.empty());
}

TEST_CASE("Prefs recent_rooms round-trips in visit order")
{
    PrefsData p;
    p.recent_rooms = {"!c:x", "!a:x", "!b:x"};
    auto back = parse(serialize(p));
    CHECK(back.recent_rooms == std::vector<std::string>{"!c:x", "!a:x", "!b:x"});
}

TEST_CASE("Prefs parse without recent_rooms gives an empty list")
{
    CHECK(parse(R"({"last_room":"!r:x"})").recent_rooms.empty());
}

TEST_CASE("Prefs serialize with empty recent_rooms removes the stored key")
{
    PrefsData p;
    auto json = serialize(p, R"({"recent_rooms":["!a:x"]})");
    CHECK(json.find("recent_rooms") == std::string::npos);
    CHECK(parse(json).recent_rooms.empty());
}

TEST_CASE("Prefs layout save carries recent_rooms alongside the tab layout")
{
    auto p = tesseract::Prefs::room_layout("!b:x", {"!a:x", "!b:x"});
    p.recent_rooms = {"!b:x", "!z:x"};
    auto back = parse(serialize(p, R"({"recent_rooms":["!old:x"],"future":1})"));
    CHECK(back.last_room == "!b:x");
    CHECK(back.recent_rooms == std::vector<std::string>{"!b:x", "!z:x"});
}

TEST_CASE("Prefs directory_servers round-trips and is capped on parse")
{
    PrefsData p;
    p.directory_servers = {"a.org", "b.org"};
    auto p2 = parse(serialize(p));
    CHECK(p2.directory_servers == p.directory_servers);

    auto many = parse(R"({"directory_servers":["1","2","3","4","5","6","7","8","9","10"]})");
    CHECK(many.directory_servers.size() == tesseract::kMaxDirectoryServers);

    // Empty list drops the key but keeps unrelated base keys.
    auto json = serialize(PrefsData{}, R"({"directory_servers":["x"],"keep":1})");
    CHECK(json.find("directory_servers") == std::string::npos);
    CHECK(json.find("keep") != std::string::npos);
}

TEST_CASE("Prefs add_directory_server moves to front, dedupes, caps at 8")
{
    using tesseract::Prefs::add_directory_server;
    std::vector<std::string> l;
    l = add_directory_server(l, "a.org");
    l = add_directory_server(l, "b.org");
    l = add_directory_server(l, "A.ORG");
    CHECK(l == std::vector<std::string>{"A.ORG", "b.org"});
    CHECK(add_directory_server(l, "") == l);
    for (int i = 0; i < 10; ++i)
    {
        l = add_directory_server(l, "s" + std::to_string(i) + ".org");
    }
    CHECK(l.size() == tesseract::kMaxDirectoryServers);
    CHECK(l.front() == "s9.org");
}

TEST_CASE("Prefs directory_server_choices lists defaults until pushed out")
{
    using tesseract::Prefs::directory_server_choices;
    CHECK(directory_server_choices({}, "home.org") ==
          std::vector<std::string>{"home.org", "activematrixrooms.com", "matrixrooms.info"});
    CHECK(
        directory_server_choices({"x.org", "MatrixRooms.info"}, "home.org") ==
        std::vector<std::string>{"x.org", "MatrixRooms.info", "home.org", "activematrixrooms.com"});
    CHECK(directory_server_choices({}, "") ==
          std::vector<std::string>{"activematrixrooms.com", "matrixrooms.info"});
    std::vector<std::string> full{"1", "2", "3", "4", "5", "6", "7", "8"};
    CHECK(directory_server_choices(full, "home.org") == full);
}

TEST_CASE("Prefs pinned_spaces round-trips in tab order")
{
    PrefsData p;
    p.pinned_spaces = {"!b:x", "!a:x"};
    auto back = parse(serialize(p));
    CHECK(back.pinned_spaces == std::vector<std::string>{"!b:x", "!a:x"});
}

TEST_CASE("Prefs without pinned_spaces parses empty and serialize drops the key")
{
    CHECK(parse(R"({"last_room":"!r:x"})").pinned_spaces.empty());
    PrefsData p;
    auto json = serialize(p, R"({"pinned_spaces":["!a:x"]})");
    CHECK(json.find("pinned_spaces") == std::string::npos);
}

TEST_CASE("Prefs without_layout forgets the layout and keeps everything else")
{
    PrefsData p;
    p.last_room = "!r:x";
    p.open_rooms = {"!r:x", "!s:x"};
    p.recent_rooms = {"!s:x"};
    p.pinned_spaces = {"!sp:x"};
    p.directory_servers = {"example.org"};
    p.bridge_not_bridged_overrides = {"!b:x"};
    p.emoji_skin_tone = "medium";

    const PrefsData out = tesseract::Prefs::without_layout(p);
    CHECK(out.last_room.empty());
    CHECK(out.open_rooms.empty());
    CHECK(out.recent_rooms.empty());
    CHECK(out.pinned_spaces == std::vector<std::string>{"!sp:x"});
    CHECK(out.directory_servers == std::vector<std::string>{"example.org"});
    CHECK(out.bridge_not_bridged_overrides == std::vector<std::string>{"!b:x"});
    CHECK(out.emoji_skin_tone == "medium");

    const auto json = serialize(out, R"({"future_key":1,"pinned_spaces":["!old:x"]})");
    CHECK(parse(json).pinned_spaces == std::vector<std::string>{"!sp:x"});
    CHECK(json.find("future_key") != std::string::npos);
}
