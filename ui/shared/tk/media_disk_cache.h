#pragma once
#include "tk/cache_key.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

namespace tk
{

// Persistent disk cache for raw media bytes (avatars, inline images, stickers).
// Files are stored flat under dir_, named by FNV-1a hash of the cache key.
// Writes are atomic (temp file → rename). Thread-safe for concurrent loads
// and stores of distinct keys; ShellBase's in-flight set prevents concurrent
// writes to the same key.
class MediaDiskCache
{
public:
    static constexpr std::uintmax_t kDefaultMaxBytes = 256ULL * 1024 * 1024;

    // Creates dir_ (and any missing parents) on construction. max_bytes is the
    // size the cache keeps itself under once its total is known (see store()).
    explicit MediaDiskCache(std::filesystem::path dir, std::uintmax_t max_bytes = kDefaultMaxBytes);

    // Returns cached bytes, or an empty vector on miss.
    std::vector<uint8_t> load(const CacheKey& key) const;

    // Writes bytes atomically. No-op if bytes is empty. Keeps a running total
    // of the cache size; when a write takes it past max_bytes, prunes the oldest
    // entries down to 90% of it, so the scan happens once per ~10% of growth.
    // The first store (or prune) scans the directory to learn the starting size.
    void store(const CacheKey& key, const std::vector<uint8_t>& bytes) const;

    // Removes the cached entry for key. No-op on miss. Thread-safe.
    void evict(const CacheKey& key) const;

    // Deletes oldest entries (by mtime) until total size ≤ max_bytes.
    // Also (re)learns the running total, so the owner calls it once at startup
    // from a background thread as a safety net; store() keeps the cap afterwards.
    void prune(std::uintmax_t max_bytes = kDefaultMaxBytes) const;

    // Sum of all cached file sizes, in bytes. Thread-safe.
    std::uintmax_t size_bytes() const;

    // Delete all cached files and recreate the (empty) cache directory.
    // Resets hit/miss counters. Blocks on the filesystem and races concurrent
    // load/store calls; prefer retire_for_clear() + finish_clear() when the
    // caller is on the UI thread.
    void clear() const;

    // First half of a clear, cheap enough for the UI thread: renames the cache
    // directory aside and creates an empty one in its place, and resets the
    // hit/miss counters. Returns the aside path, or an empty path if the rename
    // failed (for example a file held open on Windows); the cache is then left as
    // it was.
    std::filesystem::path retire_for_clear() const;

    // Second half, safe on any thread: deletes the directory retire_for_clear()
    // moved aside. With an empty path (the rename failed), removes the live
    // entries one at a time, as evict() does.
    void finish_clear(const std::filesystem::path& retired) const;

    uint64_t hits() const
    {
        return hits_.load(std::memory_order_relaxed);
    }
    uint64_t misses() const
    {
        return misses_.load(std::memory_order_relaxed);
    }

private:
    std::filesystem::path path_for(const CacheKey& key) const;
    // Scans dir_, removes oldest entries while the total exceeds `trigger` until
    // it is <= `target`, and resyncs the running total. Serialised by prune_mu_;
    // with wait=false a scan already running makes this a no-op.
    void prune_scan_(std::uintmax_t trigger, std::uintmax_t target, bool wait) const;
    std::filesystem::path dir_;
    std::uintmax_t max_bytes_;
    // Running total of entry bytes; meaningful only once tracked_valid_ is set
    // (by the first scan). Updated on store/evict/clear, resynced by each scan,
    // so concurrent writers during a scan can leave it slightly off until the
    // next one.
    mutable std::atomic<std::uintmax_t> tracked_bytes_{0};
    mutable std::atomic<bool> tracked_valid_{false};
    mutable std::mutex prune_mu_;
    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
};

} // namespace tk
