#include <catch2/catch_test_macros.hpp>
#include "tesseract/types.h"
#include "views/RoomListView.h"

using tesseract::RoomInfo;
using namespace tesseract::views;

namespace
{
constexpr uint64_t kNow = 1'000'000'000'000ULL; // fixed "now" in ms
constexpr uint64_t kDayMs = 86'400'000ULL;

RoomInfo room(bool fav, bool dm, bool space, uint64_t last_ts)
{
    RoomInfo r;
    r.is_favorite = fav;
    r.is_direct = dm;
    r.is_space = space;
    r.last_activity_ts = last_ts;
    return r;
}
} // namespace

TEST_CASE("classify: favorites/spaces never grouped", "[roomlist][inactive]")
{
    auto fav = room(true, false, false, kNow - 60 * kDayMs);
    CHECK(classify_room_section(fav, false, true, 30, kNow) ==
          RoomListView::kSecFavorites);
    auto space = room(false, false, true, kNow - 60 * kDayMs);
    CHECK(classify_room_section(space, false, true, 30, kNow) ==
          RoomListView::kSecSpaces);
}

TEST_CASE("classify: DMs and Rooms group when inactive", "[roomlist][inactive]")
{
    auto dm_old = room(false, true, false, kNow - 60 * kDayMs);
    auto dm_new = room(false, true, false, kNow - 1 * kDayMs);
    CHECK(classify_room_section(dm_old, false, true, 30, kNow) ==
          RoomListView::kSecInactive);
    CHECK(classify_room_section(dm_new, false, true, 30, kNow) ==
          RoomListView::kSecDMs);

    auto room_old = room(false, false, false, kNow - 60 * kDayMs);
    auto room_new = room(false, false, false, kNow - 1 * kDayMs);
    CHECK(classify_room_section(room_old, false, true, 30, kNow) ==
          RoomListView::kSecInactive);
    CHECK(classify_room_section(room_new, false, true, 30, kNow) ==
          RoomListView::kSecRooms);
}

TEST_CASE("classify: zero last_activity_ts is not classified as inactive", "[roomlist][inactive]")
{
    // 0 means the SDK hasn't returned a timestamp yet; don't hide the room.
    auto dm = room(false, true, false, 0);
    CHECK(classify_room_section(dm, false, true, 30, kNow) == RoomListView::kSecDMs);
    auto r = room(false, false, false, 0);
    CHECK(classify_room_section(r, false, true, 30, kNow) == RoomListView::kSecRooms);
}

TEST_CASE("classify: grouping off keeps normal sections", "[roomlist][inactive]")
{
    auto dm_old = room(false, true, false, kNow - 60 * kDayMs);
    CHECK(classify_room_section(dm_old, false, false, 30, kNow) ==
          RoomListView::kSecDMs);
}

TEST_CASE("classify: threshold boundary is strict", "[roomlist][inactive]")
{
    auto at = room(false, false, false, kNow - 30 * kDayMs);
    auto past = room(false, false, false, kNow - 30 * kDayMs - 1);
    CHECK(classify_room_section(at, false, true, 30, kNow) == RoomListView::kSecRooms);
    CHECK(classify_room_section(past, false, true, 30, kNow) ==
          RoomListView::kSecInactive);
}

TEST_CASE("classify: unread rooms go to kSecUnread when group_unread=true",
          "[roomlist][unread]")
{
    // Room with notification_count > 0
    RoomInfo r_notif;
    r_notif.notification_count = 1;
    CHECK(classify_room_section(r_notif, true, false, 30, kNow) ==
          RoomListView::kSecUnread);

    // Room with highlight_count > 0
    RoomInfo r_highlight;
    r_highlight.highlight_count = 2;
    CHECK(classify_room_section(r_highlight, true, false, 30, kNow) ==
          RoomListView::kSecUnread);

    // Room with quiet unread (unread_count > 0, not muted)
    RoomInfo r_quiet;
    r_quiet.unread_count = 3;
    r_quiet.muted = false;
    CHECK(classify_room_section(r_quiet, true, false, 30, kNow) ==
          RoomListView::kSecUnread);

    // DM with unread also goes to kSecUnread
    RoomInfo r_dm;
    r_dm.is_direct = true;
    r_dm.notification_count = 1;
    CHECK(classify_room_section(r_dm, true, false, 30, kNow) ==
          RoomListView::kSecUnread);
}

TEST_CASE("classify: muted room with only quiet unread stays in normal section",
          "[roomlist][unread]")
{
    RoomInfo r;
    r.unread_count = 5;
    r.muted = true;
    // notification_count == 0, highlight_count == 0 → UnreadStyle::None → no kSecUnread
    CHECK(classify_room_section(r, true, false, 30, kNow) ==
          RoomListView::kSecRooms);
}

TEST_CASE("classify: favorites never go to kSecUnread", "[roomlist][unread]")
{
    RoomInfo r;
    r.is_favorite = true;
    r.notification_count = 5;
    CHECK(classify_room_section(r, true, false, 30, kNow) ==
          RoomListView::kSecFavorites);
}

TEST_CASE("classify: spaces never go to kSecUnread", "[roomlist][unread]")
{
    RoomInfo r;
    r.is_space = true;
    r.notification_count = 5;
    CHECK(classify_room_section(r, true, false, 30, kNow) ==
          RoomListView::kSecSpaces);
}

TEST_CASE("classify: inactive+unread room goes to kSecUnread (unread wins)",
          "[roomlist][unread]")
{
    // A room that is old enough for Inactive AND has unread → Unread wins.
    RoomInfo r;
    r.notification_count = 2;
    r.last_activity_ts = kNow - 60 * kDayMs; // old enough for inactive
    CHECK(classify_room_section(r, true, true, 30, kNow) ==
          RoomListView::kSecUnread);
}

TEST_CASE("classify: group_unread=false keeps normal routing even with unread",
          "[roomlist][unread]")
{
    RoomInfo r;
    r.notification_count = 5;
    CHECK(classify_room_section(r, false, false, 30, kNow) ==
          RoomListView::kSecRooms);

    RoomInfo dm;
    dm.is_direct = true;
    dm.notification_count = 5;
    CHECK(classify_room_section(dm, false, false, 30, kNow) ==
          RoomListView::kSecDMs);
}

namespace
{
tesseract::RoomSummary unjoined_summary(const std::string& id,
                                        const std::string& name,
                                        uint32_t members = 5)
{
    tesseract::RoomSummary s;
    s.room_id            = id;
    s.name               = name;
    s.num_joined_members = members;
    s.join_rule          = "public";
    return s;
}
} // namespace

TEST_CASE("set_space_unjoined_rooms: section appears + clears",
          "[roomlist][space-unjoined]")
{
    RoomListView v;
    CHECK(v.visible_room_ids().empty());

    std::vector<tesseract::RoomSummary> summaries;
    summaries.push_back(unjoined_summary("!a:s", "Alpha"));
    summaries.push_back(unjoined_summary("!b:s", "Beta"));
    v.set_space_unjoined_rooms(std::move(summaries));

    CHECK(v.unjoined_room_count() == 2);
    CHECK(v.unjoined_rows_visible() == true);

    v.clear_space_unjoined_rooms();
    CHECK(v.unjoined_room_count() == 0);
    CHECK(v.unjoined_rows_visible() == false);
}

TEST_CASE("set_space_unjoined_rooms: collapsed hides rows",
          "[roomlist][space-unjoined]")
{
    RoomListView v;
    std::vector<tesseract::RoomSummary> summaries;
    summaries.push_back(unjoined_summary("!a:s", "Alpha"));
    v.set_space_unjoined_rooms(std::move(summaries));

    v.set_section_collapsed(RoomListView::kSecSpaceUnjoined, true);
    CHECK(v.unjoined_room_count() == 1); // data unchanged
    CHECK(v.unjoined_rows_visible() == false);

    v.set_section_collapsed(RoomListView::kSecSpaceUnjoined, false);
    CHECK(v.unjoined_rows_visible() == true);
}

// ─── filter_root_rooms tests ──────────────────────────────────────────────

namespace
{
// Build a minimal space-child cache: space "S" → child "C"
std::unordered_map<std::string, std::vector<std::string>> single_space_cache(
    const std::string& space_id, const std::string& child_id)
{
    return {{space_id, {child_id}}};
}

RoomInfo dm_room(const std::string& id)
{
    RoomInfo r;
    r.id = id;
    r.is_direct = true;
    return r;
}

RoomInfo space_room(const std::string& id)
{
    RoomInfo r;
    r.id = id;
    r.is_space = true;
    return r;
}

RoomInfo child_room(const std::string& id, bool unread = false)
{
    RoomInfo r;
    r.id = id;
    if (unread)
        r.notification_count = 1;
    return r;
}

RoomInfo fav_child_room(const std::string& id)
{
    RoomInfo r;
    r.id = id;
    r.is_favorite = true;
    return r;
}
} // namespace

TEST_CASE("filter_root_rooms: non-child room always included", "[roomlist][filter]")
{
    std::vector<RoomInfo> rooms = {dm_room("dm1")};
    auto sc = single_space_cache("S", "C");
    auto out = filter_root_rooms(rooms, sc, false);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "dm1");
}

TEST_CASE("filter_root_rooms: space-child excluded by default", "[roomlist][filter]")
{
    auto sp   = space_room("S");
    auto ch   = child_room("C");
    std::vector<RoomInfo> rooms = {sp, ch};
    auto sc = single_space_cache("S", "C");
    auto out = filter_root_rooms(rooms, sc, false);
    // Space itself is included; child is excluded.
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "S");
}

TEST_CASE("filter_root_rooms: space-child with unread included when group_unread on", "[roomlist][filter]")
{
    auto sp   = space_room("S");
    auto ch   = child_room("C", /*unread=*/true);
    std::vector<RoomInfo> rooms = {sp, ch};
    auto sc = single_space_cache("S", "C");
    // group_unread=true → unread space-child bypasses filter
    auto out = filter_root_rooms(rooms, sc, true);
    REQUIRE(out.size() == 2); // both space and child
    bool found_child = false;
    for (const auto& r : out) if (r.id == "C") found_child = true;
    CHECK(found_child);
}

TEST_CASE("filter_root_rooms: space-child without unread still excluded when group_unread on",
          "[roomlist][filter]")
{
    auto sp   = space_room("S");
    auto ch   = child_room("C", /*unread=*/false);
    std::vector<RoomInfo> rooms = {sp, ch};
    auto sc = single_space_cache("S", "C");
    auto out = filter_root_rooms(rooms, sc, true);
    // No unread → still excluded even when group_unread=true
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "S");
}

TEST_CASE("filter_root_rooms: favorite space-child always included", "[roomlist][filter]")
{
    auto sp   = space_room("S");
    auto fav  = fav_child_room("C");
    std::vector<RoomInfo> rooms = {sp, fav};
    auto sc = single_space_cache("S", "C");
    auto out = filter_root_rooms(rooms, sc, false);
    REQUIRE(out.size() == 2); // space + favorite child
}

TEST_CASE("filter_root_rooms: nested space-child space hidden when not top-level", "[roomlist][filter]")
{
    // S1 is a top-level space; S2 is a child space of S1.
    // S2 should be excluded from root view.
    auto s1   = space_room("S1");
    auto s2   = space_room("S2");
    s2.id     = "S2";
    std::vector<RoomInfo> rooms = {s1, s2};
    std::unordered_map<std::string, std::vector<std::string>> sc = {{"S1", {"S2"}}};
    auto out = filter_root_rooms(rooms, sc, false);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "S1");
}

TEST_CASE("filter_root_rooms: muted space-child not included (unread_style=None)", "[roomlist][filter]")
{
    auto sp   = space_room("S");
    RoomInfo ch;
    ch.id = "C";
    ch.unread_count      = 5;  // quiet unreads only
    ch.notification_count = 0;
    ch.highlight_count   = 0;
    ch.muted             = true;  // muted → unread_style_for returns None
    std::vector<RoomInfo> rooms = {sp, ch};
    auto sc = single_space_cache("S", "C");
    // Even with group_unread=true, muted+quiet → excluded
    auto out = filter_root_rooms(rooms, sc, true);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "S");
}
