#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/i18n.h"
#include "tk/theme.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <tesseract/settings.h>
#include <tesseract/types.h>

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::RoomView;

namespace
{

void run_layout(TestSurface& surface, Widget& root)
{
    LayoutCtx lc{surface.factory(), Theme::light()};
    root.measure(lc, {1000, 700});
    root.arrange(lc, {0, 0, 1000, 700});
    PaintCtx pc{surface.canvas(), surface.factory(), Theme::light()};
    root.paint(pc);
}

MessageRowData own_text()
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = "$own:example.org";
    r.sender = "@me:example.org";
    r.sender_name = "Me";
    r.body = "hello";
    r.is_own = true;
    return r;
}

struct BubblesLayoutGuard
{
    tesseract::Settings::MessageLayout prev = tesseract::Settings::instance().message_layout;
    bool prev_own_avatar = tesseract::Settings::instance().bubbles_show_own_avatar;
    BubblesLayoutGuard()
    {
        tesseract::Settings::instance().message_layout =
            tesseract::Settings::MessageLayout::Bubbles;
        // Defaults to on; with it on a right-aligned own row already has the
        // avatar header, so its height wouldn't differ from the left layout's.
        tesseract::Settings::instance().bubbles_show_own_avatar = false;
    }
    ~BubblesLayoutGuard()
    {
        tesseract::Settings::instance().message_layout = prev;
        tesseract::Settings::instance().bubbles_show_own_avatar = prev_own_avatar;
    }
};

void open_room_for_bubbles(RoomView& view)
{
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
}

} // namespace

TEST_CASE("Thread panel states toggle the main list's side-panel flag", "[tk][view][room][thread]")
{
    auto surface = TestSurface::create(1000, 700);
    auto owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *owner;
    open_room_for_bubbles(view);
    REQUIRE(view.message_list() != nullptr);
    CHECK_FALSE(view.message_list()->side_panel_open());

    view.set_thread_panel(RoomView::ThreadPanelState::List, "");
    CHECK(view.message_list()->side_panel_open());

    view.set_thread_panel(RoomView::ThreadPanelState::Open, "$root:example.org");
    CHECK(view.message_list()->side_panel_open());

    view.set_thread_panel(RoomView::ThreadPanelState::Closed, "");
    CHECK_FALSE(view.message_list()->side_panel_open());
}

TEST_CASE("Room info panel toggles the main list's side-panel flag", "[tk][view][room]")
{
    auto surface = TestSurface::create(1000, 700);
    auto owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *owner;
    open_room_for_bubbles(view);

    view.show_room_info();
    REQUIRE(view.room_info_panel()->is_open());
    CHECK(view.message_list()->side_panel_open());

    view.room_info_panel()->close();
    CHECK_FALSE(view.message_list()->side_panel_open());
}

TEST_CASE("Bubbles: own row lays out like other users' while a side panel is open",
          "[tk][view][room][bubbles]")
{
    BubblesLayoutGuard guard;
    auto surface = TestSurface::create(1000, 700);
    auto owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *owner;
    open_room_for_bubbles(view);
    view.set_messages({own_text()});
    run_layout(*surface, view);
    // Right-aligned own rows carry no avatar/name header (own avatar is off
    // via the guard); on the left they get the header band like everyone else.
    const float right_h = view.message_list()->row_world_rect(0).h;

    view.set_thread_panel(RoomView::ThreadPanelState::List, "");
    run_layout(*surface, view);
    const float left_h = view.message_list()->row_world_rect(0).h;
    CHECK(left_h > right_h);

    view.set_thread_panel(RoomView::ThreadPanelState::Closed, "");
    run_layout(*surface, view);
    CHECK(view.message_list()->row_world_rect(0).h == right_h);
}

TEST_CASE("Classic layout ignores the side-panel flag", "[tk][view][room]")
{
    auto& s = tesseract::Settings::instance();
    const auto prev = s.message_layout;
    s.message_layout = tesseract::Settings::MessageLayout::Classic;

    auto surface = TestSurface::create(1000, 700);
    auto owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *owner;
    open_room_for_bubbles(view);
    view.set_messages({own_text()});
    run_layout(*surface, view);
    const float before = view.message_list()->row_world_rect(0).h;

    view.set_thread_panel(RoomView::ThreadPanelState::List, "");
    run_layout(*surface, view);
    CHECK(view.message_list()->row_world_rect(0).h == before);

    s.message_layout = prev;
}
