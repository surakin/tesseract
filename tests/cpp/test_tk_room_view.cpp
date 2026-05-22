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

struct Stage
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
    Stage st;
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
    Stage st;
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
