// The room's world event on the C++ side: parsing a state event (the sender
// comes from the envelope, never the content) and the no-session behaviour of
// the room-world calls.

#include <catch2/catch_test_macros.hpp>

#include <tesseract/client.h>

#include <string>
#include <vector>

using tesseract::Client;

namespace
{
const std::string kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kRoom = "!room:example.invalid";
} // namespace

TEST_CASE("room_world parses a state event", "[client][world_event]")
{
    using Kind = tesseract::RoomWorldRead::Kind;
    // A forged author in the content must not become the sender.
    const std::string content = R"({"name":"Arena","author":"@mallory:example.org",
        "sender":"@mallory:example.org",
        "bundle":{"hash":")" + kHash +
                                R"(","url":"mxc://example.org/abc","size":1234,"format":1}})";
    auto r = tesseract::parse_room_world_read(content, "@alice:example.org");
    REQUIRE(r.kind == Kind::World);
    const auto& w = r.world;
    CHECK(w.name == "Arena");
    CHECK(w.hash == kHash);
    CHECK(w.url == "mxc://example.org/abc");
    CHECK(w.sender == "@alice:example.org");
    CHECK(w.size == 1234);
    CHECK(w.format == 1);

    CHECK(tesseract::parse_room_world_read("{}", "@alice:example.org").kind == Kind::NoWorld);
    CHECK(tesseract::parse_room_world_read("not json", "@alice:example.org").kind == Kind::Error);
    CHECK(tesseract::parse_room_world_read(
              R"({"name":"x","bundle":{"hash":")" + kHash +
                  R"(","url":"https://example.org/x","size":1,"format":1}})",
              "@alice:example.org")
              .kind == Kind::Error);
}

TEST_CASE("can_set_room_world is false without a client", "[client][world_event]")
{
    Client c;
    CHECK_FALSE(c.can_set_room_world(kRoom));
    const auto read = c.room_world(kRoom); // no session: an error, never "no world"
    CHECK(read.kind == tesseract::RoomWorldRead::Kind::Error);
    CHECK_FALSE(read.error.empty());
    CHECK_FALSE(c.set_room_world(kRoom, "Arena", {1, 2, 3}).ok);
    CHECK_FALSE(c.remove_room_world(kRoom).ok);
    std::vector<std::uint8_t> out{9};
    tesseract::RoomWorld w;
    CHECK_FALSE(c.download_world_bundle(w, out).ok);
    CHECK(out.empty());
}

TEST_CASE("room_world read tells no world from an unusable event", "[client][world_event]")
{
    using Kind = tesseract::RoomWorldRead::Kind;
    CHECK(tesseract::parse_room_world_read("{}", "@a:e").kind == Kind::NoWorld);
    const auto bad = tesseract::parse_room_world_read(
        R"({"bundle":{"hash":")" + kHash +
            R"(","url":"mxc://e.org/a","size":"397888","format":1}})",
        "@a:e");
    CHECK(bad.kind == Kind::Error);
    CHECK_FALSE(bad.error.empty());
    CHECK(tesseract::parse_room_world_read("not json", "@a:e").kind == Kind::Error);
    const auto future =
        tesseract::parse_room_world_read(R"({"bundle":{"format":2,"blob":{}}})", "@a:e");
    CHECK(future.kind == Kind::Error);
    CHECK(future.format == 2);
    CHECK(tesseract::parse_room_world_read(R"({"name":"A","bundle":{"hash":")" + kHash +
                                               R"(","url":"mxc://e.org/a","size":5,"format":1}})",
                                           "@a:e")
              .kind == Kind::World);
}
