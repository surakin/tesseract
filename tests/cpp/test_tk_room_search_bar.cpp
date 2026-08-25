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
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;

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
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
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
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
    bar.open();
    st.arrange(bar, {0, 0, 800, 44});

    // Default: unchecked.
    CHECK_FALSE(bar.paginate_enabled());
}

TEST_CASE("on_navigate fires with delta -1 for UP button",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
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
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
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
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
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
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
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
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
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

TEST_CASE("set_show_close_button(false) hides the close button and reclaims its space",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;

    auto full_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& full = *full_owner;
    full.open();
    st.arrange(full, {0, 0, 800, 44});
    const tk::Rect full_close = full.close_btn_rect_for_test();
    const tk::Rect full_down  = full.down_btn_rect_for_test();
    REQUIRE(full_close.w > 0.0f);
    REQUIRE(full_down.w > 0.0f);

    auto narrow_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& narrow = *narrow_owner;
    narrow.set_show_close_button(false);
    narrow.open();
    st.arrange(narrow, {0, 0, 800, 44});

    const tk::Rect narrow_close = narrow.close_btn_rect_for_test();
    CHECK(narrow_close.w == 0.0f);
    CHECK(narrow_close.h == 0.0f);

    // The down button shifts right into the reclaimed space rather than
    // leaving an empty gap where the hidden close button used to be.
    const tk::Rect narrow_down = narrow.down_btn_rect_for_test();
    CHECK(narrow_down.x > full_down.x);
}

TEST_CASE("set_show_paginate(false) additionally reclaims the paginate checkbox's space",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;

    auto close_only_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& close_only = *close_only_owner;
    close_only.set_show_close_button(false);
    close_only.open();
    st.arrange(close_only, {0, 0, 800, 44});
    REQUIRE(close_only.paginate_rect_for_test().w > 0.0f);
    const tk::Rect close_only_down = close_only.down_btn_rect_for_test();

    auto both_hidden_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& both_hidden = *both_hidden_owner;
    both_hidden.set_show_close_button(false);
    both_hidden.set_show_paginate(false);
    both_hidden.open();
    st.arrange(both_hidden, {0, 0, 800, 44});

    const tk::Rect both_paginate = both_hidden.paginate_rect_for_test();
    CHECK(both_paginate.w == 0.0f);
    CHECK(both_paginate.h == 0.0f);

    const tk::Rect both_down = both_hidden.down_btn_rect_for_test();
    CHECK(both_down.x > close_only_down.x);
}

TEST_CASE("set_show_close_button/set_show_paginate flags survive a close/reopen cycle",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
    bar.set_show_close_button(false);
    bar.set_show_paginate(false);
    bar.open();
    st.arrange(bar, {0, 0, 800, 44});
    CHECK(bar.close_btn_rect_for_test().w == 0.0f);
    CHECK(bar.paginate_rect_for_test().w == 0.0f);

    bar.close();
    bar.open(); // flags must still be respected, not reset to default-visible
    st.arrange(bar, {0, 0, 800, 44});
    CHECK(bar.close_btn_rect_for_test().w == 0.0f);
    CHECK(bar.paginate_rect_for_test().w == 0.0f);
}

TEST_CASE("clear_query resets the query without touching open state",
          "[room_search_bar]")
{
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
    bar.open();

    std::string last_query = "unset";
    int call_count = 0;
    bar.on_query_changed = [&](const std::string& q) { last_query = q; ++call_count; };

    bar.set_query("hello");
    CHECK(call_count == 1);

    bar.clear_query();
    CHECK(bar.query().empty());
    CHECK(last_query.empty());
    CHECK(call_count == 2);
    CHECK(bar.is_open());

    // Already empty — must not re-fire the callback.
    bar.clear_query();
    CHECK(call_count == 2);
}

TEST_CASE("buttons not hittable when bar is closed",
          "[room_search_bar]")
{
    TkRoomSearchBarStage st;
    auto bar_owner = tk::create_root_widget<RoomSearchBar>(nullptr);
    RoomSearchBar& bar = *bar_owner;
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
