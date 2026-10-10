#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/tab_bar.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using namespace tk;

namespace
{

struct TabBarStage
{
    StubHost host;
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 100);
    std::unique_ptr<TabBar> bar = create_root_widget<TabBar>(&host);

    LayoutCtx lc() { return LayoutCtx{surface->factory(), Theme::light()}; }
    void layout(float w = 600)
    {
        auto l = lc();
        bar->measure(l, {w, TabBar::kHeight});
        bar->arrange(l, {0, 0, w, TabBar::kHeight});
    }
    void paint()
    {
        PaintCtx p{surface->canvas(), surface->factory(), Theme::light()};
        bar->paint(p);
    }
    void add3()
    {
        bar->add_tab("!a", "Alpha", nullptr);
        bar->add_tab("!b", "Beta", nullptr);
        bar->add_tab("!c", "Gamma", nullptr);
    }
};

KeyEvent tabbar_key(Key k)
{
    KeyEvent e;
    e.key = k;
    return e;
}

// Tab width with 3 tabs in 600px: 200. Close button spans x 180..196.
constexpr float kTabBarCloseX = 188.0f;
constexpr float kTabBarCloseY = 20.0f;

} // namespace

TEST_CASE("TabBar hides itself until there are two tabs", "[tk][tab_bar]")
{
    TabBarStage s;
    CHECK_FALSE(s.bar->visible());
    s.bar->add_tab("!a", "Alpha", nullptr);
    CHECK_FALSE(s.bar->visible());
    CHECK_FALSE(s.bar->focusable());
    s.bar->add_tab("!b", "Beta", nullptr);
    CHECK(s.bar->visible());
    CHECK(s.bar->focusable());
    CHECK(s.bar->item_count() == 2);
    CHECK(s.bar->room_id_at(1) == "!b");

    s.bar->remove_tab("!zzz"); // absent: no-op
    CHECK(s.bar->item_count() == 2);
    s.bar->remove_tab("!a");
    CHECK_FALSE(s.bar->visible());
    CHECK(s.bar->item_count() == 1);

    s.bar->clear();
    CHECK(s.bar->item_count() == 0);
    CHECK_FALSE(s.bar->visible());
}

TEST_CASE("TabBar first tab is active; remove keeps the active index sane",
          "[tk][tab_bar]")
{
    TabBarStage s;
    s.add3();
    CHECK(s.bar->access_state_for_widget_row(0).selected);

    s.bar->set_active("!c");
    CHECK(s.bar->access_state_for_widget_row(2).selected);
    s.bar->set_active("!none"); // no-op
    CHECK(s.bar->access_state_for_widget_row(2).selected);

    s.bar->remove_tab("!a"); // before the active one: index shifts
    CHECK(s.bar->access_state_for_widget_row(1).selected);
    s.bar->remove_tab("!c"); // the active (last) one: clamps
    CHECK(s.bar->item_count() == 1);
    CHECK(s.bar->access_state_for_widget_row(0).selected);
}

TEST_CASE("TabBar click selects, close button closes, drag-off cancels",
          "[tk][tab_bar]")
{
    TabBarStage s;
    s.add3();
    s.layout();
    s.paint();

    std::vector<std::string> sel, closed;
    s.bar->on_tab_selected = [&](const std::string& id) { sel.push_back(id); };
    s.bar->on_tab_closed = [&](const std::string& id) { closed.push_back(id); };

    CHECK(s.bar->on_pointer_down({250, 20})); // tab 1 body
    s.bar->on_pointer_up({250, 20}, true);
    CHECK(sel == std::vector<std::string>{"!b"});

    CHECK(s.bar->on_pointer_down({kTabBarCloseX, kTabBarCloseY})); // tab 0 close
    s.bar->on_pointer_up({kTabBarCloseX, kTabBarCloseY}, true);
    CHECK(closed == std::vector<std::string>{"!a"});
    CHECK(sel.size() == 1);

    // Press close, release elsewhere on the same tab: neither fires.
    s.bar->on_pointer_down({kTabBarCloseX, kTabBarCloseY});
    s.bar->on_pointer_up({20, 20}, true);
    CHECK(closed.size() == 1);
    CHECK(sel.size() == 1);

    // Press tab, release on another tab or outside: cancelled.
    s.bar->on_pointer_down({20, 20});
    s.bar->on_pointer_up({250, 20}, true);
    s.bar->on_pointer_down({20, 20});
    s.bar->on_pointer_up({20, 20}, false);
    CHECK(sel.size() == 1);

    CHECK(s.bar->on_pointer_down({599, 20})); // last tab body: consumed
    s.bar->on_pointer_up({599, 20}, false);
    s.bar->on_pointer_up({599, 20}, true); // no pressed tab: harmless
}

TEST_CASE("TabBar single tab has no close button", "[tk][tab_bar]")
{
    TabBarStage s;
    s.bar->add_tab("!a", "Alpha", nullptr);
    s.layout();
    std::vector<std::string> sel, closed;
    s.bar->on_tab_selected = [&](const std::string& id) { sel.push_back(id); };
    s.bar->on_tab_closed = [&](const std::string& id) { closed.push_back(id); };
    // Single tab spans the clamped max width (240); its right-edge close spot
    // acts as a plain tab click.
    s.bar->on_pointer_down({220, 20});
    s.bar->on_pointer_up({220, 20}, true);
    CHECK(closed.empty());
    CHECK(sel == std::vector<std::string>{"!a"});
    s.paint();
}

TEST_CASE("TabBar hover tracking and leave", "[tk][tab_bar]")
{
    TabBarStage s;
    s.add3();
    s.layout();
    CHECK(s.bar->on_pointer_move({250, 20}));
    CHECK(s.bar->on_pointer_move({kTabBarCloseX, kTabBarCloseY}));
    s.paint();
    s.bar->on_pointer_leave();
    s.paint();
}

TEST_CASE("TabBar wheel scrolls when tabs overflow and clamps",
          "[tk][tab_bar]")
{
    TabBarStage s;
    for (int i = 0; i < 8; ++i)
        s.bar->add_tab("!r" + std::to_string(i), "Room " + std::to_string(i),
                       nullptr);
    s.layout(400); // 8 * 120 = 960 > 400
    const Rect before = s.bar->access_rect_for_widget_row(0);
    CHECK(s.bar->on_wheel({10, 10}, 0, 1));
    const Rect after = s.bar->access_rect_for_widget_row(0);
    CHECK(after.x < before.x);
    // Horizontal delta wins when larger.
    s.bar->on_wheel({10, 10}, 100, 1);
    s.bar->on_wheel({10, 10}, -1000, 0);
    CHECK(s.bar->access_rect_for_widget_row(0).x == before.x); // back at 0

    // set_active scrolls the tab into view.
    s.bar->set_active("!r7");
    CHECK(s.bar->access_rect_for_widget_row(7).x + 120 <= 400.5f);
    s.bar->set_active("!r0");
    CHECK(s.bar->access_rect_for_widget_row(0).x == before.x);

    s.bar->update_tab("!r0", "Renamed", nullptr);
    CHECK(s.bar->access_name_for_widget_row(0) == "Renamed");
    s.bar->update_tab("!nope", "x", nullptr);
    s.layout(400);
    s.paint();
}

TEST_CASE("TabBar Left/Right keys cycle via on_tab_selected only when focused",
          "[tk][tab_bar]")
{
    TabBarStage s;
    s.add3();
    s.layout();
    std::vector<std::string> sel;
    s.bar->on_tab_selected = [&](const std::string& id) { sel.push_back(id); };

    CHECK_FALSE(s.bar->on_key_down(tabbar_key(Key::Right))); // unfocused
    s.host.request_focus(s.bar.get());
    REQUIRE(s.bar->has_focus());
    CHECK(s.bar->on_key_down(tabbar_key(Key::Right)));
    CHECK(s.bar->on_key_down(tabbar_key(Key::Left)));
    CHECK(sel == std::vector<std::string>{"!b", "!c"}); // wraps 0 -> 2
    CHECK_FALSE(s.bar->on_key_down(tabbar_key(Key::Up)));
    KeyEvent ctrl = tabbar_key(Key::Right);
    ctrl.ctrl = true;
    CHECK_FALSE(s.bar->on_key_down(ctrl));
}

TEST_CASE("TabBar accessibility rows", "[tk][tab_bar][accessibility]")
{
    TabBarStage s;
    s.add3();
    s.layout();
    CHECK(s.bar->access_role() == Role::TabList);
    CHECK(s.bar->access_row_count() == 3);
    CHECK(s.bar->access_role_for_widget_row(0) == Role::Tab);
    CHECK(s.bar->access_name_for_widget_row(9).empty());
    CHECK(s.bar->access_rect_for_widget_row(9).w == 0);
    CHECK(s.bar->access_rect_for_widget_row(1).w == 200);

    CHECK_FALSE(s.bar->access_activate_widget_row(0)); // no handler
    std::string sel;
    s.bar->on_tab_selected = [&](const std::string& id) { sel = id; };
    CHECK(s.bar->access_activate_widget_row(2));
    CHECK(sel == "!c");
    CHECK_FALSE(s.bar->access_activate_widget_row(7));
}
