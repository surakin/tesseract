#pragma once

// Pure decisions for the unread-thread indicator: the per-row dot style in
// ThreadListView and the aggregate dot on the room-header threads button.
// Kept header-only and (bar the ThreadInfo struct) dependency-free so it can
// be unit-tested without constructing any canvas-backed view. Mirrors the
// shape of views/roomlist_unread.h.

#include <vector>

#include "tesseract/types.h"

namespace tesseract::views
{

enum class ThreadDot
{
    None,    ///< read → no dot
    Unread,  ///< unread replies, no ping → small neutral dot
    Mention, ///< an unread reply pings the user → accent-coloured dot
};

/// Per-thread dot style. A mention outranks a plain unread. Inputs come
/// straight from `ThreadInfo::unread` / `ThreadInfo::mentions_me`.
inline ThreadDot thread_dot_for(bool unread, bool mentions_me)
{
    if (unread && mentions_me)
        return ThreadDot::Mention;
    if (unread)
        return ThreadDot::Unread;
    return ThreadDot::None;
}

/// Folded state for the room-header threads button: whether *any* thread in
/// the room is unread, and whether any unread thread also pings the user
/// (which promotes the header dot to the accent colour).
struct ThreadsAggregate
{
    bool any_unread = false;
    bool any_mention = false;
};

inline ThreadsAggregate aggregate_threads(const std::vector<ThreadInfo>& threads)
{
    ThreadsAggregate agg;
    for (const auto& t : threads)
    {
        if (!t.unread)
            continue;
        agg.any_unread = true;
        if (t.mentions_me)
            agg.any_mention = true;
    }
    return agg;
}

} // namespace tesseract::views
