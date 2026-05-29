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
    bool store(const std::string& key, std::unique_ptr<Image> img);

    // Return the image for key, refreshing its last-use timestamp, or nullptr
    // on a miss or if the entry has exceeded the TTL.
    const Image* get(const std::string& key);

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
        std::unique_ptr<Image>                img;
        std::size_t                           bytes;
        std::chrono::steady_clock::time_point last_use;
    };

    std::unordered_map<std::string, Entry> entries_;
    std::size_t                            max_bytes_;
    std::size_t                            current_bytes_ = 0;
    std::chrono::seconds                   ttl_;

    // Sweep expired entries, then LRU-evict until current_bytes_ + needed
    // fits within max_bytes_. Returns false if needed > max_bytes_.
    bool make_room_for_(std::size_t needed);
};

} // namespace tk
