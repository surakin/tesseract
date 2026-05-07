#include <catch2/catch_test_macros.hpp>
#include "tesseract/client.h"
#include "tesseract/types.h"

// ---------------------------------------------------------------------------
// tesseract::Result
// ---------------------------------------------------------------------------

TEST_CASE("Result bool operator reflects ok field", "[types]") {
    tesseract::Result ok{.ok = true,  .message = "all good"};
    tesseract::Result fail{.ok = false, .message = "oops"};

    CHECK(static_cast<bool>(ok));
    CHECK_FALSE(static_cast<bool>(fail));
    CHECK(ok.message   == "all good");
    CHECK(fail.message == "oops");
}

TEST_CASE("Result default-constructs to not-ok", "[types]") {
    tesseract::Result r{};
    CHECK_FALSE(static_cast<bool>(r));
    CHECK(r.message.empty());
}

// ---------------------------------------------------------------------------
// tesseract::Client::OAuthFlow
// ---------------------------------------------------------------------------

TEST_CASE("OAuthFlow bool operator reflects ok field", "[types]") {
    tesseract::Client::OAuthFlow success{
        .ok       = true,
        .auth_url = "https://auth.example/oauth",
    };
    tesseract::Client::OAuthFlow failure{
        .ok      = false,
        .message = "discovery failed",
    };

    CHECK(static_cast<bool>(success));
    CHECK_FALSE(static_cast<bool>(failure));
    CHECK(success.auth_url == "https://auth.example/oauth");
    CHECK(failure.message  == "discovery failed");
}

TEST_CASE("OAuthFlow default-constructs to not-ok with empty URLs", "[types]") {
    tesseract::Client::OAuthFlow f{};
    CHECK_FALSE(static_cast<bool>(f));
    CHECK(f.auth_url.empty());
    CHECK(f.redirect_uri.empty());
}

// ---------------------------------------------------------------------------
// tesseract::RoomInfo
// ---------------------------------------------------------------------------

TEST_CASE("RoomInfo default-initialised fields", "[types]") {
    tesseract::RoomInfo r{};
    CHECK(r.id.empty());
    CHECK(r.name.empty());
    CHECK(r.topic.empty());
    CHECK(r.unread_count == 0u);
    CHECK_FALSE(r.is_direct);
}

// ---------------------------------------------------------------------------
// tesseract::Message
// ---------------------------------------------------------------------------

TEST_CASE("Message default-initialised fields", "[types]") {
    tesseract::Message m{};
    CHECK(m.event_id.empty());
    CHECK(m.room_id.empty());
    CHECK(m.sender.empty());
    CHECK(m.body.empty());
    CHECK(m.timestamp == 0u);
    CHECK(m.msg_type.empty());
}
