#include "anim_image_cache.h"

#include <chrono>

namespace tk
{

std::int64_t AnimImageCache::vis_now_() const
{
    if (clock_)
    {
        return clock_();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void AnimImageCache::store(const std::string& key,
                           std::vector<std::unique_ptr<tk::Image>> frames,
                           std::vector<int> delays_ms, std::int64_t now_ms)
{
    if (frames.empty())
    {
        return;
    }
    Entry entry;
    entry.frames = std::move(frames);
    entry.delays_ms = std::move(delays_ms);
    entry.current = 0;
    entry.next_advance_ms =
        now_ms + (entry.delays_ms.empty() ? 100 : entry.delays_ms[0]);
    // Treat a freshly stored entry as visible so the timer keeps running until
    // the first paint refreshes (or fails to refresh) this stamp.
    entry.last_seen_ms = vis_now_();
    entries_.insert_or_assign(key, std::move(entry));
}

bool AnimImageCache::has(const std::string& key) const
{
    return entries_.count(key) > 0;
}

const tk::Image* AnimImageCache::current_frame(const std::string& key) const
{
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.frames.empty())
    {
        return nullptr;
    }
    it->second.last_seen_ms = vis_now_();
    return it->second.frames[it->second.current].get();
}

bool AnimImageCache::advance(std::int64_t now_ms)
{
    const std::int64_t vis_now = vis_now_();
    bool any = false;
    for (auto& [_, entry] : entries_)
    {
        if (entry.frames.empty() ||
            entry.delays_ms.size() != entry.frames.size())
        {
            continue;
        }
        // Skip entries that have not been painted recently — they are off-
        // screen (scrolled away or in another room) and must not drive
        // repaints. Their deadline is left as-is and resynced on return.
        if (vis_now - entry.last_seen_ms > kVisibilityGraceMs)
        {
            continue;
        }
        // Just came back into view after being skipped for a while: drop the
        // stale backlog so we resume from the current frame instead of fast-
        // forwarding through every frame missed while hidden.
        if (now_ms - entry.next_advance_ms > kVisibilityGraceMs)
        {
            entry.next_advance_ms = now_ms + entry.delays_ms[entry.current];
            continue;
        }
        while (now_ms >= entry.next_advance_ms)
        {
            const bool was_last = (entry.current == entry.frames.size() - 1);
            entry.current = (entry.current + 1) % entry.frames.size();
            entry.next_advance_ms += entry.delays_ms[entry.current];
            any = true;
            if (was_last)
            {
                break; // loop-point frame gets one rendered pass before catch-up continues
            }
        }
    }
    return any;
}

bool AnimImageCache::any_visible() const
{
    const std::int64_t vis_now = vis_now_();
    for (const auto& [_, entry] : entries_)
    {
        if (vis_now - entry.last_seen_ms <= kVisibilityGraceMs)
        {
            return true;
        }
    }
    return false;
}

} // namespace tk
