#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/side_tab_view.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using namespace tk;

namespace
{

// Minimal content widget — no drawing needed for state tests.
struct NullWidget : Widget
{
    Size measure(LayoutCtx&, Size constraints) override
    {
        return constraints;
    }
    void paint(PaintCtx&) override
    {
    }
};

struct TkSideTabViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(600, 400);

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

// ---------------------------------------------------------------------------

TEST_CASE("SideTabView starts with selected_idx == -1", "[tk][side_tab_view]")
{
    SideTabView tabs;
    CHECK(tabs.selected_idx() == -1);
}

TEST_CASE("SideTabView first tab auto-selected on add_tab",
          "[tk][side_tab_view]")
{
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());
    CHECK(tabs.selected_idx() == 0);
}

TEST_CASE("SideTabView select fires on_tab_selected callback",
          "[tk][side_tab_view]")
{
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());
    tabs.add_tab("Privacy", std::make_unique<NullWidget>());

    int fired = -1;
    tabs.on_tab_selected = [&](int idx)
    {
        fired = idx;
    };

    tabs.select(1);
    CHECK(fired == 1);
    CHECK(tabs.selected_idx() == 1);
}

TEST_CASE("SideTabView select does not fire callback when same tab re-selected",
          "[tk][side_tab_view]")
{
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());
    tabs.add_tab("Privacy", std::make_unique<NullWidget>());

    tabs.select(1); // move away from 0 first

    int call_count = 0;
    tabs.on_tab_selected = [&](int)
    {
        ++call_count;
    };

    tabs.select(1); // same index — must not fire
    CHECK(call_count == 0);
}

TEST_CASE("SideTabView select out-of-range is a no-op", "[tk][side_tab_view]")
{
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());

    int fired = -99;
    tabs.on_tab_selected = [&](int idx)
    {
        fired = idx;
    };

    tabs.select(-1);
    tabs.select(5);
    CHECK(tabs.selected_idx() == 0); // unchanged from auto-select
    CHECK(fired == -99);             // callback never called
}

TEST_CASE("SideTabView tab_visible defaults to true for every added tab",
          "[tk][side_tab_view]")
{
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());
    tabs.add_bottom_tab("About", std::make_unique<NullWidget>());
    CHECK(tabs.tab_visible(0));
    CHECK(tabs.tab_visible(1));
}

TEST_CASE("SideTabView tab_visible reflects set_tab_visible", "[tk][side_tab_view]")
{
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());
    tabs.add_bottom_tab("Advanced", std::make_unique<NullWidget>());

    tabs.set_tab_visible(1, false);
    CHECK_FALSE(tabs.tab_visible(1));

    tabs.set_tab_visible(1, true);
    CHECK(tabs.tab_visible(1));
}

TEST_CASE("SideTabView tab_visible out-of-range returns false",
          "[tk][side_tab_view]")
{
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());
    CHECK_FALSE(tabs.tab_visible(-1));
    CHECK_FALSE(tabs.tab_visible(5));
}

TEST_CASE("SideTabView measure and paint do not crash with two tabs",
          "[tk][side_tab_view]")
{
    TkSideTabViewStage st;
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());
    tabs.add_tab("Privacy", std::make_unique<NullWidget>());
    tabs.select(1);

    // Should not throw or assert.
    st.run(tabs, {0, 0, 600, 400});
    CHECK(tabs.selected_idx() == 1);
}

// ---------------------------------------------------------------------------
// Scroll support — the sidebar column (kSidebarWidth == 200) scrolls its top
// tab group when there isn't enough vertical room; bottom-pinned tabs stay
// fixed.
// ---------------------------------------------------------------------------

TEST_CASE("SideTabView wheel scrolls the top group when it overflows",
          "[tk][side_tab_view][scroll]")
{
    TkSideTabViewStage st;
    SideTabView tabs;
    for (int i = 0; i < 10; ++i)
    {
        tabs.add_tab("Tab" + std::to_string(i), std::make_unique<NullWidget>());
    }
    tabs.add_bottom_tab("About", std::make_unique<NullWidget>());

    // 10 top tabs + 1 bottom tab at 36px each == 396px; a 150px viewport
    // overflows comfortably.
    st.run(tabs, {0, 0, 600, 150});

    // Wheel over the sidebar column (x < kSidebarWidth == 200) scrolls.
    REQUIRE(tabs.on_wheel({50.0f, 10.0f}, 0.0f, 60.0f) == true);
    CHECK(tabs.scroll_y_for_testing() > 0.0f);

    // Scrolling far past the bottom pins at max_scroll; a further wheel-down
    // is then a no-op.
    REQUIRE(tabs.on_wheel({50.0f, 10.0f}, 0.0f, 1e6f) == true);
    const float max_scroll = tabs.scroll_y_for_testing();
    REQUIRE(tabs.on_wheel({50.0f, 10.0f}, 0.0f, 60.0f) == false);
    CHECK(tabs.scroll_y_for_testing() == max_scroll);

    // Scrolling back up consumes again, eventually pinning at 0.
    REQUIRE(tabs.on_wheel({50.0f, 10.0f}, 0.0f, -1e6f) == true);
    CHECK(tabs.scroll_y_for_testing() == 0.0f);
    REQUIRE(tabs.on_wheel({50.0f, 10.0f}, 0.0f, -60.0f) == false);
}

TEST_CASE("SideTabView wheel over the content pane does not scroll the sidebar",
          "[tk][side_tab_view][scroll]")
{
    TkSideTabViewStage st;
    SideTabView tabs;
    for (int i = 0; i < 10; ++i)
    {
        tabs.add_tab("Tab" + std::to_string(i), std::make_unique<NullWidget>());
    }
    st.run(tabs, {0, 0, 600, 150});

    // x >= kSidebarWidth (200) is over the content pane, not the tab column.
    REQUIRE(tabs.on_wheel({250.0f, 10.0f}, 0.0f, 60.0f) == false);
    CHECK(tabs.scroll_y_for_testing() == 0.0f);
}

TEST_CASE("SideTabView wheel is a no-op when every tab fits the viewport",
          "[tk][side_tab_view][scroll]")
{
    TkSideTabViewStage st;
    SideTabView tabs;
    tabs.add_tab("General", std::make_unique<NullWidget>());
    tabs.add_tab("Privacy", std::make_unique<NullWidget>());
    st.run(tabs, {0, 0, 600, 400});

    REQUIRE(tabs.on_wheel({50.0f, 10.0f}, 0.0f, 60.0f) == false);
    REQUIRE(tabs.on_wheel({50.0f, 10.0f}, 0.0f, -60.0f) == false);
}

TEST_CASE("SideTabView hit-testing stays consistent with painted positions "
          "after scrolling",
          "[tk][side_tab_view][scroll]")
{
    TkSideTabViewStage st;
    SideTabView tabs;
    for (int i = 0; i < 10; ++i)
    {
        tabs.add_tab("Tab" + std::to_string(i), std::make_unique<NullWidget>());
    }
    tabs.add_bottom_tab("About", std::make_unique<NullWidget>());

    // 10 top tabs (360px) + 1 bottom tab (36px) vs. a 150px viewport ->
    // max_top_scroll_ == 360 - (150 - 36) == 246.
    st.run(tabs, {0, 0, 600, 150});

    // Before scrolling, tab index 2 sits at local y in [72, 108).
    Widget* claimer = tabs.dispatch_pointer_down({50.0f, 82.0f});
    REQUIRE(claimer == &tabs);
    claimer->on_pointer_up({50.0f, 82.0f}, true);
    CHECK(tabs.selected_idx() == 2);

    // Scroll to max — the last top tab (index 9) is now the last one visible,
    // flush against the bottom-pinned group.
    REQUIRE(tabs.on_wheel({50.0f, 10.0f}, 0.0f, 1e6f) == true);
    CHECK(tabs.scroll_y_for_testing() == Catch::Approx(246.0f));

    // Tab 9 now sits at local y in [78, 114) (9*36 - 246 == 78).
    claimer = tabs.dispatch_pointer_down({50.0f, 90.0f});
    REQUIRE(claimer == &tabs);
    claimer->on_pointer_up({50.0f, 90.0f}, true);
    CHECK(tabs.selected_idx() == 9);

    // The bottom-pinned "About" tab (index 10) remains reachable at the
    // very bottom of the sidebar regardless of scroll position.
    claimer = tabs.dispatch_pointer_down({50.0f, 130.0f});
    REQUIRE(claimer == &tabs);
    claimer->on_pointer_up({50.0f, 130.0f}, true);
    CHECK(tabs.selected_idx() == 10);
}
