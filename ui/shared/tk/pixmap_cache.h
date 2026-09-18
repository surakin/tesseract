#pragma once

#include "canvas.h"
#include "tk/cache_key.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
// Internally synchronised (mirrors tk::CompressedImageCache): every public
// method locks a private mutex, so store()/acquire()/peek()/evict()/etc. may
// be called from any thread without external coordination. The one caveat
// this buys less of than CompressedImageCache's shared_ptr<const Bytes>
// hand-out: peek() (like AnimImageCache::current_frame()) returns a raw,
// non-owning `const Image*` that stays valid only until the next
// sweep()/clear()/store() call on this cache — the lock protects the cache's
// own bookkeeping during the call itself, not the lifetime of a pointer
// already handed back to the caller. In this codebase every peek()/store()
// caller still runs on the UI thread (paint-path reads, and prefetch/
// media-download writes that hop back to the UI thread before storing), so
// that caveat is not currently exercised across threads — this lock is
// defense in depth, not a fix for a live race.
class PixmapCache
{
public:
    explicit PixmapCache(std::size_t max_bytes = 64u * 1024u * 1024u,
                         std::chrono::seconds ttl = std::chrono::seconds{30});
    ~PixmapCache();
    PixmapCache(const PixmapCache&)            = delete;
    PixmapCache& operator=(const PixmapCache&) = delete;

    // Insert (or replace) the image for `key`. Wraps it in an ImageRef the
    // cache retains, marks it freshly used, and returns the handle so the
    // caller can pin it without a second lookup. Always stores, even if a
    // single image exceeds max_bytes (refusing would risk a blank render).
    ImageRef store(const CacheKey& key, std::unique_ptr<Image> img);

    // Pinning lookup: returns a handle that keeps `key` un-evictable while held
    // (or nullptr if absent). Resets the entry's TTL.
    ImageRef acquire(const CacheKey& key);

    // Non-pinning lookup for transient paint/measure use (or nullptr). Resets
    // the entry's TTL. The pointer is valid until the next sweep()/clear().
    const Image* peek(const CacheKey& key);

    bool contains(const CacheKey& key) const;

    // Drop `key`, but only if the cache holds the sole reference to it.
    void evict(const CacheKey& key);

    // Drop the cache's own references to all entries. Images still pinned by a
    // widget stay alive until that widget releases its handle.
    void clear();

    // Reclaim memory: first drop expired, unreferenced entries; then, while
    // over budget, evict least-recently-used unreferenced entries oldest-first.
    // Never touches a pinned entry (use_count() > 1). Legacy path — the image
    // GC (retain_recent, below) is the live eviction policy; kept for tests.
    void sweep();

    // ── Mark-and-sweep GC (the on-screen retention mechanism) ────────────────
    // peek()/acquire()/store() stamp the touched entry with the current
    // generation and refresh its wall-clock timestamp. ShellBase::run_image_gc_
    // bumps the generation once per cycle, forces a full unclipped repaint of
    // every surface (so every on-screen image is peek()'d at the new
    // generation), then calls retain_recent(): an entry is evicted only when it
    // is held solely by the cache (use_count() == 1), has not been peeked within
    // the last `keep` generations, AND has not been touched within the TTL (a
    // wall-clock floor that protects a just-fetched image whose widget has not
    // painted yet). An idle window freezes the generation, so its on-screen
    // images are never reclaimed.
    void advance_generation();
    void retain_recent(unsigned keep);
    std::uint64_t generation() const;

    // Live resident size: sums Image::memory_bytes() across entries on every
    // call, because backends memoise extra pre-scaled / GPU copies as an image
    // is painted at different sizes, so a figure captured at store() drifts.
    std::size_t current_bytes() const;

    // Sum of current_bytes() over every live PixmapCache in the process
    // (avatar/thumbnail caches and the small per-host/per-view pill caches).
    static std::size_t total_bytes_all_instances();
    std::size_t max_bytes() const
    {
        return max_bytes_; // immutable after construction — no lock needed
    }
    std::size_t size() const;

    std::size_t hits() const;
    std::size_t misses() const;

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
        std::uint64_t last_peek_gen = 0;
    };

    std::chrono::steady_clock::time_point now_() const;

    // Re-read every entry's memory_bytes() and resync current_bytes_.
    // Caller holds mu_.
    void refresh_bytes_locked_();

    mutable std::mutex mu_;
    std::unordered_map<CacheKey, Entry, CacheKeyHash> entries_;
    std::size_t max_bytes_;
    std::size_t current_bytes_ = 0;
    std::size_t hits_          = 0;
    std::size_t misses_        = 0;
    std::uint64_t gen_         = 0;
    std::chrono::seconds ttl_;
    std::function<std::chrono::steady_clock::time_point()> clock_;
};

} // namespace tk
