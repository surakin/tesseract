#pragma once

#include "canvas.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace tk
{

// Bounded, TTL'd in-memory cache of decoded images, keyed by a stable media
// identifier (mxc URL, tile key, ...). The cache always holds a shared_ptr to
// every entry and never moves it out: a widget "pins" an image by holding a
// copy of the ImageRef returned by store()/acquire(). "Displayed somewhere" is
// therefore exactly use_count() > 1, and eviction (TTL expiry or over-budget)
// only ever reclaims entries the cache alone references (use_count() == 1).
// Because the cache keeps its own reference, a transient peek()/acquire()
// always returns a live image when present, so a not-yet-pinned widget can
// never render blank.
//
// UI-thread-only: no internal locking. Every store/acquire/peek/sweep/clear
// call must run on the UI thread (decodes are marshalled there before store()).
class PixmapCache
{
public:
    explicit PixmapCache(std::size_t max_bytes = 64u * 1024u * 1024u,
                         std::chrono::seconds ttl = std::chrono::seconds{30});

    // Insert (or replace) the image for `key`. Wraps it in an ImageRef the
    // cache retains, marks it freshly used, and returns the handle so the
    // caller can pin it without a second lookup. Always stores, even if a
    // single image exceeds max_bytes (refusing would risk a blank render).
    ImageRef store(const std::string& key, std::unique_ptr<Image> img);

    // Pinning lookup: returns a handle that keeps `key` un-evictable while held
    // (or nullptr if absent). Resets the entry's TTL.
    ImageRef acquire(const std::string& key);

    // Non-pinning lookup for transient paint/measure use (or nullptr). Resets
    // the entry's TTL. The pointer is valid until the next sweep()/clear().
    const Image* peek(const std::string& key);

    bool contains(const std::string& key) const;

    // Drop `key`, but only if the cache holds the sole reference to it.
    void evict(const std::string& key);

    // Drop the cache's own references to all entries. Images still pinned by a
    // widget stay alive until that widget releases its handle.
    void clear();

    // Reclaim memory: first drop expired, unreferenced entries; then, while
    // over budget, evict least-recently-used unreferenced entries oldest-first.
    // Never touches a pinned entry (use_count() > 1), so the budget is a soft
    // cap whenever currently-displayed content alone exceeds it.
    void sweep();

    std::size_t current_bytes() const
    {
        return current_bytes_;
    }
    std::size_t max_bytes() const
    {
        return max_bytes_;
    }
    std::size_t size() const
    {
        return entries_.size();
    }

    std::size_t hits() const
    {
        return hits_;
    }
    std::size_t misses() const
    {
        return misses_;
    }

    // Test seam: override the monotonic clock used for TTL bookkeeping.
    void set_clock_for_testing(
        std::function<std::chrono::steady_clock::time_point()> clock)
    {
        clock_ = std::move(clock);
    }

private:
    struct Entry
    {
        ImageRef img;
        std::size_t bytes = 0;
        std::chrono::steady_clock::time_point last_use;
    };

    std::chrono::steady_clock::time_point now_() const;

    std::unordered_map<std::string, Entry> entries_;
    std::size_t max_bytes_;
    std::size_t current_bytes_ = 0;
    std::size_t hits_          = 0;
    std::size_t misses_        = 0;
    std::chrono::seconds ttl_;
    std::function<std::chrono::steady_clock::time_point()> clock_;
};

} // namespace tk
