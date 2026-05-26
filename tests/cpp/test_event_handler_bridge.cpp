// Table-driven tests for ffi_convert.h::make_event and ::assign_base.
//
// Why this matters: every async timeline callback from Rust passes through
// make_event() to allocate the right C++ Event subtype, and through
// assign_base() to copy shared fields. A missing branch silently falls back
// to UnhandledEvent (data loss for that row); a missing assign_base field
// silently drops the new value. Neither failure mode is loud — they just
// show up as "weird timeline behaviour" in the UI weeks later.
//
// These tests construct an FFI TimelineEvent directly (cxx-bridged shared
// struct, public fields), feed it through make_event, and assert the right
// subtype + every key base field round-trips.

#include <catch2/catch_test_macros.hpp>

#include "ffi_convert.h"
#include "tesseract/types.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"

#include <string>

namespace
{

// Build a TimelineEvent with every base field populated to a distinct value
// so we can assert assign_base() copies each one.
tesseract_ffi::TimelineEvent make_ffi_event(const std::string& msg_type)
{
    tesseract_ffi::TimelineEvent ev{};
    ev.msg_type             = msg_type;
    ev.event_id             = "$evt:server";
    ev.room_id              = "!room:server";
    ev.sender               = "@alice:server";
    ev.sender_name          = "Alice";
    ev.sender_avatar_url    = "mxc://server/avatar";
    ev.body                 = "body text";
    ev.formatted_body       = "<p>body text</p>";
    ev.timestamp            = 1700000000000ULL;
    ev.in_reply_to_id       = "$reply_to";
    ev.in_reply_to_sender_name = "Bob";
    ev.in_reply_to_body     = "earlier message";
    ev.in_reply_to_image_url = "mxc://server/img";
    ev.in_reply_to_image_encrypted_json = R"({"enc":"json"})";
    ev.is_edited            = true;
    ev.thread_root_id       = "$root";
    ev.is_thread_root       = true;
    ev.thread_reply_count   = 7;
    ev.thread_latest_sender_name = "Carol";
    ev.thread_latest_body   = "latest reply";
    ev.thread_latest_ts     = 1700000100000ULL;
    ev.pending_state        = "sending";
    ev.pending_error        = "oops";
    ev.pending_recoverable  = true;
    ev.pending_txn_id       = "txn-abc";
    return ev;
}

} // namespace

TEST_CASE("make_event dispatches m.text to TextEvent", "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("m.text"));
    REQUIRE(ev);
    CHECK(ev->type == tesseract::EventType::Text);
    CHECK(dynamic_cast<tesseract::TextEvent*>(ev.get()) != nullptr);
}

TEST_CASE("make_event dispatches m.notice to NoticeEvent", "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("m.notice"));
    CHECK(ev->type == tesseract::EventType::Notice);
    CHECK(dynamic_cast<tesseract::NoticeEvent*>(ev.get()) != nullptr);
}

TEST_CASE("make_event dispatches m.emote to EmoteEvent", "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("m.emote"));
    CHECK(ev->type == tesseract::EventType::Emote);
    CHECK(dynamic_cast<tesseract::EmoteEvent*>(ev.get()) != nullptr);
}

TEST_CASE("make_event dispatches m.image to ImageEvent with media fields",
          "[ffi][make_event]")
{
    auto ffi = make_ffi_event("m.image");
    ffi.source_url      = "mxc://server/image";
    ffi.image_thumbnail_url = "mxc://server/thumb";
    ffi.width           = 800;
    ffi.height          = 600;
    ffi.image_filename  = "photo.png";
    ffi.blurhash        = "L6PZfSi_.AyE_3t7t7R**0o#DgR4";
    ffi.image_animated  = true;
    auto ev = tesseract::make_event(ffi);
    auto* img = dynamic_cast<tesseract::ImageEvent*>(ev.get());
    REQUIRE(img != nullptr);
    CHECK(img->width == 800);
    CHECK(img->height == 600);
    CHECK(img->filename == "photo.png");
    CHECK(img->blurhash == "L6PZfSi_.AyE_3t7t7R**0o#DgR4");
    CHECK(img->animated);
    REQUIRE(img->source);
    CHECK(img->source->is_encrypted() == false);
    REQUIRE(img->thumbnail);
}

TEST_CASE("make_event dispatches m.sticker to StickerEvent", "[ffi][make_event]")
{
    auto ffi = make_ffi_event("m.sticker");
    ffi.source_url        = "mxc://server/sticker";
    ffi.width             = 256;
    ffi.height            = 256;
    ffi.sticker_info_json = R"({"w":256,"h":256})";
    auto ev = tesseract::make_event(ffi);
    auto* st = dynamic_cast<tesseract::StickerEvent*>(ev.get());
    REQUIRE(st != nullptr);
    CHECK(st->width == 256);
    CHECK(st->height == 256);
    CHECK(st->info_json == R"({"w":256,"h":256})");
}

TEST_CASE("make_event dispatches m.redacted to RedactedEvent",
          "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("m.redacted"));
    CHECK(ev->type == tesseract::EventType::Redacted);
    CHECK(dynamic_cast<tesseract::RedactedEvent*>(ev.get()) != nullptr);
}

TEST_CASE("make_event dispatches m.utd to UtdEvent", "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("m.utd"));
    CHECK(ev->type == tesseract::EventType::Utd);
    CHECK(dynamic_cast<tesseract::UtdEvent*>(ev.get()) != nullptr);
}

TEST_CASE("make_event dispatches m.file to FileEvent with metadata",
          "[ffi][make_event]")
{
    auto ffi = make_ffi_event("m.file");
    ffi.file_url  = "mxc://server/doc";
    ffi.file_name = "report.pdf";
    ffi.file_size = 12345;
    auto ev = tesseract::make_event(ffi);
    auto* f = dynamic_cast<tesseract::FileEvent*>(ev.get());
    REQUIRE(f != nullptr);
    CHECK(f->file_name == "report.pdf");
    CHECK(f->file_size == 12345);
    REQUIRE(f->source);
}

TEST_CASE("make_event dispatches m.audio to AudioEvent with metadata",
          "[ffi][make_event]")
{
    auto ffi = make_ffi_event("m.audio");
    ffi.audio_url         = "mxc://server/clip";
    ffi.audio_mime        = "audio/mpeg";
    ffi.audio_duration_ms = 90000;
    ffi.file_name         = "song.mp3";
    ffi.file_size         = 500000;
    auto ev = tesseract::make_event(ffi);
    auto* a = dynamic_cast<tesseract::AudioEvent*>(ev.get());
    REQUIRE(a != nullptr);
    CHECK(a->mime_type == "audio/mpeg");
    CHECK(a->duration_ms == 90000);
    CHECK(a->filename == "song.mp3");
    CHECK(a->file_size == 500000);
}

TEST_CASE("make_event dispatches m.voice to VoiceEvent with waveform",
          "[ffi][make_event]")
{
    auto ffi = make_ffi_event("m.voice");
    ffi.audio_url         = "mxc://server/voice";
    ffi.audio_mime        = "audio/ogg";
    ffi.audio_duration_ms = 5000;
    ffi.audio_waveform    = rust::Vec<std::uint16_t>{};
    ffi.audio_waveform.push_back(10);
    ffi.audio_waveform.push_back(20);
    ffi.audio_waveform.push_back(30);
    auto ev = tesseract::make_event(ffi);
    auto* v = dynamic_cast<tesseract::VoiceEvent*>(ev.get());
    REQUIRE(v != nullptr);
    CHECK(v->mime_type == "audio/ogg");
    CHECK(v->duration_ms == 5000);
    REQUIRE(v->waveform.size() == 3);
    CHECK(v->waveform[0] == 10);
    CHECK(v->waveform[2] == 30);
}

TEST_CASE("make_event dispatches m.video to VideoEvent with vendor hints",
          "[ffi][make_event]")
{
    auto ffi = make_ffi_event("m.video");
    ffi.source_url           = "mxc://server/movie";
    ffi.video_thumbnail_url  = "mxc://server/mthumb";
    ffi.video_mime           = "video/mp4";
    ffi.width                = 1280;
    ffi.height               = 720;
    ffi.video_duration_ms    = 60000;
    ffi.image_filename       = "clip.mp4";
    ffi.video_autoplay       = true;
    ffi.video_loop           = true;
    ffi.video_no_audio       = true;
    ffi.video_hide_controls  = true;
    ffi.video_gif            = true;
    ffi.blurhash             = "L9AB:rb_~Wxu#k%MR*j[";
    auto ev = tesseract::make_event(ffi);
    auto* v = dynamic_cast<tesseract::VideoEvent*>(ev.get());
    REQUIRE(v != nullptr);
    CHECK(v->mime_type == "video/mp4");
    CHECK(v->width == 1280);
    CHECK(v->height == 720);
    CHECK(v->duration_ms == 60000);
    CHECK(v->filename == "clip.mp4");
    CHECK(v->autoplay);
    CHECK(v->loop);
    CHECK(v->no_audio);
    CHECK(v->hide_controls);
    CHECK(v->gif);
    CHECK(v->blurhash == "L9AB:rb_~Wxu#k%MR*j[");
}

TEST_CASE("make_event dispatches m.location to LocationEvent",
          "[ffi][make_event]")
{
    auto ffi = make_ffi_event("m.location");
    ffi.location_lat         = 40.7128;
    ffi.location_lon         = -74.0060;
    ffi.location_description = "New York";
    auto ev = tesseract::make_event(ffi);
    auto* loc = dynamic_cast<tesseract::LocationEvent*>(ev.get());
    REQUIRE(loc != nullptr);
    CHECK(loc->lat == 40.7128);
    CHECK(loc->lon == -74.0060);
    CHECK(loc->description == "New York");
}

TEST_CASE("make_event dispatches virtual.date_divider to DaySeparatorEvent",
          "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("virtual.date_divider"));
    CHECK(ev->type == tesseract::EventType::DaySeparator);
    CHECK(dynamic_cast<tesseract::DaySeparatorEvent*>(ev.get()) != nullptr);
}

TEST_CASE("make_event dispatches virtual.read_marker to ReadMarkerEvent",
          "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("virtual.read_marker"));
    CHECK(ev->type == tesseract::EventType::ReadMarker);
    CHECK(dynamic_cast<tesseract::ReadMarkerEvent*>(ev.get()) != nullptr);
}

TEST_CASE("make_event dispatches virtual.timeline_start to TimelineStartEvent",
          "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("virtual.timeline_start"));
    CHECK(ev->type == tesseract::EventType::TimelineStart);
    CHECK(dynamic_cast<tesseract::TimelineStartEvent*>(ev.get()) != nullptr);
}

TEST_CASE("make_event falls back to UnhandledEvent for unknown msg_type",
          "[ffi][make_event]")
{
    auto ev = tesseract::make_event(make_ffi_event("m.future_type_we_dont_know"));
    auto* u = dynamic_cast<tesseract::UnhandledEvent*>(ev.get());
    REQUIRE(u != nullptr);
    CHECK(u->msg_type == "m.future_type_we_dont_know");
}

// ---------------------------------------------------------------------------
// assign_base — every shared field must propagate. This is the regression
// guard the threads-UI feature would have wanted: thread_root_id,
// is_thread_root, thread_reply_count, thread_latest_* all need to round-trip.
// ---------------------------------------------------------------------------

TEST_CASE("assign_base copies every shared base field", "[ffi][assign_base]")
{
    auto ev = tesseract::make_event(make_ffi_event("m.text"));
    REQUIRE(ev);

    CHECK(ev->event_id == "$evt:server");
    CHECK(ev->room_id == "!room:server");
    CHECK(ev->sender == "@alice:server");
    CHECK(ev->sender_name == "Alice");
    CHECK(ev->sender_avatar_url == "mxc://server/avatar");
    CHECK(ev->body == "body text");
    CHECK(ev->formatted_body == "<p>body text</p>");
    CHECK(ev->timestamp == 1700000000000ULL);

    CHECK(ev->in_reply_to_id == "$reply_to");
    CHECK(ev->in_reply_to_sender_name == "Bob");
    CHECK(ev->in_reply_to_body == "earlier message");
    CHECK(ev->in_reply_to_image_url == "mxc://server/img");
    CHECK(ev->in_reply_to_image_encrypted_json == R"({"enc":"json"})");
    CHECK(ev->is_edited);

    // Thread fields — the regression guard that would have caught any
    // future drop of these when assign_base evolves.
    CHECK(ev->thread_root_id == "$root");
    CHECK(ev->is_thread_root);
    CHECK(ev->thread_reply_count == 7);
    CHECK(ev->thread_latest_sender_name == "Carol");
    CHECK(ev->thread_latest_body == "latest reply");
    CHECK(ev->thread_latest_ts == 1700000100000ULL);

    CHECK(ev->pending_state == "sending");
    CHECK(ev->pending_error == "oops");
    CHECK(ev->pending_recoverable);
    CHECK(ev->pending_txn_id == "txn-abc");
}

TEST_CASE("assign_base populates reactions", "[ffi][assign_base]")
{
    auto ffi = make_ffi_event("m.text");
    tesseract_ffi::ReactionGroup r1{};
    r1.key = "👍";
    r1.count = 2;
    r1.reacted_by_me = true;
    r1.source_url = "";
    r1.senders.push_back(rust::String("@alice:server"));
    r1.senders.push_back(rust::String("@bob:server"));
    ffi.reactions.push_back(std::move(r1));

    tesseract_ffi::ReactionGroup r2{};
    r2.key = ":custom:";
    r2.count = 1;
    r2.reacted_by_me = false;
    r2.source_url = "mxc://server/emoji";
    r2.senders.push_back(rust::String("@carol:server"));
    ffi.reactions.push_back(std::move(r2));

    auto ev = tesseract::make_event(ffi);
    REQUIRE(ev->reactions.size() == 2);

    CHECK(ev->reactions[0].key == "👍");
    CHECK(ev->reactions[0].count == 2);
    CHECK(ev->reactions[0].reacted_by_me);
    CHECK_FALSE(ev->reactions[0].source);
    REQUIRE(ev->reactions[0].senders.size() == 2);
    CHECK(ev->reactions[0].senders[0] == "@alice:server");

    CHECK(ev->reactions[1].key == ":custom:");
    CHECK(ev->reactions[1].count == 1);
    CHECK_FALSE(ev->reactions[1].reacted_by_me);
    REQUIRE(ev->reactions[1].source);
}

TEST_CASE("assign_base populates read receipts", "[ffi][assign_base]")
{
    auto ffi = make_ffi_event("m.text");
    tesseract_ffi::ReadReceipt rr{};
    rr.user_id = "@reader:server";
    rr.display_name = "Reader";
    rr.avatar_url = "mxc://server/r";
    ffi.read_receipts.push_back(std::move(rr));

    auto ev = tesseract::make_event(ffi);
    REQUIRE(ev->read_receipts.size() == 1);
    CHECK(ev->read_receipts[0].user_id == "@reader:server");
    CHECK(ev->read_receipts[0].display_name == "Reader");
    CHECK(ev->read_receipts[0].avatar_url == "mxc://server/r");
}
