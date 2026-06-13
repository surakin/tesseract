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
    CHECK(classify_room_section(fav, true, 30, kNow) ==
          RoomListView::kSecFavorites);
    auto space = room(false, false, true, kNow - 60 * kDayMs);
    CHECK(classify_room_section(space, true, 30, kNow) ==
          RoomListView::kSecSpaces);
}

TEST_CASE("classify: DMs and Rooms group when inactive", "[roomlist][inactive]")
{
    auto dm_old = room(false, true, false, kNow - 60 * kDayMs);
    auto dm_new = room(false, true, false, kNow - 1 * kDayMs);
    CHECK(classify_room_section(dm_old, true, 30, kNow) ==
          RoomListView::kSecInactive);
    CHECK(classify_room_section(dm_new, true, 30, kNow) ==
          RoomListView::kSecDMs);

    auto room_old = room(false, false, false, kNow - 60 * kDayMs);
    auto room_new = room(false, false, false, kNow - 1 * kDayMs);
    CHECK(classify_room_section(room_old, true, 30, kNow) ==
          RoomListView::kSecInactive);
    CHECK(classify_room_section(room_new, true, 30, kNow) ==
          RoomListView::kSecRooms);
}

TEST_CASE("classify: zero last_activity_ts is not classified as inactive", "[roomlist][inactive]")
{
    // 0 means the SDK hasn't returned a timestamp yet; don't hide the room.
    auto dm = room(false, true, false, 0);
    CHECK(classify_room_section(dm, true, 30, kNow) == RoomListView::kSecDMs);
    auto r = room(false, false, false, 0);
    CHECK(classify_room_section(r, true, 30, kNow) == RoomListView::kSecRooms);
}

TEST_CASE("classify: grouping off keeps normal sections", "[roomlist][inactive]")
{
    auto dm_old = room(false, true, false, kNow - 60 * kDayMs);
    CHECK(classify_room_section(dm_old, false, 30, kNow) ==
          RoomListView::kSecDMs);
}

TEST_CASE("classify: threshold boundary is strict", "[roomlist][inactive]")
{
    auto at = room(false, false, false, kNow - 30 * kDayMs);
    auto past = room(false, false, false, kNow - 30 * kDayMs - 1);
    CHECK(classify_room_section(at, true, 30, kNow) == RoomListView::kSecRooms);
    CHECK(classify_room_section(past, true, 30, kNow) ==
          RoomListView::kSecInactive);
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
