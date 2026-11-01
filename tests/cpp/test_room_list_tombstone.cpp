#include <catch2/catch_test_macros.hpp>

#include "tesseract/types.h"
#include "tk/widget.h"
#include "view_test_util.h"
#include "views/RoomListView.h"

#include <memory>
#include <string>

using tesseract::RoomInfo;
using tesseract::views::RoomListView;

namespace
{

RoomInfo room(const std::string& id, const std::string& name)
{
    RoomInfo r;
    r.id = id;
    r.name = name;
    r.last_activity_ts = 1'000;
    return r;
}

RoomInfo upgraded(const std::string& id, const std::string& name, const std::string& successor,
                  bool successor_joined)
{
    RoomInfo r = room(id, name);
    r.successor_room_id = successor;
    r.successor_joined = successor_joined;
    return r;
}

} // namespace

TEST_CASE("RoomListView hides an upgraded room only once its successor is joined",
          "[roomlist][tombstone]")
{
    vt::Stage st;
    auto owner = tk::create_root_widget<RoomListView>(&st.host);
    RoomListView& view = *owner;
    view.set_rooms({room("!new:x", "New"), upgraded("!oldj:x", "Old joined", "!new:x", true),
                    upgraded("!oldu:x", "Old unjoined", "!elsewhere:x", false)});
    const tk::Rect bounds{0, 0, 300, 400};
    st.mount(view, bounds);

    CHECK(view.room_row_rect_for_test("!new:x").h > 0.0f);
    // Successor joined: a dead end, not listed.
    CHECK(view.room_row_rect_for_test("!oldj:x").empty());
    // Successor not joined: still listed, it is the way to the new room.
    CHECK(view.room_row_rect_for_test("!oldu:x").h > 0.0f);
}
