#include <catch2/catch_test_macros.hpp>

#include "tk/controls.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>

// The Widget hover/click/cursor feature (Widget::set_hoverable,
// set_on_click, cursor()) and the Host plumbing that drives it
// (hover_target_for_ / begin_/end_hover_feedback_).

using namespace tk;

namespace
{

class HvBox : public Widget
{
public:
    explicit HvBox(Rect r)
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
};

// A view that writes its own "link" cursor while handling pointer-move,
// the way the message list / header do via on_link_hovered.
class HvLinkView : public HvBox
{
public:
    using HvBox::HvBox;
    bool on_pointer_move(Point) override
    {
        if (host_log)
        {
            host_log->push_back(CursorShape::Text);
        }
        return false;
    }
    std::vector<CursorShape>* host_log = nullptr;
};

} // namespace

TEST_CASE("A hoverable container is hovered while the pointer is over a plain child",
          "[tk][hoverable]")
{
    HvBox root({0, 0, 400, 400});
    auto* area = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 200, 40}));
    area->set_hoverable();
    auto* label = area->add_child(create_widget<HvBox>(area, Rect{20, 15, 80, 20}));
    (void)label;
    TestHost host(&root);

    host.dispatch_pointer_move({30, 20}); // over the child label
    CHECK(area->hovered());
    host.dispatch_pointer_move({300, 300});
    CHECK_FALSE(area->hovered());
}

TEST_CASE("Hover goes to the nearest hoverable, not its hoverable ancestor", "[tk][hoverable]")
{
    HvBox root({0, 0, 400, 400});
    auto* outer = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 200, 60}));
    outer->set_hoverable();
    auto* inner = outer->add_child(create_widget<HvBox>(outer, Rect{20, 20, 50, 30}));
    inner->set_hoverable();
    TestHost host(&root);

    host.dispatch_pointer_move({30, 30});
    CHECK(inner->hovered());
    CHECK_FALSE(outer->hovered());
    host.dispatch_pointer_move({150, 30}); // inside outer only
    CHECK(outer->hovered());
    CHECK_FALSE(inner->hovered());
}

TEST_CASE("A disabled widget shows no hover and hides its hoverable ancestor", "[tk][hoverable]")
{
    HvBox root({0, 0, 400, 400});
    auto* outer = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 200, 60}));
    outer->set_hoverable();
    auto* inner = outer->add_child(create_widget<HvBox>(outer, Rect{20, 20, 50, 30}));
    inner->set_hoverable();
    inner->set_enabled(false);
    TestHost host(&root);

    host.dispatch_pointer_move({30, 30});
    CHECK_FALSE(inner->hovered());
    CHECK_FALSE(outer->hovered()); // disabled widgets are opaque to input
}

TEST_CASE("Hoverable widgets inside the open popup are hover-tracked", "[tk][hoverable][popup]")
{
    HvBox root({0, 0, 400, 400});
    auto* behind = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    behind->set_hoverable();
    HvBox popup({100, 100, 100, 100});
    popup.set_hoverable(true);
    auto* item = popup.add_child(create_widget<HvBox>(&popup, Rect{110, 110, 60, 20}));
    item->set_hoverable();
    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_pointer_move({20, 20}); // behind the popup, outside it
    CHECK(behind->hovered());
    host.dispatch_pointer_move({120, 120}); // inside the popup, over item
    CHECK(item->hovered());
    CHECK_FALSE(behind->hovered());
}

TEST_CASE("A plain widget with set_on_click claims presses and fires on release",
          "[tk][hoverable][click]")
{
    HvBox root({0, 0, 400, 400});
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    int clicks = 0;
    w->set_hoverable().set_on_click(
        [&]
        {
            ++clicks;
        });
    TestHost host(&root);

    host.dispatch_pointer_down({20, 20});
    CHECK(w->pressed());
    host.dispatch_pointer_up({20, 20});
    CHECK_FALSE(w->pressed());
    CHECK(clicks == 1);

    host.dispatch_pointer_down({20, 20});
    host.dispatch_pointer_up({300, 300}); // released outside
    CHECK(clicks == 1);
}

TEST_CASE("A hoverable widget with no click handler declines presses", "[tk][hoverable][click]")
{
    HvBox root({0, 0, 400, 400});
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    w->set_hoverable();
    TestHost host(&root);

    host.dispatch_pointer_down({20, 20});
    CHECK_FALSE(w->pressed()); // the press fell through to ancestors
    CHECK(host.pressed_widget_.expired());
}

TEST_CASE("A clickable plain widget is focusable and activates on Enter/Space",
          "[tk][hoverable][keyboard]")
{
    HvBox root({0, 0, 400, 400});
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    int clicks = 0;
    w->set_on_click(
        [&]
        {
            ++clicks;
        });
    TestHost host(&root);

    CHECK(w->focusable());
    CHECK(w->access_role() == Role::Button);

    host.dispatch_key_down({Key::Enter}); // nothing focused: must not fire
    CHECK(clicks == 0);
    host.dispatch_key_down({Key::Tab});
    REQUIRE(host.focused_widget() == w);
    host.dispatch_key_down({Key::Space});
    CHECK(clicks == 1);
    CHECK(w->access_default_action());
    CHECK(clicks == 2);
}

TEST_CASE("A widget that opts into nothing is unaffected", "[tk][hoverable]")
{
    HvBox root({0, 0, 400, 400});
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    TestHost host(&root);

    CHECK_FALSE(w->focusable());
    CHECK(w->access_role() == Role::None);
    CHECK(w->cursor() == CursorShape::Default);
    host.dispatch_pointer_move({20, 20});
    CHECK_FALSE(w->hovered());
    host.dispatch_pointer_down({20, 20});
    CHECK_FALSE(w->pressed());
}

// ── Cursor ──────────────────────────────────────────────────────────────

TEST_CASE("Hovering a clickable widget asks the host for the pointer cursor",
          "[tk][hoverable][cursor]")
{
    HvBox root({0, 0, 400, 400});
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    w->set_hoverable().set_on_click([] {});
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    CHECK(host.last_cursor() == CursorShape::Pointer);
    host.dispatch_pointer_move({25, 22}); // same target: no repeat write
    CHECK(host.cursor_log.size() == 1);
    host.dispatch_pointer_move({300, 300});
    CHECK(host.last_cursor() == CursorShape::Default); // released
    CHECK(host.cursor_log.size() == 2);
}

TEST_CASE("Every Button gets the pointer cursor, a disabled one does not",
          "[tk][hoverable][cursor][button]")
{
    auto surface = TestSurface::create(100, 100);
    LayoutCtx lc{surface->factory(), Theme::light()};
    HvBox root({0, 0, 400, 400});
    auto* b = root.add_child(
        create_widget<Button>(&root, "B", std::function<void()>{}, Button::Variant::Subtle));
    b->arrange(lc, {10, 10, 50, 30});
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    CHECK(host.last_cursor() == CursorShape::Pointer);
    host.dispatch_pointer_move({300, 300});
    b->set_enabled(false);
    host.cursor_log.clear();
    host.dispatch_pointer_move({20, 20});
    CHECK(host.cursor_log.empty()); // nothing to claim
}

TEST_CASE("set_cursor overrides the default shape; Default-cursor hover writes nothing",
          "[tk][hoverable][cursor]")
{
    HvBox root({0, 0, 400, 400});
    auto* grab = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    grab->set_hoverable().set_cursor(CursorShape::Grab);
    auto* fill_only = root.add_child(create_widget<HvBox>(&root, Rect{100, 10, 50, 30}));
    fill_only->set_hoverable(); // not clickable → Default cursor
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    CHECK(host.last_cursor() == CursorShape::Grab);
    host.dispatch_pointer_move({110, 20});
    CHECK(fill_only->hovered());
    CHECK(host.last_cursor() == CursorShape::Default); // released by grab→fill_only
}

TEST_CASE("Surface leave releases the cursor and clears hover", "[tk][hoverable][cursor]")
{
    HvBox root({0, 0, 400, 400});
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    w->set_hoverable().set_on_click([] {});
    TestHost host(&root);

    host.dispatch_pointer_move({20, 20});
    REQUIRE(host.last_cursor() == CursorShape::Pointer);
    host.dispatch_pointer_leave();
    CHECK(host.last_cursor() == CursorShape::Default);
    CHECK_FALSE(w->hovered());
}

TEST_CASE("Releasing happens before the move dispatch so a view's own cursor wins",
          "[tk][hoverable][cursor]")
{
    // The hoverable region sits next to a "link" view. Moving from the
    // region onto the view releases our Pointer BEFORE the view's
    // on_pointer_move runs (which, in the shells, writes the link cursor).
    HvBox root({0, 0, 400, 400});
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    w->set_hoverable().set_on_click([] {});
    auto* view = root.add_child(create_widget<HvLinkView>(&root, Rect{100, 10, 50, 30}));
    TestHost host(&root);
    std::vector<CursorShape> order;
    view->host_log = &order;

    host.dispatch_pointer_move({20, 20});
    REQUIRE(host.last_cursor() == CursorShape::Pointer);
    const auto before = host.cursor_log.size();
    host.dispatch_pointer_move({110, 20});

    REQUIRE(host.cursor_log.size() == before + 1);
    CHECK(host.last_cursor() == CursorShape::Default); // release written…
    CHECK(order.size() == 1);                          // …and the view then ran
}

TEST_CASE("Claiming happens after the previous widget's on_pointer_leave",
          "[tk][hoverable][cursor]")
{
    // Moving from a link-ish view onto a clickable widget: the view's
    // on_pointer_leave (which clears its cursor in the shells) runs inside
    // dispatch; our Pointer is written afterwards so it is not clobbered.
    struct LeaveRecorder : HvBox
    {
        using HvBox::HvBox;
        void on_pointer_leave() override
        {
            if (host_ptr)
            {
                snapshot = host_ptr->cursor_log.size();
            }
            left = true;
        }
        TestHost* host_ptr = nullptr;
        std::size_t snapshot = 0;
        bool left = false;
    };

    HvBox root({0, 0, 400, 400});
    auto* view = root.add_child(create_widget<LeaveRecorder>(&root, Rect{100, 10, 50, 30}));
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    w->set_hoverable().set_on_click([] {});
    TestHost host(&root);
    view->host_ptr = &host;

    host.dispatch_pointer_move({110, 20}); // onto the view (claims hover_widget_)
    host.dispatch_pointer_move({20, 20});  // onto the clickable widget

    REQUIRE(view->left);
    CHECK(view->snapshot == 0);                        // leave ran before any write
    CHECK(host.last_cursor() == CursorShape::Pointer); // claim written after
}

// ── Avatar click targets (UserProfilePanel / RoomInfoPanel) ─────────────

#include "tk/hover_area.h"
#include "tesseract/types.h"
#include "views/RoomInfoPanel.h"
#include "views/UserProfilePanel.h"

namespace
{
// Sweeps the area for the point where the host's hover target is a
// tk::HoverArea (the avatar); returns it or {-1,-1}.
tk::Point find_hover_area(TestHost& host, tk::Rect area, float step = 4.0f)
{
    for (float y = area.y; y < area.y + area.h; y += step)
    {
        for (float x = area.x; x < area.x + area.w; x += step)
        {
            host.dispatch_pointer_move({x, y});
            if (auto w = host.hover_feedback_.lock(); w && dynamic_cast<tk::HoverArea*>(w.get()))
            {
                return {x, y};
            }
        }
    }
    return {-1.0f, -1.0f};
}
} // namespace

TEST_CASE("UserProfilePanel avatar: hover ring, pointer cursor, click opens the lightbox",
          "[tk][hoverable][user_profile_panel]")
{
    auto surface = TestSurface::create(300, 400);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    tesseract::views::UserProfilePanel panel;
    std::string clicked_url, clicked_name;
    panel.on_avatar_clicked = [&](const std::string& u, const std::string& n)
    {
        clicked_url = u;
        clicked_name = n;
    };
    panel.open("@alice:example.org", "Alice", "mxc://hs/avatar");
    panel.measure(lc, {300, 400});
    panel.arrange(lc, {0, 0, 300, 400});
    TestHost host(&panel);

    const tk::Point p = find_hover_area(host, {0, 0, 300, 400});
    REQUIRE(p.x >= 0.0f);
    CHECK(host.hover_feedback_.lock()->hovered());
    CHECK(host.last_cursor() == tk::CursorShape::Pointer);

    host.dispatch_pointer_down(p);
    host.dispatch_pointer_up(p);
    CHECK(clicked_url == "mxc://hs/avatar");
    CHECK(clicked_name == "Alice");
}

TEST_CASE("UserProfilePanel avatar without a URL is not a click target",
          "[tk][hoverable][user_profile_panel]")
{
    auto surface = TestSurface::create(300, 400);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    tesseract::views::UserProfilePanel panel;
    panel.on_avatar_clicked = [](const std::string&, const std::string&) {};
    panel.open("@alice:example.org", "Alice", "");
    panel.measure(lc, {300, 400});
    panel.arrange(lc, {0, 0, 300, 400});
    TestHost host(&panel);

    CHECK(find_hover_area(host, {0, 0, 300, 400}).x < 0.0f);
}

TEST_CASE("RoomInfoPanel avatar: hover ring and click opens the lightbox",
          "[tk][hoverable][room_info_panel]")
{
    auto surface = TestSurface::create(1000, 800);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    TestHost host(nullptr);
    auto owner = tk::create_root_widget<tesseract::views::RoomInfoPanel>(&host);
    host.set_root(owner.get());
    tesseract::views::RoomInfoPanel& panel = *owner;

    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Room";
    info.avatar_url = "mxc://hs/room";
    std::string clicked_url;
    panel.on_avatar_clicked = [&](const std::string& u, const std::string&)
    {
        clicked_url = u;
    };
    panel.open(info);
    const tk::Rect bounds{0, 0, 1000, 800};
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);

    const float panel_x = 1000.0f - tesseract::views::RoomInfoPanel::kPanelW;
    const tk::Point p = find_hover_area(host, {panel_x, 0, 360, 300});
    REQUIRE(p.x >= 0.0f);
    CHECK(host.last_cursor() == tk::CursorShape::Pointer);

    host.dispatch_pointer_down(p);
    host.dispatch_pointer_up(p);
    CHECK(clicked_url == "mxc://hs/room");
}

// ── RoomListView header buttons ("+" join, "×" clear search) ────────────

#include "view_test_util.h"
#include "views/RoomListView.h"

namespace
{
// Finds the Button the host reports as hovered when sweeping `area`.
tk::Button* find_hovered_button(TestHost& host, tk::Rect area)
{
    for (float y = area.y; y < area.y + area.h; y += 3.0f)
    {
        for (float x = area.x; x < area.x + area.w; x += 3.0f)
        {
            host.dispatch_pointer_move({x, y});
            if (auto w = host.hover_feedback_.lock())
            {
                if (auto* b = dynamic_cast<tk::Button*>(w.get()))
                {
                    return b;
                }
            }
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("RoomListView '+' button: hover feedback, pointer cursor, click adds a room",
          "[tk][hoverable][roomlist]")
{
    vt::Stage st;
    auto owner = tk::create_root_widget<tesseract::views::RoomListView>(&st.host);
    tesseract::views::RoomListView& view = *owner;
    int added = 0;
    view.on_add_room_requested = [&]
    {
        ++added;
    };
    st.mount(view, {0, 0, 300, 400});

    // The "+" is the only button in the header when no query is typed.
    tk::Button* plus = find_hovered_button(st.host, {200, 0, 100, 44});
    REQUIRE(plus != nullptr);
    CHECK(plus->hovered());
    CHECK(st.host.last_cursor() == tk::CursorShape::Pointer);

    const tk::Rect r = plus->bounds();
    const tk::Point c{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    st.host.dispatch_pointer_down(c);
    st.host.dispatch_pointer_up(c);
    CHECK(added == 1);
}

TEST_CASE("RoomListView '×' button appears with a query and clears it on click",
          "[tk][hoverable][roomlist]")
{
    vt::Stage st;
    auto owner = tk::create_root_widget<tesseract::views::RoomListView>(&st.host);
    tesseract::views::RoomListView& view = *owner;
    int cleared = 0;
    view.on_search_clear = [&]
    {
        ++cleared;
    };
    st.mount(view, {0, 0, 300, 400});

    const auto visible_buttons = [&]
    {
        std::vector<tk::Button*> v;
        for (const auto& ch : view.children())
        {
            if (auto* b = dynamic_cast<tk::Button*>(ch.get()); b && b->visible())
            {
                v.push_back(b);
            }
        }
        return v;
    };
    // No query: only the "+" exists in the header.
    CHECK(visible_buttons().size() == 1);

    view.set_search_text("alp");
    st.relayout_paint(view, {0, 0, 300, 400});
    auto buttons = visible_buttons();
    REQUIRE(buttons.size() == 2);
    tk::Button* clear = buttons[0]->bounds().x < buttons[1]->bounds().x ? buttons[0] : buttons[1];

    const tk::Rect r = clear->bounds();
    const tk::Point c{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    st.host.dispatch_pointer_move(c);
    CHECK(clear->hovered());
    CHECK(st.host.last_cursor() == tk::CursorShape::Pointer);
    st.host.dispatch_pointer_down(c);
    st.host.dispatch_pointer_up(c);
    CHECK(cleared == 1);
}

// ── ComposeBar multi-attachment chip × ──────────────────────────────────

#include "views/ComposeBar.h"

TEST_CASE("ComposeBar chip '×' shows hover feedback and the pointer cursor",
          "[tk][hoverable][compose]")
{
    auto surface = TestSurface::create(700, 500);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    auto bar_owner = tk::create_root_widget<tesseract::views::ComposeBar>(nullptr);
    tesseract::views::ComposeBar& bar = *bar_owner;
    bar.add_pending_image({0x89, 0x50, 0x4E, 0x47}, "image/png", "a.png");
    bar.add_pending_file({0x01}, "application/zip", "b.zip");
    const tk::Rect bounds{0.0f, 400.0f, 640.0f, 56.0f};
    bar.measure(lc, {bounds.w, bounds.h});
    bar.arrange(lc, bounds);
    TestHost host(&bar);

    const tk::Rect xr = bar.chip_remove_rect_for_test(1);
    REQUIRE(xr.w > 0.0f);
    host.dispatch_pointer_move({xr.x + xr.w * 0.5f, xr.y + xr.h * 0.5f});
    auto hovered = host.hover_feedback_.lock();
    REQUIRE(hovered != nullptr);
    CHECK(dynamic_cast<tk::Button*>(hovered.get()) != nullptr);
    CHECK(hovered->hovered());
    CHECK(host.last_cursor() == tk::CursorShape::Pointer);
}

// ── Switcher chips (MruSwitcher / QuickSwitcher "Recent") ───────────────

#include "views/MruSwitcher.h"
#include "views/QuickSwitcher.h"

namespace
{
tesseract::RoomInfo chip_room(std::string id, std::string name)
{
    tesseract::RoomInfo r;
    r.id = std::move(id);
    r.name = std::move(name);
    return r;
}
} // namespace

TEST_CASE("MruSwitcher chips: hover feedback, pointer cursor, click commits that room",
          "[tk][hoverable][mru_switcher]")
{
    vt::Stage st;
    auto mru = tk::create_root_widget<tesseract::views::MruSwitcher>(&st.host);
    mru->set_recent_provider(
        []
        {
            return std::vector<tesseract::RoomInfo>{
                chip_room("!a:x", "Alpha"), chip_room("!b:x", "Beta"), chip_room("!c:x", "Gamma")};
        });
    std::string switched;
    mru->on_room_selected = [&](const std::string& id)
    {
        switched = id;
    };
    mru->begin_cycle();
    st.mount(*mru, {0, 0, 800, 600});
    st.paint(*mru); // second paint: chip areas now placed (fill-before-chips order)

    const tk::Point p = find_hover_area(st.host, {0, 0, 800, 600});
    REQUIRE(p.x >= 0.0f);
    CHECK(st.host.hover_feedback_.lock()->hovered());
    CHECK(st.host.last_cursor() == tk::CursorShape::Pointer);

    st.host.dispatch_pointer_down(p);
    st.host.dispatch_pointer_up(p);
    CHECK_FALSE(switched.empty());
}

TEST_CASE("QuickSwitcher recent chips: hover feedback and click selects the room",
          "[tk][hoverable][quick_switcher]")
{
    vt::Stage st;
    auto qs = tk::create_root_widget<tesseract::views::QuickSwitcher>(&st.host);
    const auto rooms = []
    {
        return std::vector<tesseract::RoomInfo>{chip_room("!a:x", "Alpha"),
                                                chip_room("!b:x", "Beta")};
    };
    qs->set_rooms_provider(rooms);
    qs->set_recent_provider(rooms);
    std::string chosen;
    qs->on_room_selected = [&](const std::string& id)
    {
        chosen = id;
    };
    qs->open();
    st.mount(*qs, {0, 0, 900, 700});
    st.paint(*qs);

    const tk::Point p = find_hover_area(st.host, {0, 0, 900, 700});
    REQUIRE(p.x >= 0.0f);
    CHECK(st.host.last_cursor() == tk::CursorShape::Pointer);

    st.host.dispatch_pointer_down(p);
    st.host.dispatch_pointer_up(p);
    CHECK_FALSE(chosen.empty());
}

// ── Review follow-ups: repaint gating, inert hover targets, hidden strips ──

TEST_CASE("Idle pointer movement over non-hoverable content requests no repaint",
          "[tk][hoverable][host]")
{
    HvBox root({0, 0, 400, 400});
    auto* w = root.add_child(create_widget<HvBox>(&root, Rect{10, 10, 50, 30}));
    w->set_hoverable().set_on_click([] {});
    TestHost host(&root);

    host.dispatch_pointer_move({300, 300}); // settle: nothing hovered
    const int before = host.repaint_count;
    host.dispatch_pointer_move({310, 305});
    host.dispatch_pointer_move({320, 310});
    CHECK(host.repaint_count == before);

    host.dispatch_pointer_move({20, 20}); // hover starts: repaint
    CHECK(host.repaint_count > before);
    const int mid = host.repaint_count;
    host.dispatch_pointer_move({25, 22}); // same target: nothing new
    CHECK(host.repaint_count == mid);
}

TEST_CASE("A hovered widget that hides itself drops hover and releases the cursor",
          "[tk][hoverable][host]")
{
    // Built with the host (as in production) so widget state changes can
    // notify it.
    TestHost host(nullptr);
    auto root = create_root_widget<HvBox>(&host, Rect{0, 0, 400, 400});
    host.set_root(root.get());
    auto* w = root->add_child(create_widget<HvBox>(root.get(), Rect{10, 10, 50, 30}));
    int leaves = 0;
    w->set_hoverable().set_on_click([] {});
    w->set_on_hover_leave(
        [&]
        {
            ++leaves;
        });

    host.dispatch_pointer_move({20, 20});
    REQUIRE(w->hovered());
    REQUIRE(host.last_cursor() == CursorShape::Pointer);

    w->set_visible(false); // e.g. a clear button hiding itself when clicked
    CHECK_FALSE(w->hovered());
    CHECK(leaves == 1);
    CHECK(host.last_cursor() == CursorShape::Default);
    CHECK(host.hover_feedback_.lock() == nullptr);
}

TEST_CASE("Disabling the hovered widget, or an ancestor of it, drops hover",
          "[tk][hoverable][host]")
{
    TestHost host(nullptr);
    auto root = create_root_widget<HvBox>(&host, Rect{0, 0, 400, 400});
    host.set_root(root.get());
    auto* outer = root->add_child(create_widget<HvBox>(root.get(), Rect{10, 10, 200, 60}));
    auto* inner = outer->add_child(create_widget<HvBox>(outer, Rect{20, 20, 50, 30}));
    inner->set_hoverable().set_on_click([] {});

    host.dispatch_pointer_move({30, 30});
    REQUIRE(inner->hovered());
    outer->set_enabled(false);
    CHECK_FALSE(inner->hovered());
    CHECK(host.last_cursor() == CursorShape::Default);

    outer->set_enabled(true);
    host.dispatch_pointer_move({30, 30});
    REQUIRE(inner->hovered());
    outer->set_visible(false); // hiding an ancestor hides the target too
    CHECK_FALSE(inner->hovered());
}

TEST_CASE("QuickSwitcher recent chips stop taking clicks once a query hides the strip",
          "[tk][hoverable][quick_switcher]")
{
    vt::Stage st;
    auto qs = tk::create_root_widget<tesseract::views::QuickSwitcher>(&st.host);
    const auto rooms = []
    {
        return std::vector<tesseract::RoomInfo>{chip_room("!a:x", "Alpha"),
                                                chip_room("!b:x", "Beta")};
    };
    qs->set_rooms_provider(rooms);
    qs->set_recent_provider(rooms);
    std::string chosen;
    qs->on_room_selected = [&](const std::string& id)
    {
        chosen = id;
    };
    qs->open();
    st.mount(*qs, {0, 0, 900, 700});
    st.paint(*qs);

    const tk::Point chip = find_hover_area(st.host, {0, 0, 900, 700});
    REQUIRE(chip.x >= 0.0f); // the strip is up, so a chip exists

    qs->set_query("a"); // hides the Recent strip
    st.relayout_paint(*qs, {0, 0, 900, 700});

    // No HoverArea may be left anywhere — and in particular none at the
    // place the chip used to be.
    CHECK(find_hover_area(st.host, {0, 0, 900, 700}).x < 0.0f);
    st.host.dispatch_pointer_down(chip);
    st.host.dispatch_pointer_up(chip);
    CHECK(chosen.empty()); // the stale chip must not have selected a room
}
