#pragma once

// SpoilerRevealer — MSC2010 (`data-mx-spoiler`) click-to-reveal state extracted
// from MessageListView. A tiny state container that owns the set of event_ids
// whose spoiler blocks have been revealed by the user.
//
// MessageListView holds one of these by value: paint reads is_revealed(eid) to
// decide whether to draw a spoiler block obscured or revealed, on_pointer_up
// calls reveal(eid) when a spoiler tap completes, and set_messages() calls
// clear() on a room/timeline switch (matching the original behavior).
//
// The pointer-press FSM state (press_spoiler_ / press_spoiler_eid_) stays on
// MessageListView — this collaborator owns only the revealed set.

#include <string>
#include <unordered_set>

namespace tesseract::views
{

class SpoilerRevealer
{
public:
    bool is_revealed(const std::string& event_id) const
    {
        return revealed_.count(event_id) > 0;
    }

    void reveal(const std::string& event_id) { revealed_.insert(event_id); }

    void clear() { revealed_.clear(); }

private:
    std::unordered_set<std::string> revealed_;
};

} // namespace tesseract::views
