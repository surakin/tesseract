#pragma once

// MembershipGroupExpander — expand/collapse state for grouped
// m.room.member timeline rows, extracted from MessageListView. A tiny
// state container that owns the set of group keys (the event_id of the
// FIRST row in a collapsed run) the user has expanded to show every
// member's own line.
//
// MessageListView holds one of these by value: measure/paint read
// is_expanded(key) to pick summary-vs-per-row rendering, on_pointer_up
// calls toggle(key) when a clean click lands on a row belonging to a
// multi-member group, and set_messages() calls clear() on a room/timeline
// switch (matching SpoilerRevealer's lifecycle).
//
// The pointer-press FSM state (press_membership_group_ /
// press_membership_group_key_) stays on MessageListView — this
// collaborator owns only the expanded set.

#include <string>
#include <unordered_set>

namespace tesseract::views
{

class MembershipGroupExpander
{
public:
    bool is_expanded(const std::string& group_key) const
    {
        return expanded_.count(group_key) > 0;
    }

    void toggle(const std::string& group_key)
    {
        if (!expanded_.insert(group_key).second)
            expanded_.erase(group_key);
    }

    void clear() { expanded_.clear(); }

private:
    std::unordered_set<std::string> expanded_;
};

} // namespace tesseract::views
