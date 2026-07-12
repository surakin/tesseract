#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/side_tab_view.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"

#include <memory>

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
