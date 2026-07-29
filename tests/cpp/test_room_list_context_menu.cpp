#include <catch2/catch_test_macros.hpp>

#include "tesseract/settings.h"
#include "tesseract/types.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/RoomListView.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::RoomInfo;
using tesseract::views::RoomListView;

namespace
{

constexpr tk::Rect kContextMenuBounds{0, 0, 280, 400};

struct RoomListContextMenuStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(280, 400);

    void layout(tk::Widget& w, tk::Rect bounds = kContextMenuBounds)
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        w.measure(lc, {bounds.w, bounds.h});
        w.arrange(lc, bounds);
    }
};

RoomInfo plain_room(const std::string& id, const std::string& name)
{
    RoomInfo r;
    r.id = id;
    r.name = name;
    return r;
}

RoomInfo space_room(const std::string& id, const std::string& name)
{
    RoomInfo r = plain_room(id, name);
    r.is_space = true;
    return r;
}

} // namespace

TEST_CASE("on_right_click: resolves the room under the click and opens the menu",
          "[roomlist][context-menu]")
{
    tesseract::Settings::instance().group_inactive_rooms = false;
    RoomListContextMenuStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;

    view.set_rooms({plain_room("$r0", "Room Zero")});
    st.layout(view);

    tk::Rect row = view.room_row_rect_for_test("$r0");
    REQUIRE_FALSE(row.empty());

    tk::Point click{row.x + 5.0f, row.y + row.h * 0.5f};
    CHECK(view.on_right_click(click));

    const auto& items = view.context_menu_items_for_test();
    REQUIRE(items.size() == 4);
    CHECK(items[0].label == "Open in tab");
    CHECK(items[1].label == "Open in window");
    CHECK(items[2].is_separator);
    CHECK(items[3].label == "Leave room");
    CHECK(items[3].destructive);
}

TEST_CASE("on_right_click: open-in-tab/window items disable when providers say already open",
          "[roomlist][context-menu]")
{
    tesseract::Settings::instance().group_inactive_rooms = false;
    RoomListContextMenuStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;

    view.set_rooms({plain_room("$r0", "Room Zero")});
    st.layout(view);

    view.set_room_open_in_tab_provider(
        [](const std::string& id) { return id == "$r0"; });
    view.set_room_open_in_window_provider(
        [](const std::string&) { return false; });

    tk::Rect row = view.room_row_rect_for_test("$r0");
    REQUIRE_FALSE(row.empty());
    view.on_right_click({row.x + 5.0f, row.y + row.h * 0.5f});

    const auto& items = view.context_menu_items_for_test();
    REQUIRE(items.size() == 4);
    CHECK_FALSE(items[0].enabled); // already open in a tab
    CHECK(items[1].enabled);       // not open in a window
}

TEST_CASE("on_right_click: firing the open-in-tab item's callback requests that room",
          "[roomlist][context-menu]")
{
    tesseract::Settings::instance().group_inactive_rooms = false;
    RoomListContextMenuStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;

    view.set_rooms({plain_room("$r0", "Room Zero")});
    st.layout(view);

    std::string requested;
    view.on_open_in_tab_requested = [&](const std::string& id) { requested = id; };

    tk::Rect row = view.room_row_rect_for_test("$r0");
    REQUIRE_FALSE(row.empty());
    view.on_right_click({row.x + 5.0f, row.y + row.h * 0.5f});

    const auto& items = view.context_menu_items_for_test();
    REQUIRE(items.size() == 4);
    REQUIRE(items[0].on_selected);
    items[0].on_selected();
    CHECK(requested == "$r0");
}

TEST_CASE("on_right_click: a space row produces no menu", "[roomlist][context-menu]")
{
    tesseract::Settings::instance().group_inactive_rooms = false;
    RoomListContextMenuStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;

    view.set_rooms({space_room("$s0", "My Space")});
    st.layout(view);

    tk::Rect row = view.room_row_rect_for_test("$s0");
    REQUIRE_FALSE(row.empty());
    CHECK_FALSE(view.on_right_click({row.x + 5.0f, row.y + row.h * 0.5f}));
    CHECK(view.context_menu_items_for_test().empty());
}

TEST_CASE("on_right_click: a section header row produces no menu",
          "[roomlist][context-menu]")
{
    tesseract::Settings::instance().group_inactive_rooms = false;
    RoomListContextMenuStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;

    // Two rooms in different sections so a collapsible header row is emitted
    // above the Rooms section.
    view.set_rooms({plain_room("$r0", "Room Zero"), space_room("$s0", "Space")});
    st.layout(view);

    // The header sits at the very top of the list, above the first room row.
    tk::Rect first_row = view.room_row_rect_for_test("$r0");
    REQUIRE_FALSE(first_row.empty());
    tk::Point header_click{first_row.x + 5.0f, first_row.y - 4.0f};
    CHECK_FALSE(view.on_right_click(header_click));
    CHECK(view.context_menu_items_for_test().empty());
}

TEST_CASE("on_right_click: clicking empty space below the last row produces no menu",
          "[roomlist][context-menu]")
{
    tesseract::Settings::instance().group_inactive_rooms = false;
    RoomListContextMenuStage st;
    auto view_owner = tk::create_root_widget<RoomListView>(nullptr);
    RoomListView& view = *view_owner;

    view.set_rooms({plain_room("$r0", "Room Zero")});
    st.layout(view);

    CHECK_FALSE(view.on_right_click({10.0f, kContextMenuBounds.h - 2.0f}));
    CHECK(view.context_menu_items_for_test().empty());
}
