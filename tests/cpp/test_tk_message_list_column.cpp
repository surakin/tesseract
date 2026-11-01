#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "settings_guard.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MessageListView.h"

#include <tesseract/settings.h>

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
    void run(tk::Widget& root, float w)
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
