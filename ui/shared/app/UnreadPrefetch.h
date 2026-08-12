#pragma once

#include <tesseract/types.h>

#include "tk/hash_combine.h"
#include "views/roomlist_unread.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tesseract
{

// Result of compute_unread_prefetch_set(): the rooms to one-shot prefetch
// (already capped + LRU-ordered, most-recently-active first) plus a fingerprint
// the shell compares against the last one it acted on, so the FFI call only
// fires when the prefetch-relevant set actually changes.
struct UnreadPrefetchSet
{
    std::vector<std::string> ids;
    std::size_t              fingerprint = 0;
};

// Select the rooms to one-shot prefetch from the current room list:
//   - every favorite room (explicitly marked by the user, not a heuristic) —
//     always included, uncapped, regardless of read/mute state, mirroring
//     ShellBase::prune_warm_subscriptions_'s unconditional favorite exemption
//     from the warm-LRU cap
//   - when `include_unread` (gated by the caller on the prefetch_unread_rooms
//     setting), also all other unread rooms —
//     `unread_style_for(...) != UnreadStyle::None`, covering quiet (Dot),
//     notifying (Count), and mention (Mention) rooms, excluding muted —
//     sorted most-recently-active first (LRU) and capped at `cap`
//   - exclude the currently-open room (already subscribed / warm) from both
//
// The fingerprint XOR-combines each surviving (id, notification_count, unread_count)
// triple — mirroring the codebase's known_users_room_set_hash_ idiom. It is
// order-independent (a pure reorder among the capped set does NOT re-fire) but
// mixes in both counts so new messages or a new mention in an already-prefetched
// room DO re-fire the warm-up. An empty set yields fingerprint 0.
inline UnreadPrefetchSet
compute_unread_prefetch_set(const std::vector<RoomInfo>& rooms,
                           const std::string&           current_room_id,
                           std::size_t                  cap,
                           bool                          include_unread = true)
{
    std::vector<const RoomInfo*> selected;
    selected.reserve(rooms.size());

    for (const auto& r : rooms)
    {
        if (r.id == current_room_id)
            continue;
        if (r.is_favorite)
            selected.push_back(&r);
    }

    if (include_unread)
    {
        std::vector<const RoomInfo*> unread;
        unread.reserve(rooms.size());
        for (const auto& r : rooms)
        {
            if (r.id == current_room_id || r.is_favorite) // favorites already selected
                continue;
            if (views::unread_style_for(r.notification_count, r.highlight_count,
                                        r.unread_count, r.muted) ==
                views::UnreadStyle::None)
                continue;
            unread.push_back(&r);
        }

        // Only the most-recently-active `cap` rooms are warmed; the sort is
        // needed only when there are more candidates than the cap. The common
        // case (<= cap unread rooms) skips the O(n log n) sort on every sync
        // tick.
        if (unread.size() > cap)
        {
            std::partial_sort(unread.begin(), unread.begin() + cap, unread.end(),
                              [](const RoomInfo* a, const RoomInfo* b)
                              { return a->last_activity_ts > b->last_activity_ts; });
            unread.resize(cap);
        }
        selected.insert(selected.end(), unread.begin(), unread.end());
    }

    UnreadPrefetchSet out;
    out.ids.reserve(selected.size());
    for (const auto* r : selected)
    {
        std::size_t h = tk::hash_combine(
            tk::hash_combine(std::hash<std::string>{}(r->id),
                             std::hash<std::uint64_t>{}(r->notification_count)),
            std::hash<std::uint64_t>{}(r->unread_count));
        out.fingerprint ^= h;
        out.ids.push_back(r->id);
    }
    return out;
}

} // namespace tesseract
