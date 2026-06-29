#include <catch2/catch_test_macros.hpp>

#include "app/UnreadPrefetch.h"
#include "tesseract/types.h"

#include <string>
#include <vector>

using tesseract::compute_unread_prefetch_set;
using tesseract::RoomInfo;

namespace
{

// An unread room with a definite recency. unread_count > 0, not muted.
RoomInfo unread_room(const std::string& id, std::uint64_t ts,
                     std::uint64_t unread = 1)
{
    RoomInfo r;
    r.id = id;
    r.unread_count = unread;
    r.last_activity_ts = ts;
    return r;
}

} // namespace

TEST_CASE("prefetch set: all unread non-muted non-current rooms")
{
    std::vector<RoomInfo> rooms;
    rooms.push_back(unread_room("!unread:ex.org", 100)); // quiet unread (Dot)

    RoomInfo notifying;
    notifying.id = "!notifying:ex.org";
    notifying.notification_count = 2;
    notifying.unread_count = 2;
    notifying.last_activity_ts = 150;
    rooms.push_back(notifying); // Count → now included

    RoomInfo read;
    read.id = "!read:ex.org";
    read.unread_count = 0; // read → excluded
    read.last_activity_ts = 200;
    rooms.push_back(read);

    RoomInfo muted = unread_room("!muted:ex.org", 300);
    muted.muted = true; // muted → excluded
    rooms.push_back(muted);

    rooms.push_back(unread_room("!open:ex.org", 400)); // current room → excluded

    auto sel = compute_unread_prefetch_set(rooms, "!open:ex.org", 20);

    // Both the quiet-unread and the notifying room must be included.
    REQUIRE(sel.ids.size() == 2);
    auto has = [&](const std::string& id)
    { return std::find(sel.ids.begin(), sel.ids.end(), id) != sel.ids.end(); };
    REQUIRE(has("!unread:ex.org"));
    REQUIRE(has("!notifying:ex.org"));
}

TEST_CASE("prefetch set: cap drops the least-recently-active rooms")
{
    std::vector<RoomInfo> rooms;
    // 21 unread rooms with ascending timestamps r0..r20 (r20 most recent).
    for (int i = 0; i < 21; ++i)
        rooms.push_back(unread_room("!r" + std::to_string(i) + ":ex.org",
                                    1'000 + static_cast<std::uint64_t>(i)));

    auto sel = compute_unread_prefetch_set(rooms, "", 20);

    REQUIRE(sel.ids.size() == 20);
    // Most-recently-active first; the oldest (r0) is the one dropped.
    REQUIRE(sel.ids.front() == "!r20:ex.org");
    for (const auto& id : sel.ids)
        REQUIRE(id != "!r0:ex.org");
}

TEST_CASE("prefetch set: fingerprint is stable across reorder only")
{
    std::vector<RoomInfo> a;
    a.push_back(unread_room("!a:ex.org", 100));
    a.push_back(unread_room("!b:ex.org", 200));

    // Same rooms + counts, different input order → same capped set → same fp.
    std::vector<RoomInfo> b;
    b.push_back(unread_room("!b:ex.org", 200));
    b.push_back(unread_room("!a:ex.org", 100));

    auto sa = compute_unread_prefetch_set(a, "", 20);
    auto sb = compute_unread_prefetch_set(b, "", 20);

    REQUIRE(sa.fingerprint == sb.fingerprint);
}

TEST_CASE("prefetch set: fingerprint changes when unread_count grows")
{
    std::vector<RoomInfo> before;
    before.push_back(unread_room("!a:ex.org", 100, /*unread=*/1));

    std::vector<RoomInfo> after;
    after.push_back(unread_room("!a:ex.org", 100, /*unread=*/3)); // new messages

    auto sb = compute_unread_prefetch_set(before, "", 20);
    auto sa = compute_unread_prefetch_set(after, "", 20);

    REQUIRE(sb.fingerprint != sa.fingerprint);
}

TEST_CASE("prefetch set: fingerprint changes when notification_count grows")
{
    RoomInfo before;
    before.id = "!a:ex.org";
    before.notification_count = 1;
    before.unread_count = 1;
    before.last_activity_ts = 100;

    RoomInfo after = before;
    after.notification_count = 3; // new mention

    auto sb = compute_unread_prefetch_set({before}, "", 20);
    auto sa = compute_unread_prefetch_set({after}, "", 20);

    REQUIRE(sb.fingerprint != sa.fingerprint);
}

TEST_CASE("prefetch set: fingerprint changes on membership change")
{
    std::vector<RoomInfo> one;
    one.push_back(unread_room("!a:ex.org", 100));

    std::vector<RoomInfo> two;
    two.push_back(unread_room("!a:ex.org", 100));
    two.push_back(unread_room("!b:ex.org", 200));

    auto s1 = compute_unread_prefetch_set(one, "", 20);
    auto s2 = compute_unread_prefetch_set(two, "", 20);

    REQUIRE(s1.fingerprint != s2.fingerprint);
}

TEST_CASE("prefetch set: empty when nothing is unread")
{
    std::vector<RoomInfo> rooms;
    RoomInfo read;
    read.id = "!read:ex.org";
    read.unread_count = 0;
    rooms.push_back(read);

    auto sel = compute_unread_prefetch_set(rooms, "", 20);

    REQUIRE(sel.ids.empty());
    REQUIRE(sel.fingerprint == 0);
}
