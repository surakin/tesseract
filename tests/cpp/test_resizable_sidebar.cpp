#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "tesseract/types.h"
#include "tesseract/visual.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"
#include "views/MainAppWidget.h"
#include "views/RoomListView.h"
#include "views/sidebar_metrics.h"

#include <memory>
#include <string>

using Catch::Matchers::WithinAbs;
using tesseract::RoomInfo;
using tesseract::views::RoomListView;
using namespace tesseract::views;

namespace
{
constexpr float kDefault = static_cast<float>(tesseract::visual::kSidebarWidth);            // 260
constexpr float kCollapsed = static_cast<float>(tesseract::visual::kSidebarCollapsedWidth);  // 68
constexpr float kMinExpanded = static_cast<float>(tesseract::visual::kSidebarMinExpandedWidth); // 180
constexpr float kHuge = 100000.0f;
} // namespace

TEST_CASE("sidebar_max_width: window-fraction term floored at default width", "[sidebar]")
{
    // Wide window: half of 2000 = 1000, well above the 260 floor and below kHuge.
    CHECK_THAT(sidebar_max_width(2000.0f, kHuge), WithinAbs(1000.0f, 0.01f));

    // Narrow window: half of 300 = 150, below the 260 floor → floored to 260.
    CHECK_THAT(sidebar_max_width(300.0f, kHuge), WithinAbs(kDefault, 0.01f));

    // Exactly at 520 the half term equals the floor.
    CHECK_THAT(sidebar_max_width(520.0f, kHuge), WithinAbs(kDefault, 0.01f));
}

TEST_CASE("sidebar_max_width: longest-row term caps the result", "[sidebar]")
{
    // Longest visible room row only needs 340px → that wins over the 1000px window term.
    CHECK_THAT(sidebar_max_width(2000.0f, 340.0f), WithinAbs(340.0f, 0.01f));

    // If every room is short (row content 120px) the cap can fall below the
    // default width — you simply cannot drag the list wider than its content.
    CHECK_THAT(sidebar_max_width(2000.0f, 120.0f), WithinAbs(120.0f, 0.01f));

    // No rows to measure → the longest-row term is disabled entirely.
    CHECK_THAT(sidebar_max_width(2000.0f, 0.0f), WithinAbs(1000.0f, 0.01f));
    CHECK_THAT(sidebar_max_width(2000.0f, -1.0f), WithinAbs(1000.0f, 0.01f));
}

TEST_CASE("resolve_sidebar_drag: snaps to collapsed below the midpoint", "[sidebar]")
{
    const float midpoint = (kCollapsed + kMinExpanded) * 0.5f; // 124

    auto below = resolve_sidebar_drag(midpoint - 1.0f, kHuge);
    CHECK(below.collapsed);

    auto at_collapsed = resolve_sidebar_drag(kCollapsed, kHuge);
    CHECK(at_collapsed.collapsed);

    auto tiny = resolve_sidebar_drag(10.0f, kHuge);
    CHECK(tiny.collapsed);
}

TEST_CASE("resolve_sidebar_drag: expanded result clamped to [min, max]", "[sidebar]")
{
    const float midpoint = (kCollapsed + kMinExpanded) * 0.5f;

    // Just above the midpoint but below kMinExpanded → not collapsed, clamped up to min.
    auto low = resolve_sidebar_drag(midpoint + 1.0f, kHuge);
    CHECK_FALSE(low.collapsed);
    CHECK_THAT(low.width, WithinAbs(kMinExpanded, 0.01f));

    // A normal drag inside the range passes through untouched.
    auto mid = resolve_sidebar_drag(300.0f, 500.0f);
    CHECK_FALSE(mid.collapsed);
    CHECK_THAT(mid.width, WithinAbs(300.0f, 0.01f));

    // Beyond the max is clamped down to the max.
    auto high = resolve_sidebar_drag(900.0f, 400.0f);
    CHECK_FALSE(high.collapsed);
    CHECK_THAT(high.width, WithinAbs(400.0f, 0.01f));
}

TEST_CASE("resolve_sidebar_drag: degenerate max below min still yields min", "[sidebar]")
{
    auto r = resolve_sidebar_drag(500.0f, 50.0f);
    CHECK_FALSE(r.collapsed);
    CHECK_THAT(r.width, WithinAbs(kMinExpanded, 0.01f));
}

// ── RoomListView::longest_visible_row_width / set_icon_only ────────────────

namespace
{
RoomInfo sidebar_test_room(const std::string& id, const std::string& name)
{
    RoomInfo r;
    r.id = id;
    r.name = name;
    r.last_activity_ts = 1'000'000'000'000ULL; // recent → normal "Rooms" section
    return r;
}

// longest_visible_row_width() only measures rows currently on screen, so the
// view must be laid out first.
float measured_width(RoomListView& v, TestSurface& s)
{
    tk::LayoutCtx lc{s.factory(), tk::Theme::light()};
    v.measure(lc, {300.0f, 600.0f});
    v.arrange(lc, {0, 0, 300.0f, 600.0f});
    return v.longest_visible_row_width(s.factory(), tk::Theme::light());
}
} // namespace

TEST_CASE("longest_visible_row_width: zero when there are no rooms", "[sidebar]")
{
    auto surface = TestSurface::create(320, 640);
    auto view = tk::create_root_widget<RoomListView>(nullptr);
    CHECK(measured_width(*view, *surface) == 0.0f);
}

TEST_CASE("longest_visible_row_width: grows with a longer room name", "[sidebar]")
{
    auto surface = TestSurface::create(320, 640);
    auto view = tk::create_root_widget<RoomListView>(nullptr);

    view->set_rooms({sidebar_test_room("$a", "Hi")});
    const float narrow = measured_width(*view, *surface);

    view->set_rooms({sidebar_test_room(
        "$a", "A really quite extraordinarily long room display name here")});
    const float wide = measured_width(*view, *surface);

    CHECK(wide > narrow);
    // Always leaves room for the avatar column + insets.
    CHECK(narrow > tesseract::visual::kRoomAvatarSize);
}

TEST_CASE("longest_visible_row_width: ignores rows in a collapsed section",
          "[sidebar]")
{
    auto surface = TestSurface::create(320, 640);
    auto view = tk::create_root_widget<RoomListView>(nullptr);

    view->set_rooms({sidebar_test_room("$a", "short"),
                     sidebar_test_room("$b", "a much much much much longer name")});
    const float with_both = measured_width(*view, *surface);

    // Both land in "Rooms" (kSecRooms) — collapsing it hides every room row.
    view->set_section_collapsed(RoomListView::kSecRooms, true);
    const float collapsed = measured_width(*view, *surface);

    CHECK(with_both > 0.0f);
    CHECK(collapsed == 0.0f);
}

TEST_CASE("set_icon_only toggles cleanly", "[sidebar]")
{
    auto surface = TestSurface::create(320, 640);
    auto view = tk::create_root_widget<RoomListView>(nullptr);
    view->set_rooms({sidebar_test_room("$a", "Room A"),
                     sidebar_test_room("$b", "Room B")});
    (void)measured_width(*view, *surface);

    view->set_icon_only(true);
    CHECK(view->icon_only());
    (void)measured_width(*view, *surface); // must not crash in icon-only mode

    view->set_icon_only(false);
    CHECK_FALSE(view->icon_only());
}

// ── RootLayoutWidget drag/toggle through MainAppWidget ────────────────────

namespace
{
struct AppStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<tesseract::views::MainAppWidget> app =
        tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);

    void layout(float w = 1000.0f, float h = 700.0f)
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        app->measure(lc, {w, h});
        app->arrange(lc, {0, 0, w, h});
    }
};
} // namespace

TEST_CASE("sidebar separator drag widens the list and reports the new width",
          "[sidebar]")
{
    AppStage st;
    st.layout();

    float last_w = -1.0f;
    bool  last_collapsed = true;
    st.app->on_sidebar_layout_changed = [&](float w, bool c)
    { last_w = w; last_collapsed = c; };

    // Press on the separator (default sidebar width = 260), drag right to 420.
    tk::Widget* grip = st.app->dispatch_pointer_down({kDefault, 300.0f});
    REQUIRE(grip != nullptr);
    REQUIRE(grip != st.app.get()); // the private RootLayoutWidget, not the root
    grip->on_pointer_drag({420.0f, 300.0f});
    grip->on_pointer_up({420.0f, 300.0f}, true);

    CHECK_FALSE(last_collapsed);
    CHECK_THAT(last_w, WithinAbs(420.0f, 1.0f));
}

TEST_CASE("sidebar grip click (no drag) toggles collapsed", "[sidebar]")
{
    AppStage st;
    st.layout(1000.0f, 700.0f);

    int calls = 0;
    bool last_collapsed = false;
    st.app->on_sidebar_layout_changed = [&](float, bool c)
    { ++calls; last_collapsed = c; };

    // The grip sits on the separator at the top of the 64 px account strip:
    // y = 700 - 64 = 636, x = expanded sidebar width (260).
    const float grip_y = 700.0f - static_cast<float>(tesseract::visual::kUserStripHeight);
    tk::Widget* grip = st.app->dispatch_pointer_down({kDefault, grip_y});
    REQUIRE(grip != nullptr);
    grip->on_pointer_up({kDefault, grip_y}, true); // released without moving

    CHECK(calls == 1);
    CHECK(last_collapsed); // toggled on

    // A second click (grip now at the collapsed width) toggles back off.
    const float cw = static_cast<float>(tesseract::visual::kSidebarCollapsedWidth);
    grip = st.app->dispatch_pointer_down({cw, grip_y});
    REQUIRE(grip != nullptr);
    grip->on_pointer_up({cw, grip_y}, true);
    CHECK(calls == 2);
    CHECK_FALSE(last_collapsed);
}

TEST_CASE("sidebar band click away from the grip does not toggle", "[sidebar]")
{
    AppStage st;
    st.layout();

    bool last_collapsed = true; // seed opposite of the expected result
    st.app->on_sidebar_layout_changed = [&](float, bool c) { last_collapsed = c; };

    // Press on the separator band well above the grip, release without moving.
    tk::Widget* w = st.app->dispatch_pointer_down({kDefault, 200.0f});
    REQUIRE(w != nullptr);
    w->on_pointer_up({kDefault, 200.0f}, true);

    CHECK_FALSE(last_collapsed); // a non-grip click never collapses
}
