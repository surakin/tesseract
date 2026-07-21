#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/tab_view.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using namespace tk;

namespace
{

struct TkTabViewStage
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

TEST_CASE("TabView starts with selected_index == 0", "[tk][tab_view]")
{
    auto tv_owner = tk::create_root_widget<TabView>(nullptr);
    TabView& tv = *tv_owner;
    tv.set_items({"Join", "Create"});
    CHECK(tv.selected_index() == 0);
}

TEST_CASE("TabView set_selected_index fires on_selected only when the index "
          "actually changes",
          "[tk][tab_view]")
{
    auto tv_owner = tk::create_root_widget<TabView>(nullptr);
    TabView& tv = *tv_owner;
    tv.set_items({"Join", "Create"});

    int fired = -1;
    int call_count = 0;
    tv.on_selected = [&](int idx)
    {
        fired = idx;
        ++call_count;
    };

    tv.set_selected_index(1);
    CHECK(fired == 1);
    CHECK(call_count == 1);
    CHECK(tv.selected_index() == 1);

    tv.set_selected_index(1); // same index — must not fire again
    CHECK(call_count == 1);
}

TEST_CASE("TabView set_selected_index out-of-range is a silent no-op",
          "[tk][tab_view]")
{
    auto tv_owner = tk::create_root_widget<TabView>(nullptr);
    TabView& tv = *tv_owner;
    tv.set_items({"Join", "Create"});

    int fired = -99;
    tv.on_selected = [&](int idx) { fired = idx; };

    tv.set_selected_index(-1);
    tv.set_selected_index(5);
    CHECK(tv.selected_index() == 0);
    CHECK(fired == -99);
}

TEST_CASE("TabView set_items does not fire on_selected", "[tk][tab_view]")
{
    auto tv_owner = tk::create_root_widget<TabView>(nullptr);
    TabView& tv = *tv_owner;

    bool fired = false;
    tv.on_selected = [&](int) { fired = true; };

    tv.set_items({"Join", "Create"});
    CHECK_FALSE(fired);
    CHECK(tv.selected_index() == 0);
}

TEST_CASE("TabView focusable() requires more than one item", "[tk][tab_view]")
{
    auto tv_owner = tk::create_root_widget<TabView>(nullptr);
    TabView& tv = *tv_owner;

    CHECK_FALSE(tv.focusable()); // no items yet

    tv.set_items({"Only"});
    CHECK_FALSE(tv.focusable());

    tv.set_items({"Join", "Create"});
    CHECK(tv.focusable());
}

TEST_CASE("TabView measure and paint do not crash with two items",
          "[tk][tab_view]")
{
    TkTabViewStage st;
    auto tv_owner = tk::create_root_widget<TabView>(nullptr);
    TabView& tv = *tv_owner;
    tv.set_items({"Join", "Create"});

    // Should not throw or assert.
    st.run(tv, {0, 0, 400, 32});
    CHECK(tv.selected_index() == 0);
}

TEST_CASE("TabView pointer click on a segment selects it", "[tk][tab_view]")
{
    TkTabViewStage st;
    auto tv_owner = tk::create_root_widget<TabView>(nullptr);
    TabView& tv = *tv_owner;
    tv.set_items({"Join", "Create"});
    st.run(tv, {0, 0, 400, 32});

    // Two equal segments spanning [0,400) x [0,32) — x=300 lands in the
    // second (Create) segment.
    Widget* claimer = tv.dispatch_pointer_down({300.0f, 10.0f});
    REQUIRE(claimer == &tv);
    claimer->on_pointer_up({300.0f, 10.0f}, true);
    CHECK(tv.selected_index() == 1);
}

TEST_CASE("TabView a press that releases outside inside_self does not commit",
          "[tk][tab_view]")
{
    TkTabViewStage st;
    auto tv_owner = tk::create_root_widget<TabView>(nullptr);
    TabView& tv = *tv_owner;
    tv.set_items({"Join", "Create"});
    st.run(tv, {0, 0, 400, 32});

    Widget* claimer = tv.dispatch_pointer_down({300.0f, 10.0f});
    REQUIRE(claimer == &tv);
    claimer->on_pointer_up({300.0f, 10.0f}, false); // inside_self == false
    CHECK(tv.selected_index() == 0);                // unchanged
}
