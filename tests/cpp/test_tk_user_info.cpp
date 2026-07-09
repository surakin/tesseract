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
    // Two-line layout — name + ID + paddings should sit at ≈48 px (the
    // existing sidebar strip height). Allow a small backend-dependent
    // tolerance.
    CHECK(sz.h >= 44.0f);
    CHECK(sz.h <= 56.0f);
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
