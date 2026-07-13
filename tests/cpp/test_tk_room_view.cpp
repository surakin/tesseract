#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/RoomView.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <memory>

using namespace tk;
using tesseract::views::RoomView;

namespace
{

struct TkRoomViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("RoomView exposes a compose text-area rect while a room is active",
          "[tk][view][room]")
{
    TkRoomViewStage st;
    RoomView view;

    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);

    st.run(view, {0, 0, 800, 600});
    CHECK_FALSE(view.compose_text_area_rect().empty());
}

TEST_CASE("RoomView clears the compose text-area rect after the room closes",
          "[tk][view][room]")
{
    TkRoomViewStage st;
    RoomView view;

    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    st.run(view, {0, 0, 800, 600});
    REQUIRE_FALSE(view.compose_text_area_rect().empty());

    // Closing the room shows the brand view again; the host overlays the
    // native text area at compose_text_area_rect(), so an empty rect is what
    // tells the shell to hide it.
    view.clear_room();
    st.run(view, {0, 0, 800, 600});
    CHECK(view.compose_text_area_rect().empty());
}

TEST_CASE("RoomView claims drag-hover onto its compose bar and releases it "
          "on leave",
          "[tk][view][room][drag_hover]")
{
    TkRoomViewStage st;
    RoomView view;

    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    st.run(view, {0, 0, 800, 600});

    REQUIRE(view.compose_bar() != nullptr);
    const Rect cb = view.compose_bar()->bounds();
    const Point inside{cb.x + cb.w * 0.5f, cb.y + cb.h * 0.5f};

    Widget* target = view.dispatch_drag_hover(inside);
    CHECK(target == &view);

    view.on_drag_leave();
    // on_drag_leave clears the highlight flag; re-claiming should still work
    // (not left in some latched state).
    CHECK(view.dispatch_drag_hover(inside) == &view);
}

TEST_CASE("RoomView does not claim drag-hover while its compose bar is "
          "disabled (no active room)",
          "[tk][view][room][drag_hover]")
{
    TkRoomViewStage st;
    RoomView view;
    st.run(view, {0, 0, 800, 600});

    // Mirrors on_file_drop's gate: compose_bar_->enabled() is false until
    // set_room() runs (and again after clear_room()), regardless of where in
    // RoomView's bounds the point falls — RoomView is the position-agnostic
    // catch-all, so it always claims-or-rejects as a whole, never by point.
    Widget* target = view.dispatch_drag_hover({400.0f, 300.0f});
    CHECK(target == nullptr);
}

TEST_CASE("RoomView closes the action-pill overflow menu on room switch",
          "[tk][view][room]")
{
    TkRoomViewStage st;
    RoomView view;

    tesseract::RoomInfo room_a;
    room_a.id   = "!a:example.org";
    room_a.name = "Room A";
    view.set_room(room_a);
    st.run(view, {0, 0, 800, 600});

    REQUIRE(view.overflow_menu() != nullptr);
    view.overflow_menu()->open({{"", {}, "Pin message", false, [] {}}},
                               {10, 10, 20, 20});
    REQUIRE(view.overflow_menu()->is_open());

    // Switching to a different room must not leave the previous room's
    // action-pill submenu open — it used to stay open until the user
    // happened to click in the timeline, which triggers the popup's own
    // backdrop-dismiss handler.
    tesseract::RoomInfo room_b;
    room_b.id   = "!b:example.org";
    room_b.name = "Room B";
    view.set_room(room_b);
    st.run(view, {0, 0, 800, 600});

    CHECK_FALSE(view.overflow_menu()->is_open());
}
