#include <catch2/catch_test_macros.hpp>

#include "tesseract/settings.h"
#include "tesseract/types.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/RoomListView.h"
#include "tk_test_surface.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using tesseract::RoomInfo;
using tesseract::views::RoomListView;

namespace
{

// Lay out (measure + arrange) without painting. arrange() rebuilds row heights
// — clearing heights_dirty_ so visible_range()/visible_room_ids() are valid —
// and consumes any deferred scroll request set during set_rooms().
struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 240);

    void layout(tk::Widget& w, tk::Rect bounds)
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        w.measure(lc, {bounds.w, bounds.h});
        w.arrange(lc, bounds);
    }
};

// A plain room (goes to the Rooms section). ts gives it a definite recency.
RoomInfo plain_room(int i, std::uint64_t ts)
{
    RoomInfo r;
    r.id = "$r" + std::to_string(i);
    r.name = "Room " + std::to_string(i);
    r.last_activity_ts = ts;
    return r;
}

// 40 read plain rooms, ascending timestamps, ids $r0..$r39.
std::vector<RoomInfo> read_rooms(int n = 40)
{
    std::vector<RoomInfo> v;
    for (int i = 0; i < n; ++i)
        v.push_back(plain_room(i, 1'000 + static_cast<std::uint64_t>(i)));
    return v;
}

bool visible(RoomListView& view, const std::string& id)
{
    auto ids = view.visible_room_ids();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Small viewport so only a handful of the 40 rows fit; $r39 starts off-screen.
constexpr tk::Rect kBounds{0, 0, 280, 200};

void reset_settings(bool autoscroll)
{
    auto& s = tesseract::Settings::instance();
    s.autoscroll_unread_rooms = autoscroll;
    s.group_inactive_rooms = false; // keep all rooms in the Rooms section
}

} // namespace

TEST_CASE("autoscroll: all-read list does not scroll on first load",
          "[roomlist][autoscroll]")
{
    reset_settings(true);
    Stage st;
    RoomListView view;
    view.set_rooms(read_rooms());
    st.layout(view, kBounds);

    REQUIRE_FALSE(view.visible_room_ids().empty()); // some rows are visible
    CHECK_FALSE(visible(view, "$r39"));             // bottom room stays hidden
}

TEST_CASE("autoscroll: most-recent unread room is scrolled into view",
          "[roomlist][autoscroll]")
{
    reset_settings(true);
    Stage st;
    RoomListView view;

    view.set_rooms(read_rooms());
    st.layout(view, kBounds);
    REQUIRE_FALSE(visible(view, "$r39"));

    // $r39 receives new messages with the newest timestamp.
    auto rooms = read_rooms();
    rooms[39].notification_count = 1;
    rooms[39].last_activity_ts = 99'999;
    view.set_rooms(std::move(rooms));
    st.layout(view, kBounds);

    CHECK(visible(view, "$r39"));
}

TEST_CASE("autoscroll: first load with an unread room scrolls to it",
          "[roomlist][autoscroll]")
{
    reset_settings(true);
    Stage st;
    RoomListView view;

    auto rooms = read_rooms();
    rooms[39].notification_count = 1;
    rooms[39].last_activity_ts = 99'999;
    view.set_rooms(std::move(rooms)); // very first set_rooms
    st.layout(view, kBounds);

    CHECK(visible(view, "$r39"));
}

TEST_CASE("autoscroll: among several unread, the most recent wins",
          "[roomlist][autoscroll]")
{
    reset_settings(true);
    Stage st;
    RoomListView view;

    auto rooms = read_rooms();
    rooms[10].notification_count = 1;
    rooms[10].last_activity_ts = 50'000; // older unread, mid-list
    rooms[39].notification_count = 1;
    rooms[39].last_activity_ts = 99'999; // newer unread, bottom
    view.set_rooms(std::move(rooms));
    st.layout(view, kBounds);

    CHECK(visible(view, "$r39")); // scrolled to the newest, not $r10
}

TEST_CASE("autoscroll: a space with unread children is considered",
          "[roomlist][autoscroll]")
{
    reset_settings(true);
    Stage st;
    RoomListView view;

    // 20 read rooms, then a space row (Spaces section, bottom of the list).
    auto rooms = read_rooms(20);
    RoomInfo space;
    space.id = "$space";
    space.name = "My Space";
    space.is_space = true;
    space.notification_count = 3;   // aggregate of unread children
    space.last_activity_ts = 99'999; // newest activity among children
    rooms.push_back(space);

    view.set_rooms(std::move(rooms));
    st.layout(view, kBounds);

    CHECK(visible(view, "$space"));
}

TEST_CASE("autoscroll: low-priority unread does not grab the scroll",
          "[roomlist][autoscroll]")
{
    reset_settings(true);
    Stage st;
    RoomListView view;

    auto rooms = read_rooms();
    rooms[39].notification_count = 1;
    rooms[39].last_activity_ts = 99'999;
    rooms[39].is_low_priority = true; // excluded from candidates
    view.set_rooms(std::move(rooms));
    st.layout(view, kBounds);

    CHECK_FALSE(visible(view, "$r39")); // no scroll: it stays off-screen
}

TEST_CASE("autoscroll: disabled setting suppresses scrolling",
          "[roomlist][autoscroll]")
{
    reset_settings(false); // feature off
    Stage st;
    RoomListView view;

    auto rooms = read_rooms();
    rooms[39].notification_count = 1;
    rooms[39].last_activity_ts = 99'999;
    view.set_rooms(std::move(rooms));
    st.layout(view, kBounds);

    CHECK_FALSE(visible(view, "$r39"));
}

TEST_CASE("autoscroll: already-visible unread does not move the list",
          "[roomlist][autoscroll]")
{
    reset_settings(true);
    Stage st;
    RoomListView view;

    auto rooms = read_rooms();
    rooms[0].notification_count = 1;    // top room, already visible
    rooms[0].last_activity_ts = 99'999;
    view.set_rooms(std::move(rooms));
    st.layout(view, kBounds);

    CHECK(visible(view, "$r0"));         // still at the top
    CHECK_FALSE(visible(view, "$r39"));  // list did not jump to the bottom
}
