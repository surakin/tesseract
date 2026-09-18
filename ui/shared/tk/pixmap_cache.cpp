#include "tk/pixmap_cache.h"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tk
{

namespace
{
std::mutex& registry_mutex()
{
    static std::mutex m;
    return m;
}
std::unordered_set<const PixmapCache*>& registry()
{
    static std::unordered_set<const PixmapCache*> r;
    return r;
}
} // namespace

PixmapCache::PixmapCache(std::size_t max_bytes, std::chrono::seconds ttl)
    : max_bytes_(max_bytes), ttl_(ttl)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().insert(this);
}

PixmapCache::~PixmapCache()
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().erase(this);
}

std::size_t PixmapCache::total_bytes_all_instances()
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    std::size_t total = 0;
    for (const PixmapCache* c : registry())
        total += c->current_bytes();
    return total;
}

void PixmapCache::refresh_bytes_locked_()
{
    std::size_t total = 0;
    for (auto& [_, e] : entries_)
    {
        e.bytes = e.img ? e.img->memory_bytes() : 0;
        total += e.bytes;
    }
    current_bytes_ = total;
}

std::chrono::steady_clock::time_point PixmapCache::now_() const
{
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

ImageRef PixmapCache::store(const CacheKey& key, std::unique_ptr<Image> img)
{
    if (!img)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mu_);

    const std::size_t bytes = img->memory_bytes();
    ImageRef ref = std::move(img);

    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        current_bytes_ -= it->second.bytes;
        it->second.img = ref;
        it->second.bytes = bytes;
        it->second.last_use = now_();
        it->second.last_peek_gen = gen_;
    }
    else
    {
        // A freshly stored image is on screen right now (that is why it was
        // fetched) — stamp the current generation so the GC does not reclaim it
        // before its first paint.
        entries_.emplace(key, Entry{ref, bytes, now_(), gen_});
    }
    current_bytes_ += bytes;
    return ref;
}

ImageRef PixmapCache::acquire(const CacheKey& key)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    it->second.last_use = now_();
    it->second.last_peek_gen = gen_;
    return it->second.img;
}

const Image* PixmapCache::peek(const CacheKey& key)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    it->second.last_use = now_();
    it->second.last_peek_gen = gen_;
    return it->second.img.get();
}

bool PixmapCache::contains(const CacheKey& key) const
{
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.find(key) != entries_.end();
}

void PixmapCache::evict(const CacheKey& key)
{
    std::lock_guard<std::mutex> lock(mu_);
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
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
    current_bytes_ = 0;
    hits_          = 0;
    misses_        = 0;
}

void PixmapCache::retain_recent(unsigned keep)
{
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = now_();
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        const bool unreferenced = it->second.img.use_count() == 1;
        // Not marked (peek()'d) within the last `keep` GC generations...
        const bool gen_stale = (gen_ - it->second.last_peek_gen) >= keep;
        // ...AND not touched (stored or peek()'d) recently. The wall-clock
        // floor protects an image that was just fetched to be displayed but
        // whose widget has not laid out / painted yet (e.g. the room media
        // grid on first open, still showing "Loading…"), which the
        // generation check alone would reclaim before its first paint.
        const bool time_stale = (now - it->second.last_use) > ttl_;
        if (unreferenced && gen_stale && time_stale)
        {
            current_bytes_ -= it->second.bytes;
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void PixmapCache::sweep()
{
    std::lock_guard<std::mutex> lock(mu_);
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

    refresh_bytes_locked_();
    if (current_bytes_ <= max_bytes_)
    {
        return;
    }

    // 2) Still over budget: evict least-recently-used unreferenced entries
    //    oldest-first until we fit. Pinned (displayed) entries are skipped.
    std::vector<std::unordered_map<CacheKey, Entry, CacheKeyHash>::iterator> evictable;
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

void PixmapCache::advance_generation()
{
    std::lock_guard<std::mutex> lock(mu_);
    ++gen_;
}

std::uint64_t PixmapCache::generation() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return gen_;
}

std::size_t PixmapCache::current_bytes() const
{
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t total = 0;
    for (const auto& [_, e] : entries_)
        total += e.img ? e.img->memory_bytes() : 0;
    return total;
}

std::size_t PixmapCache::size() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

std::size_t PixmapCache::hits() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
}

std::size_t PixmapCache::misses() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return misses_;
}

} // namespace tk
