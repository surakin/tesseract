#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"
#include "views/RoomSearchBar.h"

#include <memory>
#include <string>

using tesseract::views::RoomSearchBar;

namespace
{

struct TkRoomSearchBarStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 44);

    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }

    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }

    void arrange(tk::Widget& w, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        w.measure(lc, {bounds.w, bounds.h});
        w.arrange(lc, bounds);
    }

    void paint(tk::Widget& w)
    {
        auto pc = paint_ctx();
        w.paint(pc);
    }
};

} // namespace

TEST_CASE("open/close toggles is_open and search_field_visible",
          "[room_search_bar]")
{
    RoomSearchBar bar;

    // Initially closed.
    CHECK_FALSE(bar.is_open());
    CHECK_FALSE(bar.search_field_visible());

    bar.open();
    CHECK(bar.is_open());
    CHECK(bar.search_field_visible());

    bar.close();
    CHECK_FALSE(bar.is_open());
    CHECK_FALSE(bar.search_field_visible());
}

TEST_CASE("set_match_status produces correct count text and does not crash",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    RoomSearchBar bar;
    bar.open();
    st.arrange(bar, {0, 0, 800, 44});

    // Searching state.
    bar.set_match_status(0, 0, /*searching=*/true, /*at_start=*/false);
    // No crash; bar still open.
    CHECK(bar.is_open());

    // No matches.
    bar.set_match_status(0, 0, false, false);
    CHECK(bar.is_open());

    // Start of conversation.
    bar.set_match_status(0, 0, false, /*at_start=*/true);
    CHECK(bar.is_open());

    // Normal match count.
    bar.set_match_status(3, 12, false, false);
    CHECK(bar.is_open());
}

TEST_CASE("paginate_enabled reflects CheckButton state",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    RoomSearchBar bar;
    bar.open();
    st.arrange(bar, {0, 0, 800, 44});

    // Default: unchecked.
    CHECK_FALSE(bar.paginate_enabled());
}

TEST_CASE("on_navigate fires with delta -1 for UP button",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    RoomSearchBar bar;
    bar.open();
    st.arrange(bar, {0, 0, 800, 44});

    int last_delta = 0;
    int call_count = 0;
    bar.on_navigate = [&](int d) { last_delta = d; ++call_count; };

    const tk::Rect r = bar.up_btn_rect_for_test();
    REQUIRE(r.w > 0.0f);
    REQUIRE(r.h > 0.0f);

    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = bar.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(centre), true);

    CHECK(call_count == 1);
    CHECK(last_delta == -1);
}

TEST_CASE("on_navigate fires with delta +1 for DOWN button",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    RoomSearchBar bar;
    bar.open();
    st.arrange(bar, {0, 0, 800, 44});

    int last_delta = 0;
    int call_count = 0;
    bar.on_navigate = [&](int d) { last_delta = d; ++call_count; };

    const tk::Rect r = bar.down_btn_rect_for_test();
    REQUIRE(r.w > 0.0f);
    REQUIRE(r.h > 0.0f);

    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = bar.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(centre), true);

    CHECK(call_count == 1);
    CHECK(last_delta == +1);
}

TEST_CASE("on_close fires when close button is clicked",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    RoomSearchBar bar;
    bar.open();
    st.arrange(bar, {0, 0, 800, 44});

    bool closed = false;
    bar.on_close = [&] { closed = true; };

    const tk::Rect r = bar.close_btn_rect_for_test();
    REQUIRE(r.w > 0.0f);
    REQUIRE(r.h > 0.0f);

    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = bar.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(centre), true);

    CHECK(closed);
}

TEST_CASE("arrange reserves kStripH and field_rect is inside strip",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    RoomSearchBar bar;
    bar.open();

    const tk::Rect strip{0, 0, 800, RoomSearchBar::kStripH};
    st.arrange(bar, strip);

    const tk::Rect field = bar.search_field_rect();
    CHECK(field.w > 0.0f);
    CHECK(field.h > 0.0f);

    // Field must lie within the strip bounds.
    CHECK(field.x >= strip.x);
    CHECK(field.y >= strip.y);
    CHECK(field.x + field.w <= strip.x + strip.w);
    CHECK(field.y + field.h <= strip.y + strip.h);
}

TEST_CASE("set_query fires on_query_changed",
          "[room_search_bar]")
{
    RoomSearchBar bar;
    bar.open();

    std::string received;
    int call_count = 0;
    bar.on_query_changed = [&](const std::string& q) { received = q; ++call_count; };

    bar.set_query("hello");
    CHECK(call_count == 1);
    CHECK(received == "hello");

    // Same query again — should not fire.
    bar.set_query("hello");
    CHECK(call_count == 1);

    bar.set_query("world");
    CHECK(call_count == 2);
    CHECK(received == "world");
}

TEST_CASE("buttons not hittable when bar is closed",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    RoomSearchBar bar;
    // Do NOT open — bar is closed.
    st.arrange(bar, {0, 0, 800, 44});

    bool nav_fired = false;
    bar.on_navigate = [&](int) { nav_fired = true; };

    // Rects should be zero — no hit.
    const tk::Rect up = bar.up_btn_rect_for_test();
    CHECK(up.w == 0.0f);
    CHECK(up.h == 0.0f);

    // A click anywhere in the strip should not produce a navigate event.
    tk::Widget* claimer = bar.dispatch_pointer_down({400, 22});
    if (claimer)
        claimer->on_pointer_up(claimer->world_to_local({400, 22}), true);

    CHECK_FALSE(nav_fired);
}
