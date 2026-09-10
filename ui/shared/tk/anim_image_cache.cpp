#include "anim_image_cache.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace tk
{

AnimImageCache::AnimImageCache(std::size_t max_bytes, std::int64_t ttl_ms)
    : max_bytes_(max_bytes), ttl_ms_(ttl_ms)
{
}

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
    std::lock_guard<std::mutex> lock(mu_);
    Entry entry;
    entry.frames = std::move(frames);
    entry.delays_ms = std::move(delays_ms);
    entry.current = 0;
    entry.next_advance_ms =
        now_ms + (entry.delays_ms.empty() ? 100 : entry.delays_ms[0]);
    // Treat a freshly stored entry as visible so the timer keeps running until
    // the first paint refreshes (or fails to refresh) this stamp.
    entry.last_seen_ms = vis_now_();
    for (const auto& f : entry.frames)
    {
        entry.bytes += f ? f->memory_bytes() : 0;
    }

    if (auto it = entries_.find(key); it != entries_.end())
    {
        current_bytes_ -= it->second.bytes;
    }
    current_bytes_ += entry.bytes;
    entries_.insert_or_assign(key, std::move(entry));
}

bool AnimImageCache::has(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.count(key) > 0;
}

const tk::Image* AnimImageCache::current_frame(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.frames.empty())
    {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    it->second.last_seen_ms = vis_now_();
    return it->second.frames[it->second.current].get();
}

bool AnimImageCache::advance(std::int64_t now_ms)
{
    std::lock_guard<std::mutex> lock(mu_);
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
            // Clamp to >=1ms: 0ms frame delays (some encoders emit them) leave
            // next_advance_ms perpetually due. The was_last break caps it at one
            // cycle per tick, but frames still flash past and waste CPU.
            int delay = entry.delays_ms[entry.current];
            if (delay < 1)
            {
                delay = 1;
            }
            entry.next_advance_ms += delay;
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
    std::lock_guard<std::mutex> lock(mu_);
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

void AnimImageCache::sweep()
{
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t vis_now = vis_now_();

    // 1) Drop entries not painted within the TTL window.
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (vis_now - it->second.last_seen_ms > ttl_ms_)
        {
            current_bytes_ -= it->second.bytes;
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (current_bytes_ <= max_bytes_)
    {
        return;
    }

    // 2) Still over budget: evict least-recently-seen off-screen entries
    //    oldest-first until under budget. Visible entries are kept.
    std::vector<std::unordered_map<std::string, Entry>::iterator> evictable;
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        if (vis_now - it->second.last_seen_ms > kVisibilityGraceMs)
        {
            evictable.push_back(it);
        }
    }
    std::sort(evictable.begin(), evictable.end(),
              [](const auto& a, const auto& b)
              { return a->second.last_seen_ms < b->second.last_seen_ms; });

    for (auto it : evictable)
    {
        if (current_bytes_ <= max_bytes_)
        {
            break;
        }
        current_bytes_ -= it->second.bytes;
        entries_.erase(it);
    }
}

void AnimImageCache::clear()
{
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
    current_bytes_ = 0;
    hits_           = 0;
    misses_         = 0;
}

bool AnimImageCache::empty() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.empty();
}

std::size_t AnimImageCache::current_bytes() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return current_bytes_;
}

std::size_t AnimImageCache::hits() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
}

std::size_t AnimImageCache::misses() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return misses_;
}

} // namespace tk
