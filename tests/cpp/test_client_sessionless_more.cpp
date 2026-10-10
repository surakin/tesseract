// More of the no-session wrapper surface (see test_client_sessionless.cpp):
// media sends, device / activity / backoff queries, directory search, export
// start and RTC frame pushes. Each must fail soft: a failed Result or an empty
// value, never a crash. Nothing here touches the network.

#include <catch2/catch_test_macros.hpp>

#include <tesseract/client.h>

#include <cstdint>
#include <string>
#include <vector>

using tesseract::Client;

namespace
{
const std::string kCmRoom = "!room:example.invalid";
const std::vector<std::uint8_t> kCmBytes{1, 2, 3, 4};
} // namespace

TEST_CASE("sessionless Client: blocking media sends fail", "[client][sessionless]")
{
    Client c;
    CHECK_FALSE(c.send_image(kCmRoom, kCmBytes, "image/png", "a.png", "cap", 10, 10,
                             false).ok);
    CHECK_FALSE(c.send_image(kCmRoom, kCmBytes, "image/gif", "a.gif", "", 10, 10,
                             true, "$reply", "$thread").ok);
    CHECK_FALSE(c.send_video(kCmRoom, kCmBytes, "video/mp4", "v.mp4", "cap", 10, 10,
                             kCmBytes, 5, 5, 1000).ok);
    CHECK_FALSE(c.send_audio(kCmRoom, kCmBytes, "audio/ogg", "a.ogg", "", 1000).ok);
    CHECK_FALSE(c.send_file(kCmRoom, kCmBytes, "text/plain", "a.txt", "").ok);
    const std::uint8_t pcm[8] = {0};
    CHECK_FALSE(c.send_voice(kCmRoom, pcm, sizeof pcm, 500, {1, 2, 3}, "", "").ok);
    CHECK_FALSE(c.send_gif_video(kCmRoom, kCmBytes, "video/mp4", "gif", 10, 10, 500,
                                 kCmBytes, "image/png", 5, 5, "", "").ok);
}

TEST_CASE("sessionless Client: async sends and exports are harmless",
          "[client][sessionless]")
{
    Client c;
    const std::uint8_t pcm[8] = {0};
    CHECK_NOTHROW(c.send_voice_async(1, kCmRoom, pcm, sizeof pcm, 500, {1}, "", ""));
    CHECK_NOTHROW(c.send_image_async(2, kCmRoom, kCmBytes, "image/png", "a.png", "",
                                     10, 10, false));
    CHECK_NOTHROW(c.send_file_async(3, kCmRoom, kCmBytes, "text/plain", "a.txt", ""));
    CHECK_NOTHROW(c.search_room_directory(4, "filter", "example.invalid"));
    CHECK_NOTHROW(c.start_room_export_async(5, kCmRoom, tesseract::RoomExportOptions{}));
    CHECK_NOTHROW(c.get_space_child_summary_async(6, "!space:x", kCmRoom));
}

TEST_CASE("sessionless Client: device, activity and backoff queries are empty",
          "[client][sessionless]")
{
    Client c;
    CHECK(c.list_devices().empty());
    CHECK(c.activity_snapshot().empty());
    CHECK(c.load_media_backoff().empty());
    CHECK(c.load_room_summary_backoff().empty());
    CHECK(c.room_media_count(kCmRoom) == 0);
    CHECK_FALSE(c.have_cross_signing_keys());
    CHECK_FALSE(c.get_cached_room_summary(kCmRoom).has_value());
    CHECK_FALSE(c.get_space_child_summary("!space:x", kCmRoom).has_value());
    CHECK_FALSE(c.backup_disabled_by_user().has_value());
    CHECK_NOTHROW(c.note_media_backoff_failed("mxc://x/y", 2, 123));
    CHECK_NOTHROW(c.note_room_summary_backoff_failed(kCmRoom, 1, 456));
}

TEST_CASE("sessionless Client: pusher registration fails", "[client][sessionless]")
{
    Client c;
    CHECK_FALSE(c.register_pusher("key", "app", "App", "Device",
                                  "https://push.example.invalid", "en").ok);
}

TEST_CASE("sessionless Client: RTC frame pushes with no call are no-ops",
          "[client][sessionless]")
{
    Client c;
    std::vector<std::uint8_t> y(16, 0), u(4, 0), v(4, 0);
    CHECK_NOTHROW(c.rtc_push_video_frame_i420(y.data(), u.data(), v.data(), 4, 4, 4, 2, 2));
    CHECK_NOTHROW(c.rtc_push_screen_frame_i420(y.data(), u.data(), v.data(), 4, 4, 4, 2, 2));
    CHECK_NOTHROW(c.rtc_stop_screen_share());
}
