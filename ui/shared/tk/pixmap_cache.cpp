#include "tk/pixmap_cache.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace tk
{

PixmapCache::PixmapCache(std::size_t max_bytes, std::chrono::seconds ttl)
    : max_bytes_(max_bytes), ttl_(ttl)
{
}

std::chrono::steady_clock::time_point PixmapCache::now_() const
{
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

ImageRef PixmapCache::store(const std::string& key, std::unique_ptr<Image> img)
{
    if (!img)
    {
        return nullptr;
    }

    const std::size_t bytes = img->memory_bytes();
    ImageRef ref = std::move(img);

    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        current_bytes_ -= it->second.bytes;
        it->second.img = ref;
        it->second.bytes = bytes;
        it->second.last_use = now_();
    }
    else
    {
        entries_.emplace(key, Entry{ref, bytes, now_()});
    }
    current_bytes_ += bytes;
    return ref;
}

ImageRef PixmapCache::acquire(const std::string& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return nullptr;
    }
    it->second.last_use = now_();
    return it->second.img;
}

const Image* PixmapCache::peek(const std::string& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return nullptr;
    }
    it->second.last_use = now_();
    return it->second.img.get();
}

bool PixmapCache::contains(const std::string& key) const
{
    return entries_.find(key) != entries_.end();
}

void PixmapCache::evict(const std::string& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return;
    }
    // Only the cache may reference it; otherwise it is displayed somewhere.
    if (it->second.img.use_count() > 1)
    {
        return;
    }
    current_bytes_ -= it->second.bytes;
    entries_.erase(it);
}

void PixmapCache::clear()
{
    entries_.clear();
    current_bytes_ = 0;
}

void PixmapCache::sweep()
{
    const auto now = now_();

    // 1) Drop expired entries the cache alone references.
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        const bool unreferenced = it->second.img.use_count() == 1;
        const bool expired = (now - it->second.last_use) > ttl_;
        if (unreferenced && expired)
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

    // 2) Still over budget: evict least-recently-used unreferenced entries
    //    oldest-first until we fit. Pinned (displayed) entries are skipped.
    std::vector<std::unordered_map<std::string, Entry>::iterator> evictable;
    evictable.reserve(entries_.size());
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        if (it->second.img.use_count() == 1)
        {
            evictable.push_back(it);
        }
    }
    std::sort(evictable.begin(), evictable.end(),
              [](const auto& a, const auto& b)
              { return a->second.last_use < b->second.last_use; });

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

} // namespace tk
