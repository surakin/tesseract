#include <catch2/catch_test_macros.hpp>

#include "linux_notification_reply.h"

using tesseract::linux_notify::NotificationCorrelation;
using tesseract::linux_notify::supports_inline_reply;

TEST_CASE("supports_inline_reply detects the KDE capability string",
          "[linux_notify]")
{
    CHECK(supports_inline_reply(
        {"body", "actions", "inline-reply", "persistence"}));
}

TEST_CASE("supports_inline_reply is false when the capability is absent",
          "[linux_notify]")
{
    CHECK_FALSE(supports_inline_reply({"body", "actions", "persistence"}));
}

TEST_CASE("supports_inline_reply is false for an empty capability list",
          "[linux_notify]")
{
    CHECK_FALSE(supports_inline_reply({}));
}

TEST_CASE("supports_inline_reply is case-sensitive (KDE's string is always "
          "lowercase)",
          "[linux_notify]")
{
    CHECK_FALSE(supports_inline_reply({"Inline-Reply", "INLINE-REPLY"}));
}

TEST_CASE("NotificationCorrelation finds a recorded legacy notification",
          "[linux_notify][correlation]")
{
    NotificationCorrelation c;
    c.record(42, "!room:x", "$event:x");

    auto found = c.find(42);
    REQUIRE(found);
    CHECK(found->room_id == "!room:x");
    CHECK(found->event_id == "$event:x");
}

TEST_CASE("NotificationCorrelation returns nullopt for an unknown id",
          "[linux_notify][correlation]")
{
    NotificationCorrelation c;
    CHECK_FALSE(c.find(1).has_value());
    CHECK_FALSE(c.find_portal("nope").has_value());
}

TEST_CASE("NotificationCorrelation forget erases both the entry and any "
          "pending token",
          "[linux_notify][correlation]")
{
    NotificationCorrelation c;
    c.record(7, "!room:x", "");
    c.stash_token(7, "tok-123");

    c.forget(7);

    CHECK_FALSE(c.find(7).has_value());
    CHECK(c.take_token(7).empty());
}

TEST_CASE("NotificationCorrelation take_token consumes the token exactly "
          "once",
          "[linux_notify][correlation]")
{
    NotificationCorrelation c;
    c.record(9, "!room:x", "");
    c.stash_token(9, "tok-abc");

    CHECK(c.take_token(9) == "tok-abc");
    // Second take (e.g. a duplicate ActionInvoked) gets nothing, not a
    // stale/repeated token.
    CHECK(c.take_token(9).empty());
    // The notification's own room/event correlation is untouched by
    // take_token — only NotificationClosed (forget) should clear that.
    REQUIRE(c.find(9).has_value());
}

TEST_CASE("NotificationCorrelation take_token is empty when the daemon "
          "never sent an ActivationToken",
          "[linux_notify][correlation]")
{
    NotificationCorrelation c;
    c.record(3, "!room:x", "");
    CHECK(c.take_token(3).empty());
}

TEST_CASE("NotificationCorrelation finds a recorded portal notification by "
          "its string id",
          "[linux_notify][correlation]")
{
    NotificationCorrelation c;
    c.record_portal("_sanitized_room_id", "!room:x", "$event:x");

    auto found = c.find_portal("_sanitized_room_id");
    REQUIRE(found);
    CHECK(found->room_id == "!room:x");
    CHECK(found->event_id == "$event:x");
}

TEST_CASE("NotificationCorrelation record_portal overwrites the previous "
          "event_id for the same portal id",
          "[linux_notify][correlation]")
{
    NotificationCorrelation c;
    c.record_portal("_room", "!room:x", "$event:1");
    c.record_portal("_room", "!room:x", "$event:2");

    auto found = c.find_portal("_room");
    REQUIRE(found);
    CHECK(found->event_id == "$event:2");
}

TEST_CASE("NotificationCorrelation legacy and portal maps are independent",
          "[linux_notify][correlation]")
{
    NotificationCorrelation c;
    c.record(1, "!legacy:x", "");
    c.record_portal("1", "!portal:x", "");

    // A legacy numeric id and a portal string id that happen to look alike
    // ("1") must not collide.
    REQUIRE(c.find(1));
    CHECK(c.find(1)->room_id == "!legacy:x");
    REQUIRE(c.find_portal("1"));
    CHECK(c.find_portal("1")->room_id == "!portal:x");
}
