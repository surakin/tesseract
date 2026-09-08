#include <catch2/catch_test_macros.hpp>

#include "tesseract/client.h"
#include "tesseract/types.h"

#include <string>

using tesseract::ExtendedProfile;
using tesseract::UserProfile;

TEST_CASE("UserProfile::from_json parses MSC4426 status + call fields",
          "[profile][msc4426]")
{
    const std::string json = R"({
        "exists": true,
        "user_id": "@alice:example.org",
        "display_name": "Alice",
        "avatar_url": "",
        "pronouns": [],
        "tz": "",
        "biography": "",
        "status_text": "On holiday",
        "status_emoji": "PALM",
        "call_joined_ts": 1770140640
    })";

    auto p = UserProfile::from_json(json);

    REQUIRE(p.exists);
    CHECK(p.status_text == "On holiday");
    CHECK(p.status_emoji == "PALM");
    CHECK(p.call_joined_ts == 1770140640ull);
}

TEST_CASE("UserProfile::from_json defaults MSC4426 fields when absent",
          "[profile][msc4426]")
{
    auto p = UserProfile::from_json(
        R"({"exists":true,"user_id":"@a:b","display_name":"a"})");

    CHECK(p.status_text.empty());
    CHECK(p.status_emoji.empty());
    CHECK(p.call_joined_ts == 0ull);
}

TEST_CASE("ExtendedProfile::apply_field sets then clears m.status",
          "[profile][msc4426]")
{
    ExtendedProfile ep;

    ep.apply_field("org.matrix.msc4426.status",
                   R"({"text":"Deep work","emoji":"X"})");
    CHECK(ep.status_text == "Deep work");
    CHECK(ep.status_emoji == "X");

    ep.apply_field("org.matrix.msc4426.status", "null");
    CHECK(ep.status_text.empty());
    CHECK(ep.status_emoji.empty());
}

TEST_CASE("ExtendedProfile::apply_field leaves siblings untouched on status write",
          "[profile][msc4426]")
{
    ExtendedProfile ep;
    ep.tz = "Europe/Madrid";
    ep.biography = "hi";

    ep.apply_field("org.matrix.msc4426.status", R"({"text":"busy","emoji":""})");

    CHECK(ep.tz == "Europe/Madrid");
    CHECK(ep.biography == "hi");
    CHECK(ep.status_text == "busy");
}
