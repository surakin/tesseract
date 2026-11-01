#include <catch2/catch_test_macros.hpp>

#include "app/ScreenshotFixture.h"
#include "tk/cache_key.h"
#include "tk/pixmap_cache.h"
#include "tk_test_surface.h"

#include <algorithm>
#include <set>
#include <string>

using tesseract::screenshot::installed_asset_keys;
using tesseract::screenshot::make_fixture;
using Kind = tesseract::views::MessageRowData::Kind;

namespace
{
bool installed(const std::set<std::string>& keys, const std::string& k)
{
    return k.empty() || keys.contains(k);
}
} // namespace

TEST_CASE("every referenced media key is installed", "[screenshot]")
{
    const auto f = make_fixture();
    const auto list = installed_asset_keys();
    const std::set<std::string> keys(list.begin(), list.end());

    CHECK(installed(keys, f.avatar_url));
    for (const auto& r : f.rooms)
    {
        INFO(r.name);
        CHECK(installed(keys, r.avatar_url));
        CHECK(installed(keys, r.last_message_thumbnail_url));
        CHECK(installed(keys, r.last_message_sticker_url));
    }
    auto check_rows = [&](const auto& rows)
    {
        for (const auto& m : rows)
        {
            INFO(m.event_id);
            CHECK(installed(keys, m.sender_avatar_url));
            CHECK(installed(keys, m.membership_target_avatar_url));
            if (m.in_reply_to_image_source)
            {
                CHECK(installed(keys, m.in_reply_to_image_source->fetch_token()));
            }
            for (const auto& rr : m.read_receipts)
            {
                CHECK(installed(keys, rr.avatar_url));
            }
            if (m.kind == Kind::Image)
            {
                REQUIRE(m.source);
                CHECK(installed(keys, m.source->fetch_token()));
            }
        }
    };
    check_rows(f.messages);
    check_rows(f.thread_messages);
    for (const auto& mem : f.members)
    {
        CHECK(installed(keys, mem.avatar_url));
    }
}

TEST_CASE("install_assets decodes every asset", "[screenshot]")
{
    auto surface = TestSurface::create(16, 16);
    tk::PixmapCache cache;
    REQUIRE(tesseract::screenshot::install_assets(surface->factory(), cache));
    for (const auto& k : installed_asset_keys())
    {
        INFO(k);
        CHECK(cache.peek(tk::CacheKey::media(k)) != nullptr);
    }
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
