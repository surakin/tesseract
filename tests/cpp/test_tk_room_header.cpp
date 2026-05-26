#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"
#include "views/RoomHeader.h"

#include <memory>

using tesseract::views::RoomHeader;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 60);

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

TEST_CASE("threads button fires on_threads_requested",
          "[room_header][threads]")
{
    Stage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});
    // Button rects are computed in paint(); drive one paint pass so
    // threads_btn_rect_ is populated before any pointer events.
    st.paint(h);

    bool clicked = false;
    h.on_threads_requested = [&] { clicked = true; };

    const tk::Rect r = h.threads_btn_rect_for_test();
    REQUIRE(r.w == 28.0f);
    REQUIRE(r.h == 28.0f);

    // Bounds origin is (0,0), so widget-local == world coords.
    const tk::Point centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    REQUIRE(h.on_pointer_down(centre));
    h.on_pointer_up(centre, true);
    CHECK(clicked);
}

TEST_CASE("threads button does NOT fire if release leaves the button rect",
          "[room_header][threads]")
{
    Stage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    bool clicked = false;
    h.on_threads_requested = [&] { clicked = true; };

    const tk::Rect r = h.threads_btn_rect_for_test();
    const tk::Point press{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    REQUIRE(h.on_pointer_down(press));
    // Release well outside the button (still within the header strip).
    h.on_pointer_up({10.0f, 30.0f}, true);
    CHECK_FALSE(clicked);
}

TEST_CASE("threads button sits 8 px left of the calendar button when shown",
          "[room_header][threads]")
{
    Stage st;
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

TEST_CASE("threads button is hidden by default",
          "[room_header][threads]")
{
    Stage st;
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
    h.on_threads_requested = [&] { clicked = true; };
    // Click where the button would be if it were shown.
    h.on_pointer_down({764.0f, 30.0f});
    h.on_pointer_up({764.0f, 30.0f}, true);
    CHECK_FALSE(clicked);
}

TEST_CASE("toggling threads visibility off after a click in progress is safe",
          "[room_header][threads]")
{
    Stage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    bool clicked = false;
    h.on_threads_requested = [&] { clicked = true; };

    const tk::Rect r = h.threads_btn_rect_for_test();
    const tk::Point press{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    REQUIRE(h.on_pointer_down(press));

    // SDK reports list became empty mid-press (e.g., last thread redacted).
    h.set_show_threads_btn(false);
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    // Release at the original press location. The zeroed rect must reject
    // the hit so on_threads_requested doesn't fire on a vanished button.
    h.on_pointer_up(press, true);
    CHECK_FALSE(clicked);
}

TEST_CASE("threads button alone takes the right-most slot when calendar is off",
          "[room_header][threads]")
{
    Stage st;
    RoomHeader h;
    h.set_show_threads_btn(true);
    st.arrange(h, {0, 0, 800, 60});
    st.paint(h);

    const tk::Rect r = h.threads_btn_rect_for_test();
    // With only the threads button visible, it occupies the calendar slot:
    // x = 800 - 8 (margin) - 28 (size) = 764.
    CHECK(r.x == 764.0f);
}
