#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
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
    // Creates dir_ (and any missing parents) on construction.
    explicit MediaDiskCache(std::filesystem::path dir);

    // Returns cached bytes, or an empty vector on miss.
    std::vector<uint8_t> load(const std::string& key) const;

    // Writes bytes atomically. No-op if bytes is empty.
    void store(const std::string& key, const std::vector<uint8_t>& bytes) const;

    // Removes the cached entry for key. No-op on miss. Thread-safe.
    void evict(const std::string& key) const;

    // Deletes oldest entries (by mtime) until total size ≤ max_bytes.
    // Intended to be called once per session from a background thread.
    void prune(std::uintmax_t max_bytes = 256ULL * 1024 * 1024) const;

    // Sum of all cached file sizes, in bytes. Thread-safe.
    std::uintmax_t size_bytes() const;

    // Delete all cached files and recreate the (empty) cache directory.
    // Resets hit/miss counters.
    void clear() const;

    uint64_t hits() const
    {
        return hits_.load(std::memory_order_relaxed);
    }
    uint64_t misses() const
    {
        return misses_.load(std::memory_order_relaxed);
    }

private:
    std::filesystem::path path_for(const std::string& key) const;
    std::filesystem::path dir_;
    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
};

} // namespace tk
