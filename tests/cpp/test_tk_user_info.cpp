#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/UserInfo.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::UserInfo;

namespace
{

struct TkUserInfoStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 80);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("UserInfo natural height shows the Matrix ID line by default",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Alice");
    info.set_user_id("@alice:example.org");

    auto lc = st.layout_ctx();
    auto sz = info.measure(lc, {320.0f, 0.0f});
    // Two-line layout, no status: height is driven by the 44 px avatar plus
    // padding (the text column alone is shorter). Allow a small tolerance.
    CHECK(sz.h >= 56.0f);
    CHECK(sz.h <= 64.0f);
}

TEST_CASE("UserInfo icon-only mode collapses to avatar height and centres it",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Alice");
    info.set_user_id("@alice:example.org");
    info.set_status_line_enabled(true);
    info.set_status("", "");

    auto lc = st.layout_ctx();
    const float full_h = info.measure(lc, {320.0f, 0.0f}).h;

    info.set_icon_only(true);
    CHECK(info.icon_only());
    const float icon_h = info.measure(lc, {320.0f, 0.0f}).h;

    // Text column is gone: height is just the 44 px avatar + 2×8 px padding.
    CHECK(icon_h < full_h);
    CHECK(icon_h >= 44.0f);
    CHECK(icon_h <= 64.0f);

    // Still paints cleanly in the narrow collapsed strip.
    st.run(info, {0, 0, 68, 64});
    SUCCEED();
}

TEST_CASE("UserInfo paints without crashing when no image_provider is wired",
          "[tk][view][user_info]")
{
    // The canvas has built-in initials-disc rendering, so a UserInfo with
    // no provider and an avatar URL must still paint cleanly via the
    // fallback path.
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Bob");
    info.set_user_id("@bob:matrix.org");
    info.set_avatar_url("mxc://example/never-resolved");
    st.run(info, {0, 0, 320, 48});
    // No assert needed — Catch2 fails on any thrown exception.
    SUCCEED();
}

TEST_CASE("UserInfo image_provider receives the avatar URL on paint",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Carol");
    info.set_user_id("@carol:example.org");
    info.set_avatar_url("mxc://server/abc123");

    std::string requested;
    info.set_image_provider(
        [&](const std::string& mxc) -> const tk::Image*
        {
            requested = mxc;
            return nullptr; // force the initials fallback path
        });
    st.run(info, {0, 0, 320, 48});
    CHECK(requested == "mxc://server/abc123");
}

TEST_CASE(
    "UserInfo primary callback fires on a click that lands inside the row",
    "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Dave");
    info.set_user_id("@dave:matrix.org");

    bool fired = false;
    tk::Point seen{};
    info.on_primary = [&](tk::Point world)
    {
        fired = true;
        seen = world;
    };

    st.run(info, {0, 0, 320, 48});

    // Click at the middle of the row.
    Widget* claimer = info.dispatch_pointer_down({160.0f, 24.0f});
    REQUIRE(claimer == &info);
    info.on_pointer_up({160.0f - info.bounds().x, 24.0f - info.bounds().y},
                       /*inside_self=*/true);

    CHECK(fired);
    CHECK(seen.x == 160.0f);
    CHECK(seen.y == 24.0f);
}

TEST_CASE(
    "UserInfo primary callback does NOT fire on a release outside the row",
    "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Eve");

    bool fired = false;
    info.on_primary = [&](tk::Point)
    {
        fired = true;
    };

    st.run(info, {0, 0, 320, 48});
    info.dispatch_pointer_down({50.0f, 24.0f});
    info.on_pointer_up({-50.0f, 24.0f}, /*inside_self=*/false);
    CHECK_FALSE(fired);
}

TEST_CASE("UserInfo::on_secondary is settable and survives paint cycles",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Frank");

    bool fired = false;
    info.on_secondary = [&](tk::Point)
    {
        fired = true;
    };

    st.run(info, {0, 0, 320, 48});

    // The shell invokes the callback directly when its platform-native
    // right-click signal lands on the strip.
    REQUIRE(info.on_secondary);
    info.on_secondary({100.0f, 24.0f});
    CHECK(fired);
}

TEST_CASE("UserInfo active_indicator toggles independently of content",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Grace");
    CHECK_FALSE(info.active_indicator());
    info.set_active_indicator(true);
    CHECK(info.active_indicator());

    st.run(info, {0, 0, 320, 48}); // must not crash with the indicator on
    SUCCEED();
}

// ── MSC4426 status line (sidebar strip only) ───────────────────────────────

TEST_CASE("UserInfo status line is inert until enabled",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Alice");
    info.set_user_id("@alice:example.org");

    auto lc = st.layout_ctx();
    const float without = info.measure(lc, {320.0f, 0.0f}).h;
    info.set_status("PALM", "On holiday"); // no visual effect while disabled
    CHECK(info.measure(lc, {320.0f, 0.0f}).h == without);
}

TEST_CASE("UserInfo status line grows the row when enabled",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Alice");
    info.set_user_id("@alice:example.org");

    auto lc = st.layout_ctx();
    const float two_line = info.measure(lc, {320.0f, 0.0f}).h;
    info.set_status_line_enabled(true);
    CHECK(info.measure(lc, {320.0f, 0.0f}).h > two_line);
}

TEST_CASE("UserInfo status-line click fires on_status_clicked, not on_primary",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Alice");
    info.set_user_id("@alice:example.org");
    info.set_status_line_enabled(true);
    info.set_status("PALM", "On holiday");

    bool status_fired = false;
    bool primary_fired = false;
    info.on_status_clicked = [&] { status_fired = true; };
    info.on_primary = [&](tk::Point) { primary_fired = true; };

    st.run(info, {0, 0, 320, 80});

    // Near the bottom → the status line (the last of the three text rows).
    info.dispatch_pointer_down({180.0f, 62.0f});
    info.on_pointer_up({180.0f - info.bounds().x, 62.0f - info.bounds().y},
                       /*inside_self=*/true);

    CHECK(status_fired);
    CHECK_FALSE(primary_fired);
}

TEST_CASE("UserInfo avatar click still fires on_primary with status enabled",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Alice");
    info.set_user_id("@alice:example.org");
    info.set_status_line_enabled(true);
    info.set_status("PALM", "On holiday");

    bool status_fired = false;
    bool primary_fired = false;
    info.on_status_clicked = [&] { status_fired = true; };
    info.on_primary = [&](tk::Point) { primary_fired = true; };

    st.run(info, {0, 0, 320, 80});

    // Over the avatar disc (far from the status line).
    info.dispatch_pointer_down({28.0f, 40.0f});
    info.on_pointer_up({28.0f - info.bounds().x, 40.0f - info.bounds().y},
                       /*inside_self=*/true);

    CHECK(primary_fired);
    CHECK_FALSE(status_fired);
}

TEST_CASE("UserInfo placeholder status paints without crashing",
          "[tk][view][user_info]")
{
    TkUserInfoStage st;
    UserInfo info;
    info.set_display_name("Alice");
    info.set_user_id("@alice:example.org");
    info.set_status_line_enabled(true); // no set_status → placeholder path
    st.run(info, {0, 0, 320, 80});
    SUCCEED();
}
