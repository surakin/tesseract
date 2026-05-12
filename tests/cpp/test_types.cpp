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

// ---------------------------------------------------------------------------
// tesseract::Reaction
// ---------------------------------------------------------------------------

TEST_CASE("Reaction default-initialised fields", "[types][reactions]") {
    tesseract::Reaction r{};
    CHECK(r.key.empty());
    CHECK(r.count == 0u);
    CHECK_FALSE(r.reacted_by_me);
    CHECK(r.source_json.empty());
    CHECK(r.senders.empty());
}

TEST_CASE("Reaction field assignment round-trips values", "[types][reactions]") {
    tesseract::Reaction r{
        .key           = "👍",
        .count         = 3,
        .reacted_by_me = true,
        .source_json   = "",
        .senders       = {"@alice:example.org", "@bob:example.org", "Carol"},
    };
    CHECK(r.key == "👍");
    CHECK(r.count == 3u);
    CHECK(r.reacted_by_me);
    CHECK(r.senders.size() == 3);
    CHECK(r.senders.back() == "Carol");
    // sender list size must match the count — UI relies on this invariant
    // to populate the tooltip.
    CHECK(r.senders.size() == static_cast<size_t>(r.count));
}

TEST_CASE("MSC 4027 custom reaction preserves source_json", "[types][reactions]") {
    tesseract::Reaction r{
        .key           = ":partyparrot:",
        .count         = 2,
        .reacted_by_me = false,
        .source_json   = R"({"url":"mxc://example.org/abc123"})",
        .senders       = {"@alice:example.org", "@bob:example.org"},
    };
    CHECK_FALSE(r.source_json.empty());
    CHECK(r.source_json.find("mxc://example.org/abc123") != std::string::npos);

    // Copy-assignment must preserve the JSON blob verbatim — chip-icon
    // dedup keys by source_json so any trimming would break caching.
    tesseract::Reaction copy = r;
    CHECK(copy.source_json == r.source_json);
}

TEST_CASE("Event base type carries reactions", "[types][reactions]") {
    tesseract::TextEvent ev;
    ev.event_id = "$abc:example.org";
    ev.body     = "hello";
    ev.reactions = {
        {"👍", 2, true,  "", {"@alice:example.org", "@bob:example.org"}},
        {"❤",  1, false, "", {"@bob:example.org"}},
    };

    CHECK(ev.reactions.size() == 2);
    CHECK(ev.reactions[0].key == "👍");
    CHECK(ev.reactions[0].reacted_by_me);
    CHECK(ev.reactions[1].senders.front() == "@bob:example.org");
}

TEST_CASE("Reactions live on every Event subtype (base field)", "[types][reactions]") {
    // Each subtype should carry its own reaction list since the field is on
    // the base. Sanity-check that subtype-specific fields don't shadow it.
    tesseract::TextEvent   t;   t.reactions.push_back({"👍", 1, true, "", {"@a"}});
    tesseract::ImageEvent  img; img.reactions.push_back({"❤", 2, false, "", {"@a", "@b"}});
    tesseract::StickerEvent st; st.reactions.push_back({"😂", 3, false, "", {"@a","@b","@c"}});
    tesseract::FileEvent   f;   f.reactions.push_back({"🔥", 4, true, "", {"@a","@b","@c","@d"}});
    tesseract::UnhandledEvent u;u.reactions.push_back({"👀", 0, false, "", {}});

    CHECK(t.reactions.size()   == 1);
    CHECK(img.reactions.size() == 1);
    CHECK(st.reactions.size()  == 1);
    CHECK(f.reactions.size()   == 1);
    CHECK(u.reactions.size()   == 1);
    CHECK(t.reactions[0].reacted_by_me);
    CHECK(f.reactions[0].senders.size() == 4);
}

// ---------------------------------------------------------------------------
// tesseract::EventType enum
// ---------------------------------------------------------------------------

TEST_CASE("EventType enum values are correct", "[types]") {
    CHECK(static_cast<int>(tesseract::EventType::Text)      == 0);
    CHECK(static_cast<int>(tesseract::EventType::Image)     == 1);
    CHECK(static_cast<int>(tesseract::EventType::File)      == 2);
    CHECK(static_cast<int>(tesseract::EventType::Sticker)   == 3);
    CHECK(static_cast<int>(tesseract::EventType::Redacted)  == 4);
    CHECK(static_cast<int>(tesseract::EventType::Unhandled) == 5);
}

// ---------------------------------------------------------------------------
// tesseract::RedactedEvent
// ---------------------------------------------------------------------------

TEST_CASE("RedactedEvent default-initialised fields", "[types]") {
    tesseract::RedactedEvent ev{};
    CHECK(ev.event_id.empty());
    CHECK(ev.room_id.empty());
    CHECK(ev.sender.empty());
    CHECK(ev.body.empty());
    CHECK(ev.timestamp == 0u);
    CHECK(ev.type == tesseract::EventType::Redacted);
    CHECK(ev.reactions.empty());
}

TEST_CASE("RedactedEvent carries sender identity", "[types]") {
    // The UI surfaces the redacted-event sender so users can tell which
    // peer's message was deleted; verify the base Event fields round-trip.
    tesseract::RedactedEvent ev{};
    ev.event_id = "$redacted-evt";
    ev.room_id  = "!room:example.org";
    ev.sender   = "@alice:example.org";
    ev.sender_name = "Alice";
    ev.timestamp   = 1700000000000ull;

    CHECK(ev.event_id    == "$redacted-evt");
    CHECK(ev.sender      == "@alice:example.org");
    CHECK(ev.sender_name == "Alice");
    CHECK(ev.timestamp   == 1700000000000ull);
    CHECK(ev.type        == tesseract::EventType::Redacted);
}

// ---------------------------------------------------------------------------
// tesseract::BackupState / BackupProgress  (Step 6)
// ---------------------------------------------------------------------------
//
// The wire-level u8 codes must stay in sync with `BACKUP_STATE_*` in
// sdk/src/client.rs and the conversion in client/src/ffi_convert.h.

TEST_CASE("BackupState enum values match the FFI wire encoding", "[types][recovery]") {
    CHECK(static_cast<uint8_t>(tesseract::BackupState::Unknown)     == 0);
    CHECK(static_cast<uint8_t>(tesseract::BackupState::Disabled)    == 1);
    CHECK(static_cast<uint8_t>(tesseract::BackupState::Enabled)     == 2);
    CHECK(static_cast<uint8_t>(tesseract::BackupState::Downloading) == 3);
    CHECK(static_cast<uint8_t>(tesseract::BackupState::Creating)    == 4);
}

TEST_CASE("BackupProgress default-initialised fields", "[types][recovery]") {
    tesseract::BackupProgress p{};
    CHECK(p.state         == tesseract::BackupState::Unknown);
    CHECK(p.imported_keys == 0u);
    CHECK(p.total_keys    == 0u);
}

TEST_CASE("BackupProgress fields are settable", "[types][recovery]") {
    tesseract::BackupProgress p{
        .state         = tesseract::BackupState::Downloading,
        .imported_keys = 234,
        .total_keys    = 1200,
    };
    CHECK(p.state         == tesseract::BackupState::Downloading);
    CHECK(p.imported_keys == 234u);
    CHECK(p.total_keys    == 1200u);
}

// ---------------------------------------------------------------------------
// tesseract::Client recovery surface (Step 6)
// ---------------------------------------------------------------------------

TEST_CASE("needs_recovery is false when not logged in", "[client][recovery]") {
    tesseract::Client c;
    CHECK_FALSE(c.needs_recovery());
}

TEST_CASE("recover fails with 'not logged in' before login", "[client][recovery]") {
    tesseract::Client c;
    auto r = c.recover("some-key");
    CHECK_FALSE(static_cast<bool>(r));
    CHECK(r.message == "not logged in");
}

TEST_CASE("recover rejects empty key before login", "[client][recovery]") {
    tesseract::Client c;
    // Empty input is rejected at the FFI boundary; "not logged in" wins first
    // because we never reach the empty-string check without a Client.
    auto r = c.recover("");
    CHECK_FALSE(static_cast<bool>(r));
}

TEST_CASE("backup_state starts in Unknown with zero counters", "[client][recovery]") {
    tesseract::Client c;
    auto p = c.backup_state();
    CHECK(p.state         == tesseract::BackupState::Unknown);
    CHECK(p.imported_keys == 0u);
    CHECK(p.total_keys    == 0u);
}

// ---------------------------------------------------------------------------
// tesseract::Client identity getters (sidebar user strip)
// ---------------------------------------------------------------------------

TEST_CASE("get_user_id is empty when not logged in", "[client][identity]") {
    tesseract::Client c;
    CHECK(c.get_user_id().empty());
}

TEST_CASE("get_display_name is empty when not logged in", "[client][identity]") {
    tesseract::Client c;
    CHECK(c.get_display_name().empty());
}

TEST_CASE("get_avatar_url is empty when not logged in", "[client][identity]") {
    tesseract::Client c;
    CHECK(c.get_avatar_url().empty());
}
