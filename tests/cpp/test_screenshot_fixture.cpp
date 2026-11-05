#include <catch2/catch_test_macros.hpp>

#include "app/ScreenshotFixture.h"

#include <algorithm>
#include <filesystem>
#include <string>

using tesseract::screenshot::local_media_path;
using tesseract::screenshot::make_fixture;
using Kind = tesseract::views::MessageRowData::Kind;

namespace
{
// Voice and file URLs are never fetched, so they point at no real file.
bool referenced_ok(const std::string& url)
{
    if (url.empty() || url == "mxc://tesseract.test/voice-clip" ||
        url == "mxc://tesseract.test/release-notes")
    {
        return true;
    }
    const auto path = local_media_path(url);
    return path && std::filesystem::is_regular_file(*path);
}
} // namespace

TEST_CASE("every referenced media URL is a local file or intentionally unfetched", "[screenshot]")
{
    const auto f = make_fixture();

    CHECK(referenced_ok(f.avatar_url));
    for (const auto& r : f.rooms)
    {
        INFO(r.name);
        CHECK(referenced_ok(r.avatar_url));
        CHECK(referenced_ok(r.last_message_thumbnail_url));
        CHECK(referenced_ok(r.last_message_sticker_url));
    }
    auto check_rows = [&](const auto& rows)
    {
        for (const auto& m : rows)
        {
            INFO(m.event_id);
            CHECK(referenced_ok(m.sender_avatar_url));
            CHECK(referenced_ok(m.membership_target_avatar_url));
            if (m.in_reply_to_image_source)
            {
                CHECK(referenced_ok(m.in_reply_to_image_source->fetch_token()));
            }
            for (const auto& rr : m.read_receipts)
            {
                CHECK(referenced_ok(rr.avatar_url));
            }
            if (m.kind == Kind::Image)
            {
                REQUIRE(m.source);
                CHECK(referenced_ok(m.source->fetch_token()));
            }
            if (m.audio_source)
            {
                CHECK(referenced_ok(m.audio_source->fetch_token()));
            }
            if (m.file_source)
            {
                CHECK(referenced_ok(m.file_source->fetch_token()));
            }
        }
    };
    check_rows(f.messages);
    check_rows(f.thread_messages);
    for (const auto& mem : f.members)
    {
        CHECK(referenced_ok(mem.avatar_url));
    }
}

TEST_CASE("local_media_path accepts only mxc URLs holding an absolute path", "[screenshot]")
{
    CHECK_FALSE(local_media_path("mxc://server/abc"));
    CHECK_FALSE(local_media_path(""));
    CHECK_FALSE(local_media_path("mxc://"));
    CHECK_FALSE(local_media_path("https://a/b.jpg"));
    CHECK_FALSE(local_media_path("fixture://x"));
#ifndef _WIN32
    const auto p = local_media_path("mxc:///tmp/x.jpg");
    REQUIRE(p);
    CHECK(*p == std::filesystem::path("/tmp/x.jpg"));
#else
    const auto p = local_media_path("mxc://C:/tmp/x.jpg");
    REQUIRE(p);
    CHECK(*p == std::filesystem::path("C:/tmp/x.jpg"));
#endif
}

TEST_CASE("fixture is internally consistent", "[screenshot]")
{
    const auto f = make_fixture();

    const auto root = std::find_if(f.messages.begin(), f.messages.end(),
                                   [&](const auto& m)
                                   {
                                       return m.event_id == f.thread_root_id;
                                   });
    REQUIRE(root != f.messages.end());
    CHECK(root->is_thread_root);
    REQUIRE_FALSE(f.thread_messages.empty());
    CHECK(f.thread_messages.front().event_id == f.thread_root_id);
    CHECK(root->thread_reply_count == f.thread_messages.size() - 1);

    for (const auto& m : f.messages)
    {
        if (!m.has_reply())
        {
            continue;
        }
        INFO(m.event_id);
        CHECK(std::any_of(f.messages.begin(), f.messages.end(),
                          [&](const auto& o)
                          {
                              return o.event_id == m.in_reply_to_id;
                          }));
    }

    CHECK(std::any_of(f.rooms.begin(), f.rooms.end(),
                      [&](const auto& r)
                      {
                          return r.id == f.selected_room_id;
                      }));
    CHECK(f.rooms.size() == 10);
    CHECK(f.members.size() >= 6);
    CHECK_FALSE(f.typing_names.empty());
}
