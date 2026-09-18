#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "tesseract/types.h"
#include "views/RoomInfoPanel.h"

#include <memory>

using tesseract::views::RoomInfoPanel;

TEST_CASE("RoomInfoPanel: Media row is clickable when the panel sits right of x=0",
          "[room_info_panel]")
{
    auto surface = TestSurface::create(1000, 800);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};

    TestHost host(nullptr);
    auto panel_owner = tk::create_root_widget<RoomInfoPanel>(&host);
    host.set_root(panel_owner.get());
    RoomInfoPanel& panel = *panel_owner;
    tesseract::RoomInfo info;
    info.id   = "!room:example.org";
    info.name = "Room";
    panel.set_media_count(3);
    panel.open(info);

    int fired = 0;
    panel.on_media_view_requested = [&](std::string) { ++fired; };

    const tk::Rect bounds{0, 0, 1000, 800};
    panel.measure(lc, {bounds.w, bounds.h});
    panel.arrange(lc, bounds);
    tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light(), nullptr, &host};
    panel.paint(pc);

    // Sweep down the panel's left inset (clear of the inset buttons and the
    // right-edge scrollbar); the full-width Media row must be hit somewhere.
    const float x = 1000.0f - RoomInfoPanel::kPanelW + 2.0f;
    for (float y = 50.0f; y < 800.0f && fired == 0; y += 2.0f)
    {
        host.dispatch_pointer_down({x, y});
        host.dispatch_pointer_up({x, y});
    }
    CHECK(fired == 1);
}
