#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"
#include "views/DatePickerView.h"
#include "views/RoomHeader.h"

#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>

using tesseract::views::RoomHeader;

namespace
{

struct TkRoomHeaderStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 60);

    tk::LayoutCtx layout_ctx() const
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }

    tk::PaintCtx paint_ctx() const
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(), tk::Theme::light()};
    }

    void arrange(tk::Widget& w, tk::Rect bounds) const
    {
        auto lc = layout_ctx();
        w.measure(lc, {bounds.w, bounds.h});
        w.arrange(lc, bounds);
    }

    void paint(tk::Widget& w) const
    {
        auto pc = paint_ctx();
        w.paint(pc);
    }
};

} // namespace

TEST_CASE("threads button fires on_threads_requested", "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_threads_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.threads_btn_rect_for_test();
    REQUIRE(r.w == 28.0f);
    REQUIRE(r.h == 28.0f);

    // The threads button is now a child tk::Button; dispatch the press through
    // the widget tree (as the host does) so the button claims it, then release
    // inside it to fire the click.
    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = h.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(centre), true);
    CHECK(clicked);
}

TEST_CASE("threads button does NOT fire if release leaves the button rect",
          "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_threads_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.threads_btn_rect_for_test();
    const tk::Point press{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = h.dispatch_pointer_down(press);
    REQUIRE(claimer != nullptr);
    // Release well outside the button (still within the header strip). The
    // button only fires when the release lands inside its own bounds.
    const tk::Point release{10.0f, 30.0f};
    claimer->on_pointer_up(claimer->world_to_local(release), claimer->contains_world(release));
    CHECK_FALSE(clicked);
}

TEST_CASE("threads button sits 8 px left of the calendar button when shown",
          "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_jump_to_date_enabled(true);
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    const tk::Rect threads = h.threads_btn_rect_for_test();
    REQUIRE(threads.w == 28.0f);
    // Calendar button is 8 px from the right edge → x = 800 - 8 - 28 = 764.
    // Threads should be 8 px left of that → x = 764 - 8 - 28 = 728.
    CHECK(threads.x == 728.0f);
}

TEST_CASE("threads button is hidden by default", "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    // A fresh header — no SDK confirmation that the room has threads — must
    // not paint a clickable threads button. The hit-rect is zeroed so a click
    // anywhere along the right-side action area falls through to other
    // header gestures (info / topic) instead of firing on_threads_requested.
    const tk::Rect r = h.threads_btn_rect_for_test();
    CHECK(r.w == 0.0f);
    CHECK(r.h == 0.0f);

    bool clicked = false;
    h.on_threads_requested = [&]
    {
        clicked = true;
    };
    // Click where the button would be if it were shown.
    h.on_pointer_down({764.0f, 30.0f});
    h.on_pointer_up({764.0f, 30.0f}, true);
    CHECK_FALSE(clicked);
}

TEST_CASE("toggling threads visibility off after a click in progress is safe",
          "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_threads_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.threads_btn_rect_for_test();
    const tk::Point press{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = h.dispatch_pointer_down(press);
    REQUIRE(claimer != nullptr);

    // SDK reports list became empty mid-press (e.g., last thread redacted).
    h.set_show_threads_btn(false);
    st.arrange(h, {0, 0, 800, 60});

    // Release at the original press location. The button's bounds are now
    // zeroed, so the release lands outside it and the click must not fire.
    claimer->on_pointer_up(claimer->world_to_local(press), claimer->contains_world(press));
    CHECK_FALSE(clicked);
}

TEST_CASE("threads button alone takes the right-most slot when calendar is off",
          "[room_header][threads]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    const tk::Rect r = h.threads_btn_rect_for_test();
    // With only the threads button visible, it occupies the calendar slot:
    // x = 800 - 8 (margin) - 28 (size) = 764.
    CHECK(r.x == 764.0f);
}

TEST_CASE("search button fires on_search_requested", "[room_header][search]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_search_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_search_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.search_btn_rect_for_test();
    REQUIRE(r.w == 28.0f);
    REQUIRE(r.h == 28.0f);

    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    tk::Widget* claimer = h.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(centre), true);
    CHECK(clicked);
}

TEST_CASE("search button does NOT fire if release leaves the button rect", "[room_header][search]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    h.set_show_search_btn(true);
    st.arrange(h, {0, 0, 800, 60});

    bool clicked = false;
    h.on_search_requested = [&]
    {
        clicked = true;
    };

    const tk::Rect r = h.search_btn_rect_for_test();
    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    const tk::Point outside{0.0f, 0.0f};
    tk::Widget* claimer = h.dispatch_pointer_down(centre);
    REQUIRE(claimer != nullptr);
    claimer->on_pointer_up(claimer->world_to_local(outside), false);
    CHECK_FALSE(clicked);
}

// ── Clickable title block (Widget hover/click feature) ──────────────────

#include "tesseract/types.h"
#include "tk_test_host.h"

TEST_CASE("Hovering the room name lights the title block; clicking it opens room info",
          "[room_header][hover]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    tesseract::RoomInfo info{};
    info.id = "!r:example.org";
    info.name = "Design";
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});
    TestHost host(&h);

    int opened = 0;
    h.on_info_requested = [&]
    {
        ++opened;
    };

    const tk::Point on_name{120.0f, 30.0f};
    host.dispatch_pointer_move(on_name);
    REQUIRE(host.hover_feedback_.lock() != nullptr);
    tk::Widget* title_block = host.hover_feedback_.lock().get();
    CHECK(title_block->hovered());
    CHECK_FALSE(dynamic_cast<tk::Button*>(title_block)); // not an action button
    CHECK(host.last_cursor() == tk::CursorShape::Pointer);

    host.dispatch_pointer_down(on_name);
    host.dispatch_pointer_up(on_name);
    CHECK(opened == 1);

    host.dispatch_pointer_move({400.0f, 100.0f}); // off the header entirely
    CHECK_FALSE(title_block->hovered());
}

TEST_CASE("The title block adds no extra Tab stop beside its keyboard target",
          "[room_header][hover][keyboard]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    tesseract::RoomInfo info{};
    info.name = "Design";
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});
    TestHost host(&h);

    host.dispatch_key_down({tk::Key::Tab});
    CHECK(host.focused_widget() == h.title_target());
}

TEST_CASE("The title block keeps the name and topic labels where RoomHeader placed them",
          "[room_header][hover]")
{
    // The labels are children of the hoverable title block; arranging that
    // block must not reset them to its own rect (base Widget::arrange would).
    TkRoomHeaderStage st;
    RoomHeader h;
    tesseract::RoomInfo info{};
    info.name = "Design";
    info.topic = "Weekly sync";
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});

    const tk::Rect title = h.title_target()->bounds();
    const tk::Rect name = h.name_label_rect_for_test();
    const tk::Rect topic = h.topic_label_rect_for_test();
    REQUIRE(title.w > 0.0f);
    REQUIRE(name.w > 0.0f);
    REQUIRE(topic.w > 0.0f);
    CHECK(name.h < title.h); // not stretched to the whole block
    CHECK(topic.y > name.y); // topic sits below the name
    CHECK(name.y + name.h <= topic.y + 0.5f);
}

TEST_CASE("A bridged room shows a bridge badge beside the title; an override hides it",
          "[room_header][bridge]")
{
    TkRoomHeaderStage st;
    RoomHeader h;
    tesseract::RoomInfo info{};
    info.name = "Design";
    info.is_bridged = true;
    info.bridge_network_name = "WhatsApp";
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});

    auto has_bridge_row = [&]
    {
        for (std::size_t i = 0; i < h.access_row_count(); ++i)
        {
            if (h.access_name_for_widget_row(i).find("WhatsApp") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(has_bridge_row());

    info.bridge_overridden = true;
    h.set_room(info);
    st.arrange(h, {0, 0, 800, 60});
    CHECK_FALSE(has_bridge_row());
}

#ifndef _WIN32
TEST_CASE("local_midnight_ms starts the picked day at local midnight",
          "[tk][view][room_header][jump]")
{
    // The caller's TZ is restored when the case ends.
    const char* saved = std::getenv("TZ");
    const std::string saved_tz = saved ? saved : "";
    auto set_tz = [](const char* tz)
    {
        setenv("TZ", tz, 1);
        tzset();
    };

    // POSIX zone strings: "XYZ-3" is three hours ahead of UTC, "PST+8" eight behind.
    set_tz("XYZ-3");
    CHECK(tesseract::views::local_midnight_ms(2026, 10, 12) == 1791752400000ULL);
    set_tz("PST+8");
    CHECK(tesseract::views::local_midnight_ms(2026, 10, 12) == 1791792000000ULL);
    set_tz("UTC");
    CHECK(tesseract::views::local_midnight_ms(2026, 10, 12) == 1791763200000ULL);

    if (saved)
    {
        set_tz(saved_tz.c_str());
    }
    else
    {
        unsetenv("TZ");
        tzset();
    }
}
#endif
