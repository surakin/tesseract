#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tk
{

// In-RAM cache of *compressed* (still-encoded JPEG/PNG/GIF/WebP) media bytes,
// keyed by the same disk-cache key the decoded caches derive from. It sits as
// an L1 tier in front of MediaDiskCache (L2): a hit lets a decoded-cache miss
// be resolved by a local re-decode instead of a filesystem read + the SDK
// round-trip. Entries are ~10x smaller than their decoded form, so a modest
// budget covers far more of the working set than PixmapCache can.
//
// Unlike PixmapCache this is touched from worker threads (the media io pool
// both reads it before the disk read and writes it next to MediaDiskCache),
// so every method is internally synchronised. get() hands out a shared_ptr to
// an immutable buffer, so the caller decodes without holding the lock.
class CompressedImageCache
{
public:
    using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;

    explicit CompressedImageCache(std::size_t max_bytes = 32u * 1024u * 1024u,
                                  std::size_t max_entry_bytes = 4u * 1024u * 1024u);

    // Lookup. Returns the buffer (and moves `key` to most-recently-used) or
    // nullptr on a miss.
    Bytes get(const std::string& key);

    // Insert or replace. Ignored when `bytes` is empty or larger than
    // max_entry_bytes (one huge asset must not evict the whole tier).
    void put(const std::string& key, std::vector<std::uint8_t> bytes);

    void evict(const std::string& key);
    void clear();

    std::size_t current_bytes() const;
    std::size_t size() const;
    std::size_t max_bytes() const
    {
        return max_bytes_;
    }
    std::size_t hits() const
    {
        return hits_.load(std::memory_order_relaxed);
    }
    std::size_t misses() const
    {
        return misses_.load(std::memory_order_relaxed);
    }

private:
    // Caller must hold mu_.
    void trim_to_budget_();

    struct Entry
    {
        Bytes data;
        std::size_t bytes = 0;
        std::list<std::string>::iterator lru; // position in lru_ (front = MRU)
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> map_;
    std::list<std::string> lru_;
    std::size_t max_bytes_;
    std::size_t max_entry_bytes_;
    std::size_t current_bytes_ = 0;
    std::atomic<std::size_t> hits_{0};
    std::atomic<std::size_t> misses_{0};
};

} // namespace tk
