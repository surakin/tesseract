#include <catch2/catch_test_macros.hpp>

#include "tk/controls.h"
#include "tk/list_view.h"
#include "tk/scrollable_base.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

// Keyboard-operability framework (a11y Phase 5): focus scoping for
// registered popups and modal dialogs, the context-menu key, keyboard
// paging of scrollable regions, ListView Home/End/PageUp/PageDown, and
// focus tooltips for icon-only buttons.

using namespace tk;

namespace kbd_nav_test
{

class Box : public Widget
{
public:
    explicit Box(Rect r, bool focusable = false) : focusable_(focusable)
    {
        bounds_ = r;
    }
    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void arrange(LayoutCtx&, Rect) override
    {
    }
    void paint(PaintCtx&) override
    {
    }
    bool focusable() const override
    {
        return focusable_;
    }
    bool on_right_click(Point local) override
    {
        ++right_clicks;
        last_right_click = local;
        return counts_right_clicks;
    }

    bool counts_right_clicks = false;
    int right_clicks = 0;
    Point last_right_click{};

private:
    bool focusable_;
};

class ModalBox : public Box
{
public:
    using Box::Box;
    bool access_modal() const override
    {
        return visible();
    }
};

class ScopedPopup : public Box
{
public:
    using Box::Box;
    bool popup_scopes_focus() const override
    {
        return scopes;
    }
    void on_popup_dismiss() override
    {
        ++dismissed;
    }
    bool scopes = true;
    int dismissed = 0;
};

// A scrollable region with a fixed content height; scrolls through
// on_wheel exactly like the real subclasses do.
class Scroller : public ScrollableBase
{
public:
    explicit Scroller(Rect r)
    {
        bounds_ = r;
    }
    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void arrange(LayoutCtx&, Rect) override
    {
    }
    void paint(PaintCtx&) override
    {
    }
    bool on_wheel(Point, float, float dy, bool is_touchpad) override
    {
        return on_wheel_scroll(dy, is_touchpad);
    }
    float content_height() const override
    {
        return 1000.0f;
    }
};

class RowsAdapter : public ListAdapter
{
public:
    std::size_t count() const override
    {
        return 50;
    }
    float measure_row_height(std::size_t, LayoutCtx&, float) override
    {
        return 20;
    }
    void paint_row(std::size_t, PaintCtx&, Rect, bool, bool) override
    {
    }
    bool is_selectable(std::size_t i) const override
    {
        return i != 0;
    }
};

} // namespace kbd_nav_test

using namespace kbd_nav_test;

TEST_CASE("Tab stays inside a registered popup that scopes focus", "[tk][host][focus][keyboard]")
{
    Box root({0, 0, 400, 400});
    auto* outside = root.add_child(create_widget<Box>(&root, Rect{0, 0, 50, 20}, true));
    ScopedPopup popup({100, 100, 200, 200});
    auto* a = popup.add_child(create_widget<Box>(&popup, Rect{110, 110, 50, 20}, true));
    auto* b = popup.add_child(create_widget<Box>(&popup, Rect{110, 140, 50, 20}, true));

    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == a);
    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == b);
    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == a);
    CHECK(popup.dismissed == 0);
    CHECK(host.focused_widget() != outside);
}

TEST_CASE("Tab leaves (and dismisses) a popup that doesn't scope focus",
          "[tk][host][focus][keyboard]")
{
    Box root({0, 0, 400, 400});
    auto* outside = root.add_child(create_widget<Box>(&root, Rect{0, 0, 50, 20}, true));
    ScopedPopup popup({100, 100, 200, 200});
    popup.scopes = false;
    popup.add_child(create_widget<Box>(&popup, Rect{110, 110, 50, 20}, true));

    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == outside);
    CHECK(popup.dismissed == 1);
}

TEST_CASE("Tab traversal is confined to an open modal dialog", "[tk][host][focus][keyboard]")
{
    Box root({0, 0, 400, 400});
    root.add_child(create_widget<Box>(&root, Rect{0, 0, 50, 20}, true));
    auto* modal = root.add_child(create_widget<ModalBox>(&root, Rect{100, 100, 200, 200}));
    auto* ok = modal->add_child(create_widget<Box>(modal, Rect{110, 110, 50, 20}, true));
    auto* cancel = modal->add_child(create_widget<Box>(modal, Rect{170, 110, 50, 20}, true));

    TestHost host(&root);
    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == ok);
    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == cancel);
    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == ok);

    // Closing the modal frees traversal again.
    modal->set_visible(false);
    host.dispatch_key_down({Key::Backtab});
    CHECK(host.focused_widget() != ok);
    CHECK(host.focused_widget() != cancel);
}

TEST_CASE("An explicit focus scope wins over a modal underneath it", "[tk][host][focus][keyboard]")
{
    // e.g. the Quick Switcher (scoped by MainAppWidget) opened over the
    // encryption dialog (a modal): Tab must stay in the switcher.
    Box root({0, 0, 400, 400});
    auto* modal = root.add_child(create_widget<ModalBox>(&root, Rect{150, 100, 200, 200}));
    modal->add_child(create_widget<Box>(modal, Rect{160, 110, 50, 20}, true));
    auto* overlay = root.add_child(create_widget<Box>(&root, Rect{0, 0, 400, 400}));
    auto* search = overlay->add_child(create_widget<Box>(overlay, Rect{10, 10, 200, 20}, true));

    TestHost host(&root);
    host.set_focus_scope(overlay);
    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == search);
    host.dispatch_key_down({Key::Tab});
    CHECK(host.focused_widget() == search);
}

TEST_CASE("The context-menu key right-clicks the focused widget",
          "[tk][host][keyboard][context-menu]")
{
    Box root({0, 0, 400, 400});
    auto* target = root.add_child(create_widget<Box>(&root, Rect{100, 100, 40, 20}, true));
    target->counts_right_clicks = true;

    TestHost host(&root);
    host.request_focus(target);

    SECTION("Menu key")
    {
        CHECK(host.dispatch_key_down({Key::Menu}));
        CHECK(target->right_clicks == 1);
        // Centre of the anchor rect, in the widget's local coordinates.
        CHECK(target->last_right_click.x == 20.0f);
        CHECK(target->last_right_click.y == 10.0f);
    }
    SECTION("Shift+F10")
    {
        KeyEvent e{Key::F10};
        e.shift = true;
        CHECK(host.dispatch_key_down(e));
        CHECK(target->right_clicks == 1);
    }
    SECTION("plain F10 is not a context-menu chord")
    {
        host.dispatch_key_down({Key::F10});
        CHECK(target->right_clicks == 0);
    }
}

TEST_CASE("Paging keys scroll the scrollable region around the focused widget",
          "[tk][host][keyboard][scroll]")
{
    Scroller region({0, 0, 200, 200});
    auto* button = region.add_child(create_widget<Box>(&region, Rect{10, 10, 50, 20}, true));

    TestHost host(&region);
    host.request_focus(button);

    CHECK(host.dispatch_key_down({Key::PageDown}));
    CHECK(region.scroll_y() == 160.0f); // viewport minus the 40px overlap
    CHECK(host.dispatch_key_down({Key::End}));
    CHECK(region.scroll_y() == 800.0f); // content 1000 - viewport 200
    CHECK(host.dispatch_key_down({Key::PageUp}));
    CHECK(region.scroll_y() == 640.0f);
    CHECK(host.dispatch_key_down({Key::Home}));
    CHECK(region.scroll_y() == 0.0f);
}

TEST_CASE("ListView Home/End/PageDown move the selection", "[tk][list_view][keyboard]")
{
    auto surface = TestSurface::create(300, 200);
    LayoutCtx lc{surface->factory(), Theme::light()};
    RowsAdapter ad;
    auto list = create_root_widget<ListView>(nullptr);
    list->set_adapter(&ad);
    list->measure(lc, {300, 200});
    list->arrange(lc, {0, 0, 300, 200});

    TestHost host(list.get());
    host.request_focus(list.get());

    CHECK(list->on_key_down({Key::End}));
    CHECK(list->selected_index() == 49);
    CHECK(list->on_key_down({Key::Home}));
    CHECK(list->selected_index() == 1); // row 0 isn't selectable
    CHECK(list->on_key_down({Key::PageDown}));
    CHECK(list->selected_index() > 5);
}

TEST_CASE("Tab onto an icon-only button shows its name as a tooltip",
          "[tk][host][keyboard][tooltip]")
{
    auto surface = TestSurface::create(100, 100);
    LayoutCtx lc{surface->factory(), Theme::light()};
    Box root({0, 0, 400, 400});
    auto* icon = root.add_child(
        create_widget<Button>(&root, "", std::function<void()>{}, Button::Variant::Icon));
    icon->set_accessible_name("Jump to Date");
    icon->arrange(lc, {10, 10, 28, 28});

    TestHost host(&root);
    host.dispatch_key_down({Key::Tab});
    REQUIRE(host.focused_widget() == icon);
    CHECK(host.tooltip_owner_ == icon);
    CHECK(host.tooltip_text_ == "Jump to Date");

    // Any other key dismisses it.
    host.dispatch_key_down({Key::Enter});
    CHECK(host.tooltip_owner_ == nullptr);
}

#include "views/DatePickerView.h"

using tesseract::views::DatePickerView;

namespace kbd_nav_test
{
// Opens a picker whose max date is far in the future so keyboard moves from
// "today" are never clamped by it.
std::unique_ptr<DatePickerView> open_test_picker(TestSurface& surface)
{
    auto picker = std::make_unique<DatePickerView>();
    picker->set_max_date(2100, 12, 31);
    picker->open_at({0, 0, DatePickerView::kWidth, DatePickerView::kHeight});
    PaintCtx pc{surface.canvas(), surface.factory(), Theme::light()};
    picker->paint_overlay(pc); // builds the cells
    return picker;
}
} // namespace kbd_nav_test

TEST_CASE("DatePickerView: Tab cycles its parts instead of escaping", "[datepicker][keyboard]")
{
    auto surface = TestSurface::create(400, 400);
    auto picker = open_test_picker(*surface);
    using P = DatePickerView::FocusPart;

    REQUIRE(picker->focus_part() == P::Grid);
    // First Tab only reveals the cursor; it's still consumed (so the Host
    // never moves focus out and dismisses the popup).
    CHECK(picker->on_key_down({Key::Tab}));
    CHECK(picker->focus_part() == P::Grid);
    CHECK(picker->on_key_down({Key::Tab}));
    CHECK(picker->focus_part() == P::TodayBtn);
    CHECK(picker->on_key_down({Key::Tab}));
    CHECK(picker->focus_part() == P::PrevBtn);
    CHECK(picker->on_key_down({Key::Tab}));
    CHECK(picker->focus_part() == P::Month);
    CHECK(picker->on_key_down({Key::Tab}));
    CHECK(picker->focus_part() == P::Year);
    CHECK(picker->on_key_down({Key::Backtab}));
    CHECK(picker->focus_part() == P::Month);
}

TEST_CASE("DatePickerView: the year field steps the year by keyboard", "[datepicker][keyboard]")
{
    auto surface = TestSurface::create(400, 400);
    auto picker = open_test_picker(*surface);
    using P = DatePickerView::FocusPart;

    const int start_year = picker->view_year();
    picker->on_key_down({Key::Tab}); // reveal
    while (picker->focus_part() != P::Year)
    {
        picker->on_key_down({Key::Tab});
    }
    CHECK(picker->on_key_down({Key::Up}));
    CHECK(picker->view_year() == start_year + 1);
    CHECK(picker->on_key_down({Key::Down}));
    CHECK(picker->on_key_down({Key::Down}));
    CHECK(picker->view_year() == start_year - 1);

    // Shift+PageUp/PageDown step the year from anywhere.
    KeyEvent shift_pgdn{Key::PageDown};
    shift_pgdn.shift = true;
    picker->on_key_down(shift_pgdn);
    CHECK(picker->view_year() == start_year);
}

TEST_CASE("DatePickerView: grid arrows cross into the previous month", "[datepicker][keyboard]")
{
    auto surface = TestSurface::create(400, 400);
    auto picker = open_test_picker(*surface);

    picker->on_key_down({Key::Left}); // reveal
    const int month = picker->cursor_month();
    const int day = picker->cursor_day();
    for (int i = 0; i < day; ++i)
    {
        picker->on_key_down({Key::Left});
    }
    CHECK(picker->cursor_month() != month);
    CHECK(picker->view_month() == picker->cursor_month());
    CHECK(picker->cursor_day() >= 28);
}

TEST_CASE("DatePickerView: Enter picks the keyboard cursor date", "[datepicker][keyboard]")
{
    auto surface = TestSurface::create(400, 400);
    auto picker = open_test_picker(*surface);
    int py = 0, pm = 0, pd = 0;
    picker->on_date_picked = [&](int y, int m, int d)
    {
        py = y;
        pm = m;
        pd = d;
    };

    picker->on_key_down({Key::Enter}); // first press only reveals
    CHECK(pd == 0);
    picker->on_key_down({Key::Right});
    picker->on_key_down({Key::Enter});
    CHECK(py == picker->cursor_year());
    CHECK(pm == picker->cursor_month());
    CHECK(pd == picker->cursor_day());
}

TEST_CASE("DatePickerView: the cursor never passes the max date", "[datepicker][keyboard]")
{
    auto surface = TestSurface::create(400, 400);
    auto picker = std::make_unique<DatePickerView>();
    int ty, tm, td;
    DatePickerView::today(ty, tm, td);
    picker->set_max_date(ty, tm, td);
    picker->open_at({0, 0, DatePickerView::kWidth, DatePickerView::kHeight});

    picker->on_key_down({Key::Right}); // reveal
    picker->on_key_down({Key::Right});
    picker->on_key_down({Key::PageDown});
    CHECK(picker->cursor_year() == ty);
    CHECK(picker->cursor_month() == tm);
    CHECK(picker->cursor_day() == td);
}

#include "tk/keyboard_target.h"

TEST_CASE("KeyboardTarget activates on Enter and is pointer-transparent",
          "[tk][keyboard][keyboard_target]")
{
    Box root({0, 0, 400, 400});
    root.counts_right_clicks = true;
    auto* t = root.add_child(create_widget<KeyboardTarget>(&root));
    int activations = 0;
    t->on_activate = [&]
    {
        ++activations;
    };
    t->set_target_rect({10, 10, 100, 30});

    TestHost host(&root);
    host.dispatch_key_down({Key::Tab});
    REQUIRE(host.focused_widget() == t);
    host.dispatch_key_down({Key::Enter});
    CHECK(activations == 1);

    // A click on it reaches the owner, never the target.
    CHECK(root.hit_test({20, 20}) != t);
    // The context-menu key falls through to the owner's right-click.
    host.dispatch_key_down({Key::Menu});
    CHECK(root.right_clicks == 1);
}

TEST_CASE("KeyboardTarget cycles links with Left/Right and opens with Enter",
          "[tk][keyboard][keyboard_target]")
{
    // Built under the host so host() is set — the tooltip needs it.
    TestHost host(nullptr);
    auto root_owner = create_root_widget<Box>(&host, Rect{0, 0, 400, 400});
    Box& root = *root_owner;
    host.set_root(&root);
    auto* t = root.add_child(create_widget<KeyboardTarget>(&root));
    std::string opened;
    t->on_link_activated = [&](const std::string& url)
    {
        opened = url;
    };
    t->set_links({{"one", "https://one.example"}, {"two", "https://two.example"}});
    t->set_target_rect({10, 10, 100, 30});

    host.request_focus(t); // links alone make it focusable
    REQUIRE(host.focused_widget() == t);

    host.dispatch_key_down({Key::Right});
    host.dispatch_key_down({Key::Right});
    CHECK(t->link_index() == 1);
    CHECK(host.tooltip_owner_ == t);
    host.dispatch_key_down({Key::Enter});
    CHECK(opened == "https://two.example");

    host.dispatch_key_down({Key::Escape});
    CHECK(t->link_index() == -1);
}

TEST_CASE("KeyboardTarget with nothing to do is not a Tab stop", "[tk][keyboard][keyboard_target]")
{
    Box root({0, 0, 400, 400});
    auto* t = root.add_child(create_widget<KeyboardTarget>(&root));
    t->set_target_rect({10, 10, 100, 30});
    CHECK_FALSE(t->focusable());
    t->on_activate = [] {};
    CHECK(t->focusable());
    t->set_target_rect({});
    CHECK_FALSE(t->focusable());
}

#include "views/MessageListView.h"

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace kbd_nav_test
{
MessageRowData kbd_text_row(int i)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = "$e" + std::to_string(i);
    r.sender = "@user:example.org";
    r.sender_name = "User";
    r.body = "row " + std::to_string(i);
    return r;
}

struct TimelineStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 200);
    void run(Widget& root, Rect bounds)
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        root.paint(pc);
    }
};
} // namespace kbd_nav_test

TEST_CASE("Timeline: focus lands on the newest visible message; Up/Down move",
          "[message_list][keyboard]")
{
    TimelineStage st;
    MessageListView v;
    std::vector<MessageRowData> rows;
    for (int i = 0; i < 20; ++i)
    {
        rows.push_back(kbd_text_row(i));
    }
    v.set_messages(std::move(rows), /*room_switch=*/true);
    st.run(v, {0, 0, 320, 200});

    TestHost host(&v);
    REQUIRE(v.focusable());
    host.request_focus(&v);
    CHECK(v.selected_index() == 19);
    CHECK(v.on_key_down({Key::Up}));
    CHECK(v.selected_index() == 18);
    CHECK(v.on_key_down({Key::Home}));
    CHECK(v.selected_index() == 0);
    CHECK(v.on_key_down({Key::End}));
    CHECK(v.selected_index() == 19);
}

TEST_CASE("Timeline: Shift+PageUp pages up when nothing is unread",
          "[message_list][keyboard][unread]")
{
    TimelineStage st;
    MessageListView v;
    std::vector<MessageRowData> rows;
    for (int i = 0; i < 20; ++i)
    {
        rows.push_back(kbd_text_row(i));
    }
    v.set_messages(std::move(rows), /*room_switch=*/true);
    st.run(v, {0, 0, 320, 200});

    TestHost host(&v);
    host.request_focus(&v);
    REQUIRE(v.selected_index() == 19);
    KeyEvent e{Key::PageUp};
    e.shift = true;
    CHECK(v.on_key_down(e));
    CHECK(v.selected_index() < 19);
}

TEST_CASE("Timeline: Left/Right reach a message's actions and Enter fires them",
          "[message_list][keyboard]")
{
    TimelineStage st;
    MessageListView v;
    std::vector<MessageRowData> rows;
    for (int i = 0; i < 5; ++i)
    {
        rows.push_back(kbd_text_row(i));
    }
    v.set_messages(std::move(rows), true);
    st.run(v, {0, 0, 320, 200});

    std::string replied_to;
    v.on_reply_requested = [&](const std::string& ev, const std::string&, const std::string&)
    {
        replied_to = ev;
    };
    std::string more_for;
    Rect more_anchor{};
    v.on_actions_menu_requested = [&](const MessageListView::MessageMenuRequest& req)
    {
        more_for = req.event_id;
        more_anchor = req.anchor;
    };
    int exits = 0;
    v.on_keyboard_exit = [&]
    {
        ++exits;
    };

    TestHost host(&v);
    host.request_focus(&v);
    REQUIRE(v.selected_index() == 4);

    const auto names = v.keyboard_part_names();
    int reply_at = -1;
    for (int i = 0; i < static_cast<int>(names.size()); ++i)
    {
        if (names[static_cast<std::size_t>(i)] == "Reply")
        {
            reply_at = i;
        }
    }
    REQUIRE(reply_at >= 0);
    for (int i = 0; i <= reply_at; ++i)
    {
        v.on_key_down({Key::Right});
    }
    CHECK(v.keyboard_part_index() == reply_at);
    v.on_key_down({Key::Enter});
    CHECK(replied_to == "$e4");

    // Escape drops the chosen part first, then leaves the timeline.
    v.on_key_down({Key::Escape});
    CHECK(v.keyboard_part_index() == -1);
    CHECK(exits == 0);
    v.on_key_down({Key::Escape});
    CHECK(exits == 1);

    // Enter with no part chosen, and the context-menu key, open More.
    v.on_key_down({Key::Enter});
    CHECK(more_for == "$e4");
    CHECK(more_anchor.h > 0.0f);
    more_for.clear();
    CHECK(host.dispatch_key_down({Key::Menu}));
    CHECK(more_for == "$e4");
}

TEST_CASE("Timeline: a click never moves keyboard focus onto it", "[message_list][keyboard]")
{
    TimelineStage st;
    MessageListView v;
    std::vector<MessageRowData> rows;
    for (int i = 0; i < 5; ++i)
    {
        rows.push_back(kbd_text_row(i));
    }
    v.set_messages(std::move(rows), true);
    st.run(v, {0, 0, 320, 200});

    TestHost host(&v);
    host.dispatch_pointer_down({100, 150});
    host.dispatch_pointer_up({100, 150});
    CHECK(host.focused_widget() != &v);
}

TEST_CASE("Timeline: the cursor follows its message across a history prepend",
          "[message_list][keyboard]")
{
    TimelineStage st;
    MessageListView v;
    std::vector<MessageRowData> rows;
    for (int i = 10; i < 15; ++i)
    {
        rows.push_back(kbd_text_row(i));
    }
    v.set_messages(std::move(rows), true);
    st.run(v, {0, 0, 320, 200});

    std::string more_for;
    v.on_actions_menu_requested = [&](const MessageListView::MessageMenuRequest& req)
    {
        more_for = req.event_id;
    };

    TestHost host(&v);
    host.request_focus(&v);
    REQUIRE(v.selected_index() == 4); // $e14

    std::vector<MessageRowData> older;
    for (int i = 0; i < 10; ++i)
    {
        older.push_back(kbd_text_row(i));
    }
    v.prepend_messages(std::move(older));
    st.run(v, {0, 0, 320, 200});

    v.on_key_down({Key::Enter});
    CHECK(more_for == "$e14");
}

TEST_CASE("Timeline: a part added before the chosen one doesn't redirect Enter",
          "[message_list][keyboard]")
{
    TimelineStage st;
    MessageListView v;
    std::vector<MessageRowData> rows;
    for (int i = 0; i < 3; ++i)
    {
        rows.push_back(kbd_text_row(i));
    }
    v.set_messages(rows, true);
    st.run(v, {0, 0, 320, 200});

    std::string replied_to;
    int toggles = 0;
    v.on_reply_requested = [&](const std::string& ev, const std::string&, const std::string&)
    {
        replied_to = ev;
    };
    v.on_reaction_toggled = [&](const std::string&, const std::string&, const std::string&)
    {
        ++toggles;
    };

    TestHost host(&v);
    host.request_focus(&v);
    const auto names = v.keyboard_part_names();
    int reply_at = -1;
    for (int i = 0; i < static_cast<int>(names.size()); ++i)
    {
        if (names[static_cast<std::size_t>(i)] == "Reply")
        {
            reply_at = i;
        }
    }
    REQUIRE(reply_at >= 0);
    for (int i = 0; i <= reply_at; ++i)
    {
        v.on_key_down({Key::Right});
    }

    // A reaction arrives on the cursor message: its chip precedes Reply.
    tesseract::Reaction r;
    r.key = "\U0001F44D";
    r.count = 1;
    rows[2].reactions = {r};
    v.set_messages(rows, false);
    st.run(v, {0, 0, 320, 200});

    v.on_key_down({Key::Enter});
    CHECK(replied_to == "$e2");
    CHECK(toggles == 0);
}

TEST_CASE("The context-menu key scrolls an off-screen selected row back and "
          "right-clicks that row",
          "[tk][list_view][keyboard][context-menu]")
{
    auto surface = TestSurface::create(300, 200);
    LayoutCtx lc{surface->factory(), Theme::light()};
    Box parent({0, 0, 300, 200});
    parent.counts_right_clicks = true;
    RowsAdapter ad;
    auto* list = parent.add_child(create_widget<ListView>(&parent));
    list->set_adapter(&ad);
    list->measure(lc, {300, 200});
    list->arrange(lc, {0, 0, 300, 200});

    TestHost host(&parent);
    host.request_focus(list);
    list->set_selected_index(40); // far below the 200px viewport
    REQUIRE(list->row_world_rect(40).y > 200.0f);

    CHECK(host.dispatch_key_down({Key::Menu}));
    CHECK(parent.right_clicks == 1);
    const Rect row = list->row_world_rect(40);
    CHECK(row.y >= 0.0f);
    CHECK(row.y + row.h <= 200.0f + 0.5f);
    CHECK(parent.last_right_click.y >= row.y);
    CHECK(parent.last_right_click.y <= row.y + row.h);
}

TEST_CASE("Keyboard movement to the top of a list fires the near-top hook",
          "[tk][list_view][keyboard]")
{
    auto surface = TestSurface::create(300, 200);
    LayoutCtx lc{surface->factory(), Theme::light()};
    RowsAdapter ad;
    auto list = create_root_widget<ListView>(nullptr);
    list->set_adapter(&ad);
    list->measure(lc, {300, 200});
    list->arrange(lc, {0, 0, 300, 200});
    int near_top = 0;
    list->on_near_top = [&]
    {
        ++near_top;
    };

    TestHost host(list.get());
    host.request_focus(list.get());
    list->on_key_down({Key::End});
    list->on_key_down({Key::Home});
    CHECK(near_top >= 1);
}

TEST_CASE("DatePickerView: a past max date caps the shown month and cursor", "[tk][datepicker]")
{
    auto picker = std::make_unique<DatePickerView>();
    picker->set_max_date(2020, 3, 14);
    picker->open_at({0, 0, DatePickerView::kWidth, DatePickerView::kHeight});
    CHECK(picker->view_year() == 2020);
    CHECK(picker->view_month() == 3);
    CHECK(picker->cursor_year() == 2020);
    CHECK(picker->cursor_month() == 3);
    CHECK(picker->cursor_day() == 14);

    // A cap that moves earlier while open re-clamps the view and cursor.
    picker->set_max_date(2019, 11, 2);
    CHECK(picker->view_year() == 2019);
    CHECK(picker->view_month() == 11);
    CHECK(picker->cursor_day() == 2);
}

TEST_CASE("DatePickerView: reset_view reopens on a later max date", "[tk][datepicker]")
{
    auto picker = std::make_unique<DatePickerView>();
    picker->set_max_date(2019, 11, 2);
    picker->open_at({0, 0, DatePickerView::kWidth, DatePickerView::kHeight});
    REQUIRE(picker->view_month() == 11);

    // Without a reset, a later cap leaves the earlier month showing.
    picker->set_max_date(2021, 6, 20);
    picker->open_at({0, 0, DatePickerView::kWidth, DatePickerView::kHeight});
    CHECK(picker->view_year() == 2019);

    picker->reset_view();
    picker->open_at({0, 0, DatePickerView::kWidth, DatePickerView::kHeight});
    CHECK(picker->view_year() == 2021);
    CHECK(picker->view_month() == 6);
    CHECK(picker->cursor_year() == 2021);
    CHECK(picker->cursor_month() == 6);
    CHECK(picker->cursor_day() == 20);
}
