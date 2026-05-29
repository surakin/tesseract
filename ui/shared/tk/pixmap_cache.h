#pragma once

#include "canvas.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

namespace tk
{

// Bounded, TTL-limited in-memory cache for decoded tk::Image objects.
//
// Sits in front of MediaDiskCache in the fetch chain: ensure_media_image_()
// checks here first and skips disk + SDK access when the decoded pixels are
// already resident. This makes room-switch return fast when the user comes
// back within the TTL window.
//
// Memory budget: entries are evicted LRU-first once current_bytes() would
// exceed max_bytes(). TTL eviction runs on every store() (cheap sweep of
// expired entries before trying to make room) and on explicit evict_expired()
// calls (e.g. on room switch).
//
// Checkout model: message rows call take() to check images out of the
// evictable pool into shared row ownership. While checked out the image is
// absent from entries_ (not counted toward the memory budget) but tracked in
// checked_out_ via weak_ptr so subsequent take() calls for the same live key
// rejoin the existing shared owner. store() returns an image to the evictable
// pool and clears its checked_out_ entry.
//
// Thread-safety: all methods must be called on the UI thread.
class PixmapCache
{
public:
    // max_bytes: total decoded-pixel budget (default 64 MiB).
    // ttl:       seconds before an unaccessed entry is eligible for eviction.
    explicit PixmapCache(
        std::size_t      max_bytes = 64u * 1024u * 1024u,
        std::chrono::seconds ttl   = std::chrono::seconds{30});

    // Store a decoded image. The memory cost is taken from img->memory_bytes().
    // Evicts expired entries first, then LRU entries until budget allows it.
    // Returns false (without storing) if the single entry exceeds max_bytes_.
    // Clears any checked_out_ tracking for this key.
    bool store(const std::string& key, std::shared_ptr<Image> img);

    // Return the image for key, refreshing its last-use timestamp, or nullptr
    // on a miss or if the entry has exceeded the TTL.
    const Image* get(const std::string& key);

    // True if the key is in the cache OR currently checked out into ≥1 row.
    // Use this in ensure_media_image_() guards instead of get() != nullptr so
    // checked-out images don't trigger redundant re-fetches.
    bool has(const std::string& key) const noexcept;

    // Remove the key from the evictable pool and return a shared reference.
    // The first call for a live key removes it from entries_ and records a
    // weak_ptr in checked_out_. Subsequent calls while any row still holds the
    // image lock the weak_ptr and return the same shared object (no second
    // eviction). Returns nullptr on miss or if the entry has expired.
    std::shared_ptr<Image> take(const std::string& key);

    // Remove a single entry (e.g. when a fresher decode arrives).
    void evict(const std::string& key);

    // Remove all entries whose last-use time is older than ttl_.
    // Called on room switch and from any periodic coarse timer.
    void evict_expired();

    std::size_t current_bytes() const noexcept
    {
        return current_bytes_;
    }
    std::size_t max_bytes() const noexcept
    {
        return max_bytes_;
    }

private:
    struct Entry
    {
        std::shared_ptr<Image>                img;
        std::size_t                           bytes;
        std::chrono::steady_clock::time_point last_use;
    };

    std::unordered_map<std::string, Entry>                    entries_;
    std::unordered_map<std::string, std::weak_ptr<Image>>     checked_out_;
    std::size_t                                               max_bytes_;
    std::size_t                                               current_bytes_ = 0;
    std::chrono::seconds                                      ttl_;

    // Sweep expired entries, then LRU-evict until current_bytes_ + needed
    // fits within max_bytes_. Returns false if needed > max_bytes_.
    bool make_room_for_(std::size_t needed);
};

} // namespace tk
