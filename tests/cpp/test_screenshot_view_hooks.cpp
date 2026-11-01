#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/i18n.h"
#include "tk/theme.h"
#include "views/ComposeBar.h"
#include "views/EmojiPicker.h"
#include "views/RoomView.h"
#include "views/SettingsView.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <tesseract/types.h>

#include <algorithm>
#include <memory>

using namespace tk;
using tesseract::views::RoomView;
using tesseract::views::SettingsView;

TEST_CASE("close_pickers hides an open emoji picker", "[screenshot][room_view]")
{
    auto surface = TestSurface::create(1000, 700);
    TestHost host(nullptr);
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    LayoutCtx lc{surface->factory(), Theme::light()};
    view.measure(lc, {1000, 700});
    view.arrange(lc, {0, 0, 1000, 700});

    view.show_emoji_picker();
    REQUIRE(view.emoji_picker()->visible());

    view.close_pickers();
    CHECK_FALSE(view.emoji_picker()->visible());
}

TEST_CASE("show_appearance_section selects the Appearance tab", "[screenshot][settings]")
{
    TestHost host(nullptr);
    auto view_owner = tk::create_root_widget<SettingsView>(&host);
    SettingsView& view = *view_owner;
    host.set_root(&view);
    REQUIRE(view.selected_tab_label() == tk::tr("Account"));

    // Compare labels, not indices: a reordered add_tab() must fail here.
    view.show_appearance_section();
    CHECK(view.selected_tab_label() == tk::tr("Appearance"));
}

TEST_CASE("show_emoji_picker anchors on the compose bar's emoji button", "[screenshot][room_view]")
{
    auto surface = TestSurface::create(1000, 700);
    TestHost host(nullptr);
    auto view_owner = tk::create_root_widget<RoomView>(&host);
    RoomView& view = *view_owner;
    host.set_root(&view);
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    LayoutCtx lc{surface->factory(), Theme::light()};
    view.measure(lc, {1000, 700});
    view.arrange(lc, {0, 0, 1000, 700});

    const Rect button = view.compose_bar()->emoji_button()->bounds();
    REQUIRE(button.w > 0.0f);

    view.show_emoji_picker();
    const Rect picker = view.emoji_picker()->bounds();

    // Same placement as clicking the button: starts at the button, clamped
    // so it stays inside the view (it must not open at the bar's left edge).
    CHECK(picker.x == std::min(button.x, 1000.0f - picker.w));
    CHECK(picker.y + picker.h <= button.y);
}
