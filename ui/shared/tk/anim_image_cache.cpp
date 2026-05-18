#include "anim_image_cache.h"

namespace tk
{

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
    return it->second.frames[it->second.current].get();
}

bool AnimImageCache::advance(std::int64_t now_ms)
{
    bool any = false;
    for (auto& [_, entry] : entries_)
    {
        if (entry.frames.empty() ||
            entry.delays_ms.size() != entry.frames.size())
        {
            continue;
        }
        while (now_ms >= entry.next_advance_ms)
        {
            const bool was_last =
                (entry.current == entry.frames.size() - 1);
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

} // namespace tk
