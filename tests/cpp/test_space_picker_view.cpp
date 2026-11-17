#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "view_test_util.h"
#include "views/SpacePickerView.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using tesseract::views::SpacePickerEntry;
using tesseract::views::SpacePickerView;

namespace
{

SpacePickerEntry entry(const std::string& id, const std::string& name)
{
    SpacePickerEntry e;
    e.space_id = id;
    e.name = name;
    return e;
}

struct PickerStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(SpacePickerView::kWidth, 400);

    void layout(tk::Widget& w) const
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        w.measure(lc, {SpacePickerView::kWidth, 400});
        w.arrange(lc, {0, 0, SpacePickerView::kWidth, 400});
    }
};

tk::Point centre_of(const SpacePickerView& v, int tab)
{
    const tk::Rect r = v.tab_rect_for_test(tab);
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}

} // namespace

TEST_CASE("picker: Home plus one tab per entry", "[spacepicker]")
{
    PickerStage st;
    auto owner = tk::create_root_widget<SpacePickerView>(nullptr);
    SpacePickerView& v = *owner;
    CHECK(v.tab_count() == 1);
    v.set_entries({entry("!a", "Alpha"), entry("!b", "Beta")});
    st.layout(v);
    CHECK(v.tab_count() == 3);
    CHECK(v.tab_rect_for_test(1).y > v.tab_rect_for_test(0).y);
}

TEST_CASE("picker: clicking a tab selects its space, Home fires on_home", "[spacepicker]")
{
    PickerStage st;
    auto owner = tk::create_root_widget<SpacePickerView>(nullptr);
    SpacePickerView& v = *owner;
    v.set_entries({entry("!a", "Alpha"), entry("!b", "Beta")});
    st.layout(v);

    std::string selected;
    int home = 0;
    v.on_select = [&](const std::string& id)
    {
        selected = id;
    };
    v.on_home = [&]
    {
        ++home;
    };

    const tk::Point b = centre_of(v, 2);
    REQUIRE(v.on_pointer_down(b));
    v.on_pointer_up(b, true);
    CHECK(selected == "!b");

    const tk::Point h = centre_of(v, 0);
    REQUIRE(v.on_pointer_down(h));
    v.on_pointer_up(h, true);
    CHECK(home == 1);
}

TEST_CASE("picker: releasing off the pressed tab does not select", "[spacepicker]")
{
    PickerStage st;
    auto owner = tk::create_root_widget<SpacePickerView>(nullptr);
    SpacePickerView& v = *owner;
    v.set_entries({entry("!a", "Alpha"), entry("!b", "Beta")});
    st.layout(v);

    std::string selected;
    v.on_select = [&](const std::string& id)
    {
        selected = id;
    };
    v.on_pointer_down(centre_of(v, 1));
    v.on_pointer_up(centre_of(v, 2), true);
    CHECK(selected.empty());
}

TEST_CASE("picker: right-click on a space tab offers unpin, Home has no menu",
          "[spacepicker][context-menu]")
{
    PickerStage st;
    auto owner = tk::create_root_widget<SpacePickerView>(nullptr);
    SpacePickerView& v = *owner;
    v.set_entries({entry("!a", "Alpha")});
    st.layout(v);

    CHECK_FALSE(v.on_right_click(centre_of(v, 0)));

    REQUIRE(v.on_right_click(centre_of(v, 1)));
    const auto& items = v.context_menu_items_for_test();
    REQUIRE(items.size() == 1);
    CHECK(items[0].label == "Unpin from sidebar");

    std::string unpinned;
    v.on_unpin = [&](const std::string& id)
    {
        unpinned = id;
    };
    items[0].on_selected();
    CHECK(unpinned == "!a");
}

TEST_CASE("picker: active space is tracked and entries dedupe", "[spacepicker]")
{
    auto owner = tk::create_root_widget<SpacePickerView>(nullptr);
    SpacePickerView& v = *owner;
    CHECK(v.active_space().empty());
    v.set_active_space("!a");
    CHECK(v.active_space() == "!a");
    v.set_entries({entry("!a", "Alpha")});
    v.set_entries({entry("!a", "Alpha")}); // unchanged: no-op
    CHECK(v.entries().size() == 1);
}

namespace
{

struct DragStage : vt::Stage
{
    std::unique_ptr<SpacePickerView> picker = tk::create_root_widget<SpacePickerView>(&host);
    std::string moved;
    std::string before = "unset";
    int selected = 0;
    int reorders = 0;

    DragStage() : vt::Stage(static_cast<int>(SpacePickerView::kWidth), 400)
    {
        picker->set_entries({entry("!a", "Alpha"), entry("!b", "Beta"), entry("!c", "Gamma")});
        picker->on_reorder = [this](const std::string& id, const std::string& b)
        {
            moved = id;
            before = b;
            ++reorders;
        };
        picker->on_select = [this](const std::string&)
        {
            ++selected;
        };
        mount(*picker, {0, 0, SpacePickerView::kWidth, 400});
    }
    tk::Point at(int tab) const
    {
        const tk::Rect r = picker->tab_rect_for_test(tab);
        return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    }
};

} // namespace

TEST_CASE("picker: dragging a tab down uses the host drag and reorders on drop",
          "[spacepicker][drag]")
{
    DragStage s;
    const tk::Point start = s.at(1);
    s.host.dispatch_pointer_down(start);
    s.host.dispatch_pointer_move({start.x, start.y + 2.0f}); // under the threshold
    CHECK_FALSE(s.host.is_dragging());
    s.host.dispatch_pointer_move({start.x, start.y + 10.0f});
    CHECK(s.host.is_dragging());

    const tk::Point end = s.at(3);
    s.host.dispatch_pointer_move(end);
    CHECK(s.picker->drop_slot_for_test() == 3);
    s.host.dispatch_pointer_up(end);

    CHECK(s.moved == "!a");
    CHECK(s.before.empty()); // now last
    CHECK(s.selected == 0);  // a drag is not a click
    CHECK_FALSE(s.host.is_dragging());
    CHECK(s.picker->drop_slot_for_test() == -1);
    REQUIRE(s.picker->entries().size() == 3);
    CHECK(s.picker->entries()[0].space_id == "!b");
    CHECK(s.picker->entries()[2].space_id == "!a");
}

TEST_CASE("picker: dragging up reports the tab it now precedes", "[spacepicker][drag]")
{
    DragStage s;
    const tk::Point start = s.at(3);
    s.host.dispatch_pointer_down(start);
    s.host.dispatch_pointer_move({start.x, start.y - 10.0f});
    const tk::Point end = s.at(1);
    s.host.dispatch_pointer_move(end);
    s.host.dispatch_pointer_up(end);
    CHECK(s.moved == "!c");
    CHECK(s.before == "!a");
    CHECK(s.picker->entries()[0].space_id == "!c");
}

TEST_CASE("picker: Home is not draggable, dropping in place is a no-op, Escape cancels",
          "[spacepicker][drag]")
{
    DragStage s;
    s.host.dispatch_pointer_down(s.at(0));
    s.host.dispatch_pointer_move({s.at(0).x, s.at(0).y + 30.0f});
    CHECK_FALSE(s.host.is_dragging());
    s.host.dispatch_pointer_up(s.at(2));
    CHECK(s.reorders == 0);

    const tk::Point a = s.at(1);
    s.host.dispatch_pointer_down(a);
    s.host.dispatch_pointer_move({a.x, a.y + 10.0f});
    REQUIRE(s.host.is_dragging());
    s.host.dispatch_pointer_up({a.x, a.y + 10.0f}); // still inside slot 1
    CHECK(s.reorders == 0);
    CHECK(s.picker->entries()[0].space_id == "!a");

    const tk::Point b = s.at(2);
    s.host.dispatch_pointer_down(b);
    s.host.dispatch_pointer_move({b.x, b.y + 10.0f});
    REQUIRE(s.host.is_dragging());
    s.host.cancel_drag();
    CHECK_FALSE(s.host.is_dragging());
    CHECK(s.picker->drop_slot_for_test() == -1);
    CHECK(s.reorders == 0);
}

TEST_CASE("picker: only its own pinned_space_id drags are accepted", "[spacepicker][drag]")
{
    PickerStage st;
    auto owner = tk::create_root_widget<SpacePickerView>(nullptr);
    SpacePickerView& v = *owner;
    v.set_entries({entry("!a", "Alpha"), entry("!b", "Beta")});
    st.layout(v);

    CHECK(v.on_drag_enter({1, 1}, tk::DragPayload("pinned_space_id", std::string("!a"))));
    CHECK_FALSE(v.on_drag_enter({1, 1}, tk::DragPayload("pinned_space_id", std::string("!zz"))));
    CHECK_FALSE(v.on_drag_enter({1, 1}, tk::DragPayload("space_room_id", std::string("!a"))));
    CHECK_FALSE(v.on_drop({1, 1}, tk::DragPayload("pinned_space_id", 7)));
}

TEST_CASE("picker: the drag ghost is exactly one tab avatar in logical pixels",
          "[spacepicker][drag]")
{
    DragStage s;
    const tk::Point start = s.at(1);
    s.host.dispatch_pointer_down(start);
    s.host.dispatch_pointer_move({start.x, start.y + 10.0f});
    REQUIRE(s.host.is_dragging());

    const auto& visual = s.host.active_drag_->visual;
    if (!visual.image) // backend without offscreen rendering
    {
        SKIP("no offscreen surface on this backend");
    }
    const float scale = s.surface->canvas().scale_factor();
    CHECK(visual.image->width() == static_cast<int>(std::ceil(36.0f * scale)));
    CHECK(visual.image->height() == static_cast<int>(std::ceil(36.0f * scale)));
    CHECK(visual.hotspot.x == 18.0f);
    CHECK(visual.hotspot.y == 18.0f);
}

TEST_CASE("picker: an open tab menu registers as the host popup so it can be dismissed",
          "[spacepicker][context-menu]")
{
    DragStage s;
    REQUIRE(s.picker->on_right_click(s.at(1)));

    // A real paint pass carries the host; PopupMenu::paint() registers itself
    // as the active popup there (click-outside / Escape dismissal hangs off it).
    tk::PaintCtx ctx{s.surface->canvas(), s.surface->factory(), tk::Theme::light(), nullptr,
                     &s.host};
    s.picker->paint(ctx);
    CHECK(s.host.pending_popup_.lock() != nullptr);
}

TEST_CASE("picker: wheel scrolls tall tab lists and clamps at both ends", "[spacepicker]")
{
    auto surface = TestSurface::create(SpacePickerView::kWidth, 200);
    auto owner = tk::create_root_widget<SpacePickerView>(nullptr);
    SpacePickerView& v = *owner;
    std::vector<SpacePickerEntry> es;
    es.reserve(30);
    for (int i = 0; i < 30; ++i)
    {
        es.push_back(entry("!s" + std::to_string(i), "Space " + std::to_string(i)));
    }
    v.set_entries(es);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    v.measure(lc, {SpacePickerView::kWidth, 200});
    v.arrange(lc, {0, 0, SpacePickerView::kWidth, 200});

    const float y0 = v.tab_rect_for_test(5).y;
    CHECK(v.on_wheel({10, 10}, 0, 96.0f, false));
    CHECK(v.tab_rect_for_test(5).y == y0 - 96.0f);

    // Past the end: clamps so the last tab's bottom sits at the view bottom.
    v.on_wheel({10, 10}, 0, 1e6f, false);
    const tk::Rect last = v.tab_rect_for_test(v.tab_count() - 1);
    CHECK(last.y + last.h == 200.0f);
    CHECK_FALSE(v.on_wheel({10, 10}, 0, 60.0f, false));

    // Back to the top.
    v.on_wheel({10, 10}, 0, -1e6f, false);
    CHECK(v.tab_rect_for_test(0).y == 8.0f);
    CHECK(v.scroll_y() == 0.0f);
}

TEST_CASE("picker: clicks hit the right tab after scrolling", "[spacepicker]")
{
    auto surface = TestSurface::create(SpacePickerView::kWidth, 200);
    auto owner = tk::create_root_widget<SpacePickerView>(nullptr);
    SpacePickerView& v = *owner;
    std::vector<SpacePickerEntry> es;
    es.reserve(30);
    for (int i = 0; i < 30; ++i)
    {
        es.push_back(entry("!s" + std::to_string(i), "Space " + std::to_string(i)));
    }
    v.set_entries(es);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    v.measure(lc, {SpacePickerView::kWidth, 200});
    v.arrange(lc, {0, 0, SpacePickerView::kWidth, 200});

    std::string selected;
    v.on_select = [&](const std::string& id)
    {
        selected = id;
    };
    v.on_wheel({10, 10}, 0, 480.0f, false);

    // Tab 12 is now on screen; a tab scrolled above the view is not hittable.
    const tk::Point p = centre_of(v, 12);
    REQUIRE(p.y >= 0.0f);
    REQUIRE(p.y < 200.0f);
    REQUIRE(v.on_pointer_down(p));
    v.on_pointer_up(p, true);
    CHECK(selected == "!s11");

    CHECK_FALSE(v.on_pointer_down({10, v.tab_rect_for_test(0).y + 20.0f}));
}
