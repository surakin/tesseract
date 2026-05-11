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
    CHECK_FALSE(r.is_space);
}

// ---------------------------------------------------------------------------
// tesseract::Event (base)
// ---------------------------------------------------------------------------

TEST_CASE("Event default-initialised fields", "[types]") {
    tesseract::Event ev{};
    CHECK(ev.event_id.empty());
    CHECK(ev.room_id.empty());
    CHECK(ev.sender.empty());
    CHECK(ev.body.empty());
    CHECK(ev.timestamp == 0u);
    CHECK(ev.type == tesseract::EventType::Unhandled);
}

// ---------------------------------------------------------------------------
// tesseract::TextEvent
// ---------------------------------------------------------------------------

TEST_CASE("TextEvent default-initialised fields", "[types]") {
    tesseract::TextEvent ev{};
    CHECK(ev.event_id.empty());
    CHECK(ev.room_id.empty());
    CHECK(ev.sender.empty());
    CHECK(ev.body.empty());
    CHECK(ev.timestamp == 0u);
    CHECK(ev.type == tesseract::EventType::Text);
}

// ---------------------------------------------------------------------------
// tesseract::ImageEvent
// ---------------------------------------------------------------------------

TEST_CASE("ImageEvent default-initialised fields", "[types]") {
    tesseract::ImageEvent ev{};
    CHECK(ev.event_id.empty());
    CHECK(ev.room_id.empty());
    CHECK(ev.sender.empty());
    CHECK(ev.body.empty());
    CHECK(ev.timestamp == 0u);
    CHECK(ev.type == tesseract::EventType::Image);
    CHECK(ev.image_url.empty());
    CHECK(ev.width == 0u);
    CHECK(ev.height == 0u);
    CHECK(ev.filename.empty());
}

TEST_CASE("ImageEvent fields are settable", "[types]") {
    tesseract::ImageEvent ev{};
    ev.event_id = "evt123";
    ev.room_id = "!room:example.org";
    ev.sender = "@user:example.org";
    ev.body = "look at this";
    ev.timestamp = 1234567890;
    ev.image_url = "mxc://example.org/image";
    ev.width = 1920;
    ev.height = 1080;
    ev.filename = "";  // no MSC2530 filename → body is not a caption

    CHECK(ev.event_id == "evt123");
    CHECK(ev.room_id == "!room:example.org");
    CHECK(ev.sender == "@user:example.org");
    CHECK(ev.body == "look at this");
    CHECK(ev.timestamp == 1234567890);
    CHECK(ev.image_url == "mxc://example.org/image");
    CHECK(ev.width == 1920);
    CHECK(ev.height == 1080);
    CHECK(ev.filename.empty());
}

TEST_CASE("ImageEvent MSC2530 caption via filename field", "[types]") {
    tesseract::ImageEvent ev{};
    ev.image_url = "mxc://example.org/photo.jpg";
    ev.body = "My holiday photo";
    ev.filename = "photo.jpg";  // distinct filename → body is a caption

    CHECK_FALSE(ev.filename.empty());
    CHECK(ev.body == "My holiday photo");
    CHECK(ev.filename == "photo.jpg");
}

// ---------------------------------------------------------------------------
// tesseract::FileEvent
// ---------------------------------------------------------------------------

TEST_CASE("FileEvent default-initialised fields", "[types]") {
    tesseract::FileEvent ev{};
    CHECK(ev.event_id.empty());
    CHECK(ev.room_id.empty());
    CHECK(ev.sender.empty());
    CHECK(ev.body.empty());
    CHECK(ev.timestamp == 0u);
    CHECK(ev.type == tesseract::EventType::File);
    CHECK(ev.file_url.empty());
    CHECK(ev.file_name.empty());
    CHECK(ev.file_size == 0u);
}

TEST_CASE("FileEvent fields are settable", "[types]") {
    tesseract::FileEvent ev{};
    ev.event_id = "evt456";
    ev.room_id = "!room:example.org";
    ev.sender = "@user:example.org";
    ev.body = "sending a file";
    ev.timestamp = 1234567890;
    ev.file_url = "mxc://example.org/file";
    ev.file_name = "document.pdf";
    ev.file_size = 102400;

    CHECK(ev.event_id == "evt456");
    CHECK(ev.room_id == "!room:example.org");
    CHECK(ev.sender == "@user:example.org");
    CHECK(ev.body == "sending a file");
    CHECK(ev.timestamp == 1234567890);
    CHECK(ev.file_url == "mxc://example.org/file");
    CHECK(ev.file_name == "document.pdf");
    CHECK(ev.file_size == 102400);
}

// ---------------------------------------------------------------------------
// tesseract::UnhandledEvent
// ---------------------------------------------------------------------------

TEST_CASE("UnhandledEvent default-initialised fields", "[types]") {
    tesseract::UnhandledEvent ev{};
    CHECK(ev.event_id.empty());
    CHECK(ev.room_id.empty());
    CHECK(ev.sender.empty());
    CHECK(ev.body.empty());
    CHECK(ev.timestamp == 0u);
    CHECK(ev.type == tesseract::EventType::Unhandled);
    CHECK(ev.msg_type.empty());
}

TEST_CASE("UnhandledEvent constructed with msg_type", "[types]") {
    tesseract::UnhandledEvent ev("m.unknown");
    CHECK(ev.type == tesseract::EventType::Unhandled);
    CHECK(ev.msg_type == "m.unknown");
}

// ---------------------------------------------------------------------------
// tesseract::StickerEvent
// ---------------------------------------------------------------------------

TEST_CASE("StickerEvent default-initialised fields", "[types]") {
    tesseract::StickerEvent ev{};
    CHECK(ev.event_id.empty());
    CHECK(ev.room_id.empty());
    CHECK(ev.sender.empty());
    CHECK(ev.body.empty());
    CHECK(ev.timestamp == 0u);
    CHECK(ev.type == tesseract::EventType::Sticker);
    CHECK(ev.image_url.empty());
    CHECK(ev.width == 0u);
    CHECK(ev.height == 0u);
}

TEST_CASE("StickerEvent fields are settable", "[types]") {
    tesseract::StickerEvent ev{};
    ev.event_id = "sticker1";
    ev.room_id = "!room:example.org";
    ev.sender = "@user:example.org";
    ev.body = "wave";
    ev.timestamp = 1234567890;
    ev.image_url = "mxc://example.org/sticker.png";
    ev.width = 512;
    ev.height = 512;

    CHECK(ev.event_id == "sticker1");
    CHECK(ev.room_id == "!room:example.org");
    CHECK(ev.sender == "@user:example.org");
    CHECK(ev.body == "wave");
    CHECK(ev.timestamp == 1234567890);
    CHECK(ev.image_url == "mxc://example.org/sticker.png");
    CHECK(ev.width == 512);
    CHECK(ev.height == 512);
    CHECK(ev.type == tesseract::EventType::Sticker);
}

// ---------------------------------------------------------------------------
// tesseract::EventType enum
// ---------------------------------------------------------------------------

TEST_CASE("EventType enum values are correct", "[types]") {
    CHECK(static_cast<int>(tesseract::EventType::Text)      == 0);
    CHECK(static_cast<int>(tesseract::EventType::Image)     == 1);
    CHECK(static_cast<int>(tesseract::EventType::File)      == 2);
    CHECK(static_cast<int>(tesseract::EventType::Sticker)   == 3);
    CHECK(static_cast<int>(tesseract::EventType::Unhandled) == 4);
}
