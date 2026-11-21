#include <catch2/catch_test_macros.hpp>

#include "app/SmokeFeatures.h"

#include <nlohmann/json.hpp>

#include <string>

using namespace tesseract;
using nlohmann::json;
using namespace tesseract::smoke;

TEST_CASE("smoke_features: parse accepts a valid manifest")
{
    std::string err;
    const auto checks = parse_feature_manifest(
        R"({"checks":[{"name":"a","kind":"event","room_id":"!r:x","expect":{"type":"Poll"}},
                      {"name":"b","kind":"room"},{"name":"c","kind":"action"},
                      {"name":"d","kind":"order"}]})",
        err);
    REQUIRE(err.empty());
    REQUIRE(checks.size() == 4);
    CHECK(checks[0].name == "a");
    CHECK(checks[0].kind == "event");
    CHECK(checks[0].args["room_id"] == "!r:x");
    CHECK(checks[0].args["expect"]["type"] == "Poll");
    CHECK(checks[3].kind == "order");
}

TEST_CASE("smoke_features: parse rejects an unknown kind and names it in the error")
{
    std::string err;
    const auto checks = parse_feature_manifest(R"({"checks":[{"name":"a","kind":"bogus"}]})", err);
    CHECK(checks.empty());
    CHECK(err.find("bogus") != std::string::npos);
}

TEST_CASE("smoke_features: parse rejects non-JSON")
{
    std::string err;
    CHECK(parse_feature_manifest("not json", err).empty());
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK(parse_feature_manifest(R"({"nochecks":1})", err).empty());
    CHECK_FALSE(err.empty());
}

TEST_CASE("smoke_features: event_mismatch is empty on a full match")
{
    Event ev;
    ev.type = EventType::Poll;
    ev.sender = "@a:x";
    ev.body = "hi";
    ev.is_edited = true;
    ev.in_reply_to_id = "$e";
    ev.forwarded = true;
    ev.bot_command = "ping";
    const json expect = {{"type", "Poll"},       {"sender", "@a:x"},       {"body", "hi"},
                         {"is_edited", true},    {"in_reply_to_id", "$e"}, {"forwarded", true},
                         {"bot_command", "ping"}};
    CHECK(event_mismatch(ev, expect) == "");
}

TEST_CASE("smoke_features: event_mismatch names the first wrong key and both values")
{
    Event ev;
    ev.is_edited = false;
    const auto m = event_mismatch(ev, json{{"is_edited", true}});
    CHECK(m == "is_edited: want true, got false");
    CHECK(event_mismatch(ev, json{{"type", "Nope"}}).find("Nope") != std::string::npos);
    CHECK(event_mismatch(ev, json{{"zzz", 1}}) == "unknown key 'zzz'");
}

TEST_CASE("smoke_features: event_mismatch checks reactions by key and count")
{
    Event ev;
    Reaction r;
    r.key = "+";
    r.count = 2;
    ev.reactions.push_back(r);
    CHECK(event_mismatch(ev, json{{"reactions", json::array({{{"key", "+"}, {"count", 2}}})}}) ==
          "");
    CHECK_FALSE(event_mismatch(ev, json{{"reactions", json::array({{{"key", "+"}, {"count", 3}}})}})
                    .empty());
    CHECK_FALSE(event_mismatch(ev, json{{"reactions", json::array({{{"key", "x"}, {"count", 1}}})}})
                    .empty());
}

TEST_CASE("smoke_features: room_mismatch matches last_message_kind and counts")
{
    RoomInfo room;
    room.last_message_kind = "image";
    room.last_message_body = "pic";
    room.unread_count = 3;
    room.notification_count = 2;
    room.highlight_count = 1;
    room.is_favorite = true;
    room.is_direct = true;
    CHECK(room_mismatch(room, json{{"last_message_kind", "image"},
                                   {"last_message_body", "pic"},
                                   {"unread_count", 3},
                                   {"notification_count", 2},
                                   {"highlight_count", 1},
                                   {"is_favorite", true},
                                   {"is_direct", true}}) == "");
    CHECK(room_mismatch(room, json{{"unread_count", 4}}) == "unread_count: want 4, got 3");
    CHECK(room_mismatch(room, json{{"bad", 1}}) == "unknown key 'bad'");
}

TEST_CASE("smoke_features: room_mismatch replaced maps to a non-empty successor_room_id")
{
    RoomInfo room;
    CHECK(room_mismatch(room, json{{"replaced", false}}) == "");
    CHECK_FALSE(room_mismatch(room, json{{"replaced", true}}).empty());
    room.successor_room_id = "!new:x";
    CHECK(room_mismatch(room, json{{"replaced", true}}) == "");
}

TEST_CASE("smoke_features: event_mismatch reports a wrong-typed reactions count without throwing")
{
    Event ev;
    Reaction r;
    r.key = "+";
    r.count = 2;
    ev.reactions.push_back(r);
    for (const json& bad : {json("2"), json(nullptr), json(-1)})
    {
        const json expect = {{"reactions", json::array({{{"key", "+"}, {"count", bad}}})}};
        std::string m;
        REQUIRE_NOTHROW(m = event_mismatch(ev, expect));
        CHECK(m.find("reactions['+'].count") != std::string::npos);
    }
}

TEST_CASE("smoke_features: an absent check passes when no event with that id was recorded")
{
    CHECK(absent_mismatch(false) == "");
}

TEST_CASE("smoke_features: an absent check fails when it was recorded")
{
    CHECK_FALSE(absent_mismatch(true).empty());
}

TEST_CASE("smoke_features: event_mismatch checks the thread summary on a root")
{
    Event ev;
    ev.is_thread_root = true;
    ev.thread_reply_count = 1;
    ev.thread_latest_body = "reply";
    CHECK(event_mismatch(ev, {{"thread_reply_count", 1}, {"thread_latest_body", "reply"}}).empty());
    const auto m = event_mismatch(ev, {{"thread_reply_count", 2}});
    CHECK(m.find("thread_reply_count") != std::string::npos);
    CHECK(m.find("want 2") != std::string::npos);
    CHECK(m.find("got 1") != std::string::npos);
    CHECK(event_mismatch(ev, {{"thread_latest_body", "other"}}).find("thread_latest_body") !=
          std::string::npos);
}

TEST_CASE("smoke_features: parse rejects an empty checks array")
{
    std::string err;
    CHECK(parse_feature_manifest(R"({"checks":[]})", err).empty());
    CHECK(err.find("empty") != std::string::npos);
}

TEST_CASE("smoke_features: manifest_features_room_id reads the top-level id")
{
    CHECK(manifest_features_room_id(R"({"features_room_id":"!f:x","checks":[]})") == "!f:x");
    CHECK(manifest_features_room_id(R"({"checks":[]})").empty());
    CHECK(manifest_features_room_id("not json").empty());
}
