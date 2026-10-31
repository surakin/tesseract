#pragma once

// CodeBlockExpander — per-message expand/collapse state for long code blocks
// in the timeline. Owns, per event_id, the set of code-block indices (the
// n-th <pre> of the message) the user has expanded; everything defaults to
// collapsed. Modelled on SpoilerRevealer: MessageListView holds one by value,
// the pointer-up / accessibility toggle calls toggle(), and set_messages()
// calls clear() on a room/timeline switch.
//
// state_hash() feeds the body layout cache key so a toggle rebuilds that
// message's layout (and only that message's).

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>

namespace tesseract::views
{

class CodeBlockExpander
{
public:
    bool is_expanded(const std::string& event_id, int block) const
    {
        auto it = expanded_.find(event_id);
        return it != expanded_.end() && it->second.count(block) > 0;
    }

    void toggle(const std::string& event_id, int block)
    {
        auto& s = expanded_[event_id];
        if (!s.erase(block))
            s.insert(block);
        if (s.empty())
            expanded_.erase(event_id);
    }

    // 0 when nothing in this event is expanded.
    std::size_t state_hash(const std::string& event_id) const
    {
        auto it = expanded_.find(event_id);
        if (it == expanded_.end())
            return 0;
        std::size_t h = 0x9e3779b97f4a7c15ULL;
        for (int b : it->second)
            h = h * 1099511628211ULL ^ std::hash<int>{}(b + 1);
        return h;
    }

    void clear() { expanded_.clear(); }

private:
    std::unordered_map<std::string, std::set<int>> expanded_;
};

} // namespace tesseract::views
