#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "settings_guard.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MessageListView.h"
#include "views/format.h"

#include <tesseract/settings.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using tesseract::Settings;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using Anchor = Settings::TimelineColumnAnchor;

namespace
{

struct ColumnStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(1600, 600);
    tesseract::test::SettingsGuard guard;

    ColumnStage()
    {
        auto& s = Settings::instance();
        s.message_layout = Settings::MessageLayout::Classic;
        s.timeline_column_pct = 50;
        s.timeline_column_anchor = Anchor::Left;
    }
    void run(tk::Widget& root, float w) const
    {
        const tk::Rect bounds{0, 0, w, 600};
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light()};
        root.paint(pc);
    }
};

MessageRowData long_text(const std::string& id, bool own = false)
{
    std::string body;
    for (int i = 0; i < 60; ++i)
    {
        body += "lorem ipsum dolor sit amet ";
    }
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.sender = own ? "@me:x" : "@alice:x";
    r.sender_name = own ? "Me" : "Alice";
    r.is_own = own;
    r.body = body;
    return r;
}

MessageRowData membership(const std::string& id, const std::string& target)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Membership;
    r.event_id = id;
    r.sender = target;
    r.membership_action = tesseract::MembershipAction::Joined;
    r.membership_target_user_id = target;
    return r;
}

MessageRowData day_separator()
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::DaySeparator;
    return r;
}

} // namespace

TEST_CASE("message list column: off by default, rows measure at the full width",
          "[message_list][column]")
{
    ColumnStage st;
    MessageListView plain;
    plain.set_messages({long_text("$a")}, false);
    st.run(plain, 1600);

    MessageListView full;
    full.set_content_column_enabled(true);
    Settings::instance().timeline_column_pct = 100;
    full.set_messages({long_text("$a")}, false);
    st.run(full, 1600);

    CHECK(plain.content_height() == Catch::Approx(full.content_height()));
}

TEST_CASE("message list column: an enabled column measures at the column width",
          "[message_list][column]")
{
    ColumnStage st;
    MessageListView narrow; // reference: a plain list as wide as the column
    narrow.set_messages({long_text("$a")}, false);
    st.run(narrow, 800);

    MessageListView col;
    col.set_content_column_enabled(true);
    col.set_messages({long_text("$a")}, false);
    st.run(col, 1600);

    CHECK(col.content_height() == Catch::Approx(narrow.content_height()));
}

TEST_CASE("message list column: IRC layout ignores the column", "[message_list][column]")
{
    ColumnStage st;
    Settings::instance().message_layout = Settings::MessageLayout::Irc;
    MessageListView plain;
    plain.set_messages({long_text("$a")}, false);
    st.run(plain, 1600);

    MessageListView col;
    col.set_content_column_enabled(true);
    col.set_messages({long_text("$a")}, false);
    st.run(col, 1600);

    CHECK(col.content_height() == Catch::Approx(plain.content_height()));
}

TEST_CASE("message list column: left anchor keeps the body origin, centre shifts it",
          "[message_list][column]")
{
    ColumnStage st;
    MessageListView narrow;
    narrow.set_messages({long_text("$a")}, false);
    st.run(narrow, 800);
    const float base_x = narrow.body_origin_x_for_test("$a");
    REQUIRE(base_x > 0.0f);

    MessageListView left;
    left.set_content_column_enabled(true);
    left.set_messages({long_text("$a")}, false);
    st.run(left, 1600);
    CHECK(left.body_origin_x_for_test("$a") == Catch::Approx(base_x));

    Settings::instance().timeline_column_anchor = Anchor::Center;
    MessageListView centred;
    centred.set_content_column_enabled(true);
    centred.set_messages({long_text("$a")}, false);
    st.run(centred, 1600);
    CHECK(centred.body_origin_x_for_test("$a") == Catch::Approx(base_x + 400.0f));
}

TEST_CASE("message list column: Bubbles own messages follow the column's right edge",
          "[message_list][column]")
{
    ColumnStage st;
    Settings::instance().message_layout = Settings::MessageLayout::Bubbles;
    MessageListView narrow;
    narrow.set_messages({long_text("$a", /*own=*/true)}, false);
    st.run(narrow, 800);
    const float base_x = narrow.body_origin_x_for_test("$a");
    REQUIRE(base_x > 0.0f);

    MessageListView col;
    col.set_content_column_enabled(true);
    col.set_messages({long_text("$a", /*own=*/true)}, false);
    st.run(col, 1600);
    CHECK(col.body_origin_x_for_test("$a") == Catch::Approx(base_x));
}

TEST_CASE("message list column: a membership avatar click lands in the centred column",
          "[message_list][column]")
{
    ColumnStage st;
    Settings::instance().timeline_column_anchor = Anchor::Center;
    MessageListView v;
    v.set_content_column_enabled(true);
    v.set_messages({membership("$m", "@bob:x")}, false);
    std::string clicked;
    v.on_sender_clicked = [&](const std::string& uid, const std::string&, const std::string&)
    {
        clicked = uid;
    };
    st.run(v, 1600);

    const tk::Rect row = v.row_world_rect(0);
    const float cy = row.y + 14.0f; // middle of the one-line membership row
    // Avatar sits 12px + radius into the column, which starts at x = 400.
    const tk::Point on_avatar{400.0f + 12.0f + 9.0f, cy};
    const tk::Point old_spot{12.0f + 9.0f, cy};

    v.on_pointer_down(old_spot); // where the avatar sits without the column
    v.on_pointer_up(old_spot, true);
    CHECK(clicked.empty());

    REQUIRE(v.on_pointer_down(on_avatar));
    v.on_pointer_up(on_avatar, true);
    CHECK(clicked == "@bob:x");
}

TEST_CASE("centered system messages keep clickable avatars and expanded timestamps",
          "[message_list][membership][system_messages][column]")
{
    ColumnStage st;
    auto& settings = Settings::instance();
    settings.message_layout = Settings::MessageLayout::Bubbles;
    settings.system_message_timeline_alignment = Settings::SystemMessageTimelineAlignment::Center;
    settings.timeline_column_anchor = Anchor::Center;

    const std::uint8_t red[] = {255, 0, 0, 255};
    auto avatar = st.surface->factory().create_image_rgba(red, 1, 1);
    REQUIRE(avatar != nullptr);

    auto alice = membership("$alice", "@alice:x");
    alice.membership_target_name = "Alice";
    alice.membership_target_avatar_url = "mxc://x/alice";
    alice.timestamp_ms = 1700000000000;
    auto bob = membership("$bob", "@bob:x");
    bob.membership_target_name = "Bob";
    bob.membership_target_avatar_url = "mxc://x/bob";
    bob.timestamp_ms = 1700000060000;

    MessageListView v;
    v.set_content_column_enabled(true);
    v.set_avatar_provider(
        [&](const std::string&)
        {
            return avatar.get();
        });
    v.set_messages({alice, bob}, false);
    std::string clicked;
    v.on_sender_clicked = [&](const std::string& uid, const std::string&, const std::string&)
    {
        clicked = uid;
    };

    auto avatar_x = [&](tk::Rect row)
    {
        const int cy = static_cast<int>(row.y + 12.0f);
        for (int x = static_cast<int>(row.x); x < static_cast<int>(row.x + row.w); ++x)
        {
            const auto c = st.surface->read_pixel(x, cy);
            if (c.r > 240 && c.g < 15 && c.b < 15)
            {
                return static_cast<float>(x) + 9.0f;
            }
        }
        return -1.0f;
    };

    st.run(v, 1600);
    const tk::Rect collapsed = v.row_world_rect(0);
    const float collapsed_avatar = avatar_x(collapsed);
    REQUIRE(collapsed_avatar > 400.0f);
    REQUIRE(collapsed_avatar < 800.0f);
    const tk::Point summary_click{collapsed_avatar, collapsed.y + 12.0f};
    REQUIRE(v.on_pointer_down(summary_click));
    v.on_pointer_up(summary_click, true);
    CHECK(clicked.empty());

    st.surface = TestSurface::create(1600, 600);
    avatar = st.surface->factory().create_image_rgba(red, 1, 1);
    REQUIRE(avatar != nullptr);
    st.run(v, 1600);
    const tk::Rect first = v.row_world_rect(0);
    const tk::Rect second = v.row_world_rect(1);
    REQUIRE(second.h > 0.0f);
    const float first_avatar = avatar_x(first);
    const float second_avatar = avatar_x(second);
    REQUIRE(first_avatar > 400.0f);
    REQUIRE(second_avatar > 400.0f);
    const tk::Point avatar_click{second_avatar, second.y + 12.0f};
    REQUIRE(v.on_pointer_down(avatar_click));
    v.on_pointer_up(avatar_click, true);
    CHECK(clicked == "@bob:x");

    tk::TextStyle text_style{};
    text_style.role = tk::FontRole::Small;
    text_style.wrap = false;
    text_style.trim = tk::TextTrim::Ellipsis;
    tk::TextStyle time_style = text_style;
    time_style.role = tk::FontRole::Timestamp;
    time_style.max_width = 800.0f - 24.0f - 18.0f - 6.0f;
    const auto time = st.surface->factory().build_text(
        tesseract::views::format_hhmm(alice.timestamp_ms), time_style);
    REQUIRE(time != nullptr);
    const float time_width = time->measure().w;
    text_style.max_width = time_style.max_width - time_width - 6.0f;
    const auto phrase = st.surface->factory().build_text("Alice joined the room", text_style);
    REQUIRE(phrase != nullptr);
    const float phrase_width = phrase->measure().w;
    const float group_width = 18.0f + 6.0f + phrase_width + 6.0f + time_width;
    const float expected_avatar = 800.0f - group_width * 0.5f + 9.0f;
    CHECK(first_avatar == Catch::Approx(expected_avatar).margin(2.0f));

    const float time_x = 800.0f - group_width * 0.5f + 18.0f + 6.0f + phrase_width + 6.0f;
    const auto background = st.surface->read_pixel(450, static_cast<int>(first.y + 12.0f));
    int timestamp_ink = 0;
    for (int y = static_cast<int>(first.y); y < static_cast<int>(first.y + first.h); ++y)
    {
        for (int x = static_cast<int>(std::ceil(time_x));
             x < static_cast<int>(std::floor(time_x + time_width)); ++x)
        {
            const auto c = st.surface->read_pixel(x, y);
            if (c.r != background.r || c.g != background.g || c.b != background.b)
            {
                ++timestamp_ink;
            }
        }
    }
    CHECK(timestamp_ink > 0);

    alice.timestamp_ms = 0;
    bob.timestamp_ms = 0;
    v.set_messages({alice, bob}, false);
    st.surface = TestSurface::create(1600, 600);
    avatar = st.surface->factory().create_image_rgba(red, 1, 1);
    REQUIRE(avatar != nullptr);
    st.run(v, 1600);
    const tk::Rect new_collapsed = v.row_world_rect(0);
    const float new_avatar = avatar_x(new_collapsed);
    REQUIRE(new_avatar > 0.0f);
    const tk::Point expand_again{new_avatar, new_collapsed.y + 12.0f};
    REQUIRE(v.on_pointer_down(expand_again));
    v.on_pointer_up(expand_again, true);
    st.surface = TestSurface::create(1600, 600);
    avatar = st.surface->factory().create_image_rgba(red, 1, 1);
    REQUIRE(avatar != nullptr);
    st.run(v, 1600);
    const float no_timestamp_avatar = avatar_x(v.row_world_rect(0));
    CHECK(no_timestamp_avatar ==
          Catch::Approx(800.0f - (18.0f + 6.0f + phrase_width) * 0.5f + 9.0f).margin(2.0f));
}

TEST_CASE("centered system message avatars track summary changes and resizing",
          "[message_list][membership][system_messages][column]")
{
    ColumnStage st;
    auto& settings = Settings::instance();
    settings.message_layout = Settings::MessageLayout::Bubbles;
    settings.system_message_timeline_alignment = Settings::SystemMessageTimelineAlignment::Center;
    settings.timeline_column_anchor = Anchor::Center;
    const std::uint8_t red[] = {255, 0, 0, 255};
    auto avatar = st.surface->factory().create_image_rgba(red, 1, 1);
    REQUIRE(avatar != nullptr);

    auto alice = membership("$alice", "@alice:x");
    alice.membership_target_name = "Alice";
    alice.membership_target_avatar_url = "mxc://x/alice";
    auto bob = membership("$bob", "@bob:x");
    bob.membership_target_name = "Bob";
    bob.membership_target_avatar_url = "mxc://x/bob";
    bob.membership_action = tesseract::MembershipAction::Left;

    MessageListView v;
    v.set_content_column_enabled(true);
    v.set_avatar_provider(
        [&](const std::string&)
        {
            return avatar.get();
        });
    v.set_messages({alice, bob}, false);
    std::string clicked;
    v.on_sender_clicked = [&](const std::string& uid, const std::string&, const std::string&)
    {
        clicked = uid;
    };

    auto check_avatar = [&](float width, int line, const std::string& expected)
    {
        st.run(v, width);
        const auto row = v.row_world_rect(0);
        const int y = static_cast<int>(row.y + 12.0f + 24.0f * line);
        int x = 0;
        for (; x < static_cast<int>(width); ++x)
        {
            const auto c = st.surface->read_pixel(x, y);
            if (c.r > 240 && c.g < 15 && c.b < 15)
            {
                break;
            }
        }
        REQUIRE(x < static_cast<int>(width));
        const tk::Point point{static_cast<float>(x) + 9.0f, static_cast<float>(y)};
        clicked.clear();
        REQUIRE(v.on_pointer_down(point));
        v.on_pointer_up(point, true);
        CHECK(clicked == expected);
    };
    auto new_surface = [&]
    {
        st.surface = TestSurface::create(1600, 600);
        avatar = st.surface->factory().create_image_rgba(red, 1, 1);
        REQUIRE(avatar != nullptr);
    };

    check_avatar(1600, 0, "@alice:x");
    new_surface();
    check_avatar(1600, 1, "@bob:x");
    bob.membership_target_name = "Bob with a much longer display name";
    v.update_message(1, bob);
    new_surface();
    check_avatar(1600, 1, "@bob:x");
    new_surface();
    check_avatar(1000, 1, "@bob:x");

    auto carol = membership("$carol", "@carol:x");
    carol.membership_action = tesseract::MembershipAction::Invited;
    carol.membership_target_name = "Carol";
    carol.membership_target_avatar_url = "mxc://x/carol";
    v.insert_message(1, carol);
    new_surface();
    check_avatar(1000, 2, "@bob:x");
    v.remove_message(1);
    new_surface();
    check_avatar(1000, 1, "@bob:x");

    settings.system_message_timeline_alignment = Settings::SystemMessageTimelineAlignment::Left;
    v.on_display_prefs_changed();
    new_surface();
    check_avatar(1000, 1, "@bob:x");
}

TEST_CASE("centered system message avatars stay within a narrow list",
          "[message_list][membership][system_messages][column]")
{
    ColumnStage st;
    auto& settings = Settings::instance();
    settings.message_layout = Settings::MessageLayout::Bubbles;
    settings.system_message_timeline_alignment = Settings::SystemMessageTimelineAlignment::Center;
    const std::uint8_t red[] = {255, 0, 0, 255};
    auto avatar = st.surface->factory().create_image_rgba(red, 1, 1);
    REQUIRE(avatar != nullptr);
    auto alice = membership("$alice", "@alice:x");
    alice.membership_target_name = "Alice";
    alice.membership_target_avatar_url = "mxc://x/alice";
    MessageListView v;
    v.set_avatar_provider(
        [&](const std::string&)
        {
            return avatar.get();
        });
    v.set_messages({alice}, false);
    std::string clicked;
    v.on_sender_clicked = [&](const std::string& uid, const std::string&, const std::string&)
    {
        clicked = uid;
    };
    st.run(v, 12);
    const auto row = v.row_world_rect(0);
    const int y = static_cast<int>(row.y + 12.0f);
    const auto inside = st.surface->read_pixel(6, y);
    CHECK(inside.r > 240);
    CHECK(inside.g < 15);
    CHECK(inside.b < 15);
    for (int x = 12; x < 30; ++x)
    {
        const auto outside = st.surface->read_pixel(x, y);
        CHECK_FALSE((outside.r > 240 && outside.g < 15 && outside.b < 15));
    }
    const tk::Point point{6.0f, static_cast<float>(y)};
    REQUIRE(v.on_pointer_down(point));
    v.on_pointer_up(point, true);
    CHECK(clicked == "@alice:x");
}

TEST_CASE("message list column: day separators keep the full width", "[message_list][column]")
{
    ColumnStage st;
    MessageListView plain;
    plain.set_messages({day_separator(), long_text("$a")}, false);
    st.run(plain, 800);
    MessageListView col;
    col.set_content_column_enabled(true);
    col.set_messages({day_separator(), long_text("$a")}, false);
    st.run(col, 1600);
    // The separator row spans the whole list, the message row only the column.
    CHECK(col.row_world_rect(0).w == Catch::Approx(1600.0f));
    CHECK(col.content_height() == Catch::Approx(plain.content_height()));
}

TEST_CASE("message list column: toggling the setting re-measures an open list",
          "[message_list][column]")
{
    ColumnStage st;
    MessageListView v;
    v.set_content_column_enabled(true);
    v.set_messages({long_text("$a")}, false);
    st.run(v, 1600);
    const float at_50 = v.content_height();

    Settings::instance().timeline_column_pct = 100;
    v.on_display_prefs_changed();
    st.run(v, 1600);
    CHECK(v.content_height() < at_50);
}
