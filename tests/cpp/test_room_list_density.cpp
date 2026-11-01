#include <catch2/catch_test_macros.hpp>

#include "tesseract/settings.h"
#include "tesseract/types.h"
#include "tk/widget.h"
#include "view_test_util.h"
#include "views/RoomListView.h"

#include <cmath>
#include <memory>
#include <string>

using tesseract::RoomInfo;
using tesseract::views::RoomListView;

namespace
{

RoomInfo density_room(const std::string& id, const std::string& name)
{
    RoomInfo r;
    r.id = id;
    r.name = name;
    r.last_message_kind = "text";
    r.last_message_body = "hello there";
    r.last_message_sender_name = "Alex";
    r.last_activity_ts = 1'000;
    return r;
}

} // namespace

TEST_CASE("RoomListView compact density shrinks rows and toggles back", "[roomlist][density]")
{
    vt::Stage st;
    auto owner = tk::create_root_widget<RoomListView>(&st.host);
    RoomListView& view = *owner;
    view.set_rooms({density_room("!a:x", "Alpha"), density_room("!b:x", "Beta")});
    const tk::Rect bounds{0, 0, 300, 400};
    st.mount(view, bounds);

    const float normal_h = view.room_row_rect_for_test("!a:x").h;
    CHECK(normal_h == 48.0f);

    view.set_compact(true);
    CHECK(view.compact());
    st.relayout_paint(view, bounds);
    const float compact_h = view.room_row_rect_for_test("!a:x").h;
    CHECK(compact_h < normal_h);
    // avatar (21px at the default fonts) + 2 x 4px padding
    const float expected = 21.0f + 8.0f;
    CHECK(compact_h == expected);

    view.set_compact(false);
    st.relayout_paint(view, bounds);
    CHECK(view.room_row_rect_for_test("!a:x").h == normal_h);
}

TEST_CASE("RoomListView compact density works together with icon-only mode", "[roomlist][density]")
{
    vt::Stage st;
    auto owner = tk::create_root_widget<RoomListView>(&st.host);
    RoomListView& view = *owner;
    view.set_rooms({density_room("!a:x", "Alpha")});
    view.set_compact(true);
    view.set_icon_only(true);
    st.mount(view, {0, 0, 64, 400});
    CHECK(view.room_row_rect_for_test("!a:x").h < 48.0f);
    view.set_icon_only(false);
    st.relayout_paint(view, {0, 0, 300, 400});
    CHECK(view.compact());
}

TEST_CASE("Compact rows show the last message as a hover tooltip", "[roomlist][density]")
{
    vt::Stage st;
    auto owner = tk::create_root_widget<RoomListView>(&st.host);
    RoomListView& view = *owner;
    view.set_rooms({density_room("!a:x", "Alpha")});
    const tk::Rect bounds{0, 0, 300, 400};
    st.mount(view, bounds);
    const tk::Rect r = view.room_row_rect_for_test("!a:x");
    const tk::Point inside{r.x + r.w * 0.5f, r.y + r.h * 0.5f};

    // Normal rows already show the preview: no tooltip.
    st.host.dispatch_pointer_move(inside);
    st.relayout_paint(view, bounds);
    CHECK(st.host.tooltip_text_.empty());

    view.set_compact(true);
    st.relayout_paint(view, bounds);
    const tk::Rect cr = view.room_row_rect_for_test("!a:x");
    st.host.dispatch_pointer_move({cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f});
    st.relayout_paint(view, bounds);
    CHECK(st.host.tooltip_text_.find("Alpha") != std::string::npos);
    CHECK(st.host.tooltip_text_.find("Alex: hello there") != std::string::npos);

    // Pointer away: hidden again.
    st.host.dispatch_pointer_move({r.x + r.w * 0.5f, 390.0f});
    st.relayout_paint(view, bounds);
    CHECK(st.host.tooltip_text_.empty());
}
