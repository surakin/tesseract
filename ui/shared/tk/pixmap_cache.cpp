#include "pixmap_cache.h"

#include <algorithm>
#include <vector>

namespace tk
{

PixmapCache::PixmapCache(std::size_t max_bytes, std::chrono::seconds ttl)
    : max_bytes_(max_bytes), ttl_(ttl)
{
}

bool PixmapCache::store(const std::string& key, std::shared_ptr<Image> img)
{
    if (!img)
        return false;

    // Clear any checkout tracking for this key (image is being returned
    // to the evictable pool by a row or freshly decoded by a shell).
    checked_out_.erase(key);

    const std::size_t needed = img->memory_bytes();

    // Remove any existing entry for this key first so we don't double-count.
    if (auto it = entries_.find(key); it != entries_.end())
    {
        current_bytes_ -= it->second.bytes;
        entries_.erase(it);
    }

    if (!make_room_for_(needed))
        return false;

    current_bytes_ += needed;
    entries_.emplace(key,
                     Entry{std::move(img), needed,
                           std::chrono::steady_clock::now()});
    return true;
}

const Image* PixmapCache::get(const std::string& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end())
        return nullptr;

    // Lazy TTL check: evict and return nullptr when the entry has expired.
    const auto age = std::chrono::steady_clock::now() - it->second.last_use;
    if (age > ttl_)
    {
        current_bytes_ -= it->second.bytes;
        entries_.erase(it);
        return nullptr;
    }

    it->second.last_use = std::chrono::steady_clock::now();
    return it->second.img.get();
}

bool PixmapCache::has(const std::string& key) const noexcept
{
    if (entries_.count(key))
        return true;
    auto it = checked_out_.find(key);
    return it != checked_out_.end() && !it->second.expired();
}

std::shared_ptr<Image> PixmapCache::take(const std::string& key)
{
    // Already checked out into ≥1 row? Rejoin the same shared object.
    if (auto it = checked_out_.find(key); it != checked_out_.end())
    {
        if (auto sp = it->second.lock())
            return sp;
        checked_out_.erase(it); // all rows dropped it — fall through to entries_
    }

    auto it = entries_.find(key);
    if (it == entries_.end())
        return nullptr;

    // Honour TTL: don't hand out a stale image.
    const auto age = std::chrono::steady_clock::now() - it->second.last_use;
    if (age > ttl_)
    {
        current_bytes_ -= it->second.bytes;
        entries_.erase(it);
        return nullptr;
    }

    current_bytes_ -= it->second.bytes;
    auto sp = std::move(it->second.img);
    entries_.erase(it);
    checked_out_.emplace(key, std::weak_ptr<Image>(sp));
    return sp;
}

void PixmapCache::evict(const std::string& key)
{
    checked_out_.erase(key);
    if (auto it = entries_.find(key); it != entries_.end())
    {
        current_bytes_ -= it->second.bytes;
        entries_.erase(it);
    }
}

void PixmapCache::evict_expired()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (now - it->second.last_use > ttl_)
        {
            current_bytes_ -= it->second.bytes;
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Also prune dead weak_ptrs from checked_out_.
    for (auto it = checked_out_.begin(); it != checked_out_.end();)
    {
        if (it->second.expired())
            it = checked_out_.erase(it);
        else
            ++it;
    }
}

bool PixmapCache::make_room_for_(std::size_t needed)
{
    if (needed > max_bytes_)
        return false;

    // First pass: free expired entries (no need to touch still-valid cache).
    evict_expired();

    if (current_bytes_ + needed <= max_bytes_)
        return true;

    // Second pass: collect entries sorted oldest-first and evict until budget fits.
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> order;
    order.reserve(entries_.size());
    for (const auto& [k, e] : entries_)
        order.emplace_back(k, e.last_use);

    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    for (const auto& [k, _] : order)
    {
        if (current_bytes_ + needed <= max_bytes_)
            break;
        if (auto it = entries_.find(k); it != entries_.end())
        {
            current_bytes_ -= it->second.bytes;
            entries_.erase(it);
        }
    }

    return current_bytes_ + needed <= max_bytes_;
}

} // namespace tk
