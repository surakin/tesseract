#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/row_cursor.h"
#include "tk/scrollable_base.h"
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

struct RcRow
{
    Role role = Role::Button;
    std::string name;
    bool disabled = false;
    Rect rect{};
};

// A plain widget exposing configurable rows.
class RcOwner : public Widget, public WidgetRowAccessibility
{
public:
    std::vector<RcRow> rows;
    std::vector<std::size_t> activated;

    Size measure(LayoutCtx&, Size c) override { return c; }
    bool focusable() const override { return true; }
    std::size_t access_row_count() const override { return rows.size(); }
    Role access_role_for_widget_row(std::size_t i) const override
    {
        return rows[i].role;
    }
    std::string access_name_for_widget_row(std::size_t i) const override
    {
        return rows[i].name;
    }
    AccessState access_state_for_widget_row(std::size_t i) const override
    {
        AccessState s;
        s.disabled = rows[i].disabled;
        return s;
    }
    bool access_activate_widget_row(std::size_t i) override
    {
        activated.push_back(i);
        return true;
    }
    Rect access_rect_for_widget_row(std::size_t i) const override
    {
        return rows[i].rect;
    }
};

// A ScrollableBase owner that records scroll_into_view requests.
class RcScroller : public ScrollableBase, public WidgetRowAccessibility
{
public:
    std::vector<RcRow> rows;
    std::vector<Rect> scrolled_to;

    Size measure(LayoutCtx&, Size c) override { return c; }
    bool focusable() const override { return true; }
    float content_height() const override { return 1000.0f; }
    void scroll_into_view(Rect r) override { scrolled_to.push_back(r); }
    std::size_t access_row_count() const override { return rows.size(); }
    Role access_role_for_widget_row(std::size_t i) const override
    {
        return rows[i].role;
    }
    std::string access_name_for_widget_row(std::size_t i) const override
    {
        return rows[i].name;
    }
    Rect access_rect_for_widget_row(std::size_t i) const override
    {
        return rows[i].rect;
    }
};

KeyEvent rc_key(Key k)
{
    KeyEvent e;
    e.key = k;
    return e;
}

std::vector<RcRow> sample_rows()
{
    return {
        {Role::StaticText, "heading", false, {0, 0, 100, 20}},    // not a stop
        {Role::Button, "A", false, {0, 20, 100, 20}},        // stop
        {Role::Button, "B", true, {0, 40, 100, 20}},         // disabled
        {Role::Link, "C", false, {0, 60, 100, 20}},          // stop
        {Role::Button, "offscreen", false, {0, 80, 0, 0}},   // empty rect
        {Role::CheckBox, "D", false, {0, 100, 100, 20}},     // stop
    };
}

} // namespace

TEST_CASE("RowKeyboardCursor row_is_stop and has_stops classify rows",
          "[tk][row_cursor]")
{
    StubHost host;
    auto o = create_root_widget<RcOwner>(&host);
    CHECK_FALSE(RowKeyboardCursor::has_stops(*o));
    o->rows = sample_rows();
    CHECK_FALSE(RowKeyboardCursor::row_is_stop(*o, 0));
    CHECK(RowKeyboardCursor::row_is_stop(*o, 1));
    CHECK_FALSE(RowKeyboardCursor::row_is_stop(*o, 2));
    CHECK(RowKeyboardCursor::row_is_stop(*o, 3));
    CHECK_FALSE(RowKeyboardCursor::row_is_stop(*o, 4));
    CHECK(RowKeyboardCursor::row_is_stop(*o, 5));
    CHECK(RowKeyboardCursor::has_stops(*o));

    for (Role r : {Role::RadioButton, Role::Switch, Role::MenuItem,
                   Role::GridCell})
    {
        o->rows = {{r, "x", false, {0, 0, 10, 10}}};
        CHECK(RowKeyboardCursor::has_stops(*o));
    }
    o->rows = {{Role::StaticText, "x", false, {0, 0, 10, 10}}};
    CHECK_FALSE(RowKeyboardCursor::has_stops(*o));
}

TEST_CASE("RowKeyboardCursor ignores keys when the owner is unfocused or a "
          "modifier is held",
          "[tk][row_cursor]")
{
    StubHost host;
    auto o = create_root_widget<RcOwner>(&host);
    o->rows = sample_rows();
    RowKeyboardCursor c;
    CHECK_FALSE(c.handle_key(*o, *o, rc_key(Key::Down)));
    host.request_focus(o.get());
    REQUIRE(o->has_focus());
    for (int m = 0; m < 3; ++m)
    {
        KeyEvent e = rc_key(Key::Down);
        (m == 0 ? e.ctrl : m == 1 ? e.alt : e.meta) = true;
        CHECK_FALSE(c.handle_key(*o, *o, e));
    }
    CHECK(c.index() == -1);
    CHECK_FALSE(c.handle_key(*o, *o, rc_key(Key::Escape)));
}

TEST_CASE("RowKeyboardCursor arrow keys move between stops and swallow at the "
          "ends",
          "[tk][row_cursor]")
{
    StubHost host;
    auto o = create_root_widget<RcOwner>(&host);
    o->rows = sample_rows();
    host.request_focus(o.get());
    RowKeyboardCursor c;

    CHECK(c.handle_key(*o, *o, rc_key(Key::Down)));
    CHECK(c.index() == 1);
    CHECK(c.handle_key(*o, *o, rc_key(Key::Right)));
    CHECK(c.index() == 3); // skips disabled B
    CHECK(c.handle_key(*o, *o, rc_key(Key::Down)));
    CHECK(c.index() == 5); // skips the empty-rect row
    CHECK(c.handle_key(*o, *o, rc_key(Key::Down)));
    CHECK(c.index() == 5); // end: swallowed, stays
    CHECK(c.handle_key(*o, *o, rc_key(Key::Up)));
    CHECK(c.index() == 3);
    CHECK(c.handle_key(*o, *o, rc_key(Key::Left)));
    CHECK(c.index() == 1);
    CHECK(c.handle_key(*o, *o, rc_key(Key::Up)));
    CHECK(c.index() == 1);

    // Landing shows a tooltip naming the row.
    CHECK(host.tooltip_owner_ == o.get());

    c.reset();
    CHECK(c.index() == -1);
    // First Up with no current stop lands on the last one.
    CHECK(c.handle_key(*o, *o, rc_key(Key::Up)));
    CHECK(c.index() == 5);
}

TEST_CASE("RowKeyboardCursor Home/End jump and Enter/Space activate",
          "[tk][row_cursor]")
{
    StubHost host;
    auto o = create_root_widget<RcOwner>(&host);
    o->rows = sample_rows();
    host.request_focus(o.get());
    RowKeyboardCursor c;

    CHECK(c.handle_key(*o, *o, rc_key(Key::End)));
    CHECK(c.index() == 5);
    CHECK(c.handle_key(*o, *o, rc_key(Key::Home)));
    CHECK(c.index() == 1);

    CHECK(c.handle_key(*o, *o, rc_key(Key::Enter)));
    CHECK(c.handle_key(*o, *o, rc_key(Key::Space)));
    CHECK(o->activated == std::vector<std::size_t>{1, 1});

    // First Enter with no current stop only lands.
    c.reset();
    CHECK(c.handle_key(*o, *o, rc_key(Key::Enter)));
    CHECK(c.index() == 1);
    CHECK(o->activated.size() == 2);
}

TEST_CASE("RowKeyboardCursor Enter with no stops is not consumed; a stale "
          "index resets",
          "[tk][row_cursor]")
{
    StubHost host;
    auto o = create_root_widget<RcOwner>(&host);
    o->rows = {{Role::StaticText, "x", false, {0, 0, 10, 10}}};
    host.request_focus(o.get());
    RowKeyboardCursor c;
    CHECK_FALSE(c.handle_key(*o, *o, rc_key(Key::Enter)));
    CHECK(c.handle_key(*o, *o, rc_key(Key::Home)));
    CHECK(c.index() == -1);

    o->rows = sample_rows();
    c.handle_key(*o, *o, rc_key(Key::End));
    REQUIRE(c.index() == 5);
    o->rows.resize(2); // index now out of range -> reset, then lands on 1
    CHECK(c.handle_key(*o, *o, rc_key(Key::Down)));
    CHECK(c.index() == 1);
    o->rows[1].disabled = true; // current stop became disabled -> reset
    CHECK_FALSE(c.handle_key(*o, *o, rc_key(Key::Enter)));
    CHECK_FALSE(RowKeyboardCursor::has_stops(*o));
}

TEST_CASE("RowKeyboardCursor scrolls a ScrollableBase owner to the landed row",
          "[tk][row_cursor]")
{
    StubHost host;
    auto o = create_root_widget<RcScroller>(&host);
    o->rows = {{Role::Button, "A", false, {0, 500, 50, 20}}};
    host.request_focus(o.get());
    RowKeyboardCursor c;
    CHECK(c.handle_key(*o, *o, rc_key(Key::Down)));
    REQUIRE(o->scrolled_to.size() == 1);
    CHECK(o->scrolled_to[0].y == 500);
}

TEST_CASE("RowKeyboardCursor paint_ring targets the row or the owner",
          "[tk][row_cursor]")
{
    StubHost host;
    auto o = create_root_widget<RcOwner>(&host);
    o->rows = sample_rows();
    host.request_focus(o.get());
    auto surface = TestSurface::create(200, 200);
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
    RowKeyboardCursor c;
    c.paint_ring(pc, *o, *o); // no current row: owner ring
    c.handle_key(*o, *o, rc_key(Key::Down));
    c.paint_ring(pc, *o, *o); // row ring
    o->rows[1].rect = {};     // current row has an empty rect: falls back
    c.paint_ring(pc, *o, *o);
    SUCCEED();
}
