#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/MessageListView.h"
#include "views/MessageRowGeometry.h"
#include "views/RoomView.h"
#include "tk_test_surface.h"

#include <tesseract/settings.h>
#include <tesseract/types.h>
#include <tesseract/visual.h>

#include <cmath>
#include <memory>

using namespace tk;
using tesseract::views::MessageRowData;
using tesseract::views::RoomView;
namespace msgbubble = tesseract::views::msgbubble;

namespace
{

struct BubblesLayoutGuard
{
    tesseract::Settings::MessageLayout prev = tesseract::Settings::instance().message_layout;
    bool prev_own_avatar = tesseract::Settings::instance().bubbles_show_own_avatar;
    explicit BubblesLayoutGuard(bool own_avatar)
    {
        tesseract::Settings::instance().message_layout =
            tesseract::Settings::MessageLayout::Bubbles;
        tesseract::Settings::instance().bubbles_show_own_avatar = own_avatar;
    }
    ~BubblesLayoutGuard()
    {
        tesseract::Settings::instance().message_layout = prev;
        tesseract::Settings::instance().bubbles_show_own_avatar = prev_own_avatar;
    }
};

struct Sample
{
    Rect row;
    Color pixel;
};

Sample sample_bubble(RoomView& view, bool own_avatar, bool paint_room)
{
    auto surface = TestSurface::create(1000, 700);
    LayoutCtx lc{surface->factory(), Theme::light()};
    view.measure(lc, {1000, 700});
    view.arrange(lc, {0, 0, 1000, 700});
    auto* list = view.message_list();
    const Rect row = list->row_world_rect(0);
    const float gutter = own_avatar ? msgbubble::kAvatarSize + msgbubble::kAvatarGap : 0.0f;
    const int x = static_cast<int>(row.right() - msgbubble::kEdgePadX - gutter - 5.0f);
    const int y = static_cast<int>(row.y + tesseract::visual::kMsgRowVerticalPad +
                                   (own_avatar ? msgbubble::kAvatarSize : 0.0f) + 10.0f);
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
    if (paint_room)
    {
        view.paint(pc);
    }
    else
    {
        const bool dimmed = list->dimmed();
        list->set_dimmed(false);
        list->paint(pc);
        list->set_dimmed(dimmed);
    }
    return {row, surface->read_pixel(x, y)};
}

bool near_color(Color a, Color b)
{
    return std::abs(int(a.r) - int(b.r)) <= 2 && std::abs(int(a.g) - int(b.g)) <= 2 &&
           std::abs(int(a.b) - int(b.b)) <= 2;
}

} // namespace

TEST_CASE("Side panels cover own bubbles without moving them", "[tk][view][room][bubbles]")
{
    const bool own_avatar = GENERATE(false, true);
    BubblesLayoutGuard guard(own_avatar);
    auto owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Test Room";
    view.set_room(info);
    MessageRowData message;
    message.kind = MessageRowData::Kind::Text;
    message.event_id = "$own:example.org";
    message.sender = "@me:example.org";
    message.sender_name = "Me";
    message.body = "hello";
    message.is_own = true;
    view.set_messages({message});
    REQUIRE(view.message_list() != nullptr);
    view.message_list()->set_content_column_enabled(false);

    const Sample before = sample_bubble(view, own_avatar, false);
    REQUIRE(near_color(before.pixel, Theme::light().palette.bubble_bg_me));

    SECTION("thread list")
    {
        view.set_thread_panel(RoomView::ThreadPanelState::List, "");
    }
    SECTION("open thread")
    {
        view.set_thread_panel(RoomView::ThreadPanelState::Open, "$root:example.org");
    }
    SECTION("room info")
    {
        view.show_room_info();
        REQUIRE(view.room_info_panel()->is_open());
    }

    const Sample under_panel = sample_bubble(view, own_avatar, false);
    CHECK(under_panel.row.x == before.row.x);
    CHECK(under_panel.row.y == before.row.y);
    CHECK(under_panel.row.w == before.row.w);
    CHECK(under_panel.row.h == before.row.h);
    CHECK(near_color(under_panel.pixel, before.pixel));
    CHECK_FALSE(near_color(sample_bubble(view, own_avatar, true).pixel, before.pixel));

    view.set_thread_panel(RoomView::ThreadPanelState::Closed, "");
    view.room_info_panel()->close();
    const Sample after = sample_bubble(view, own_avatar, false);
    CHECK(after.row.h == before.row.h);
    CHECK(near_color(after.pixel, before.pixel));
}
