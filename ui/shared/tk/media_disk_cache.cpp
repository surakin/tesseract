#include "tk/media_disk_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tk
{

namespace fs = std::filesystem;

static uint64_t fnv1a64(std::string_view s)
{
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s)
    {
        h = (h ^ c) * 1099511628211ULL;
    }
    return h;
}

// Each entry file begins with this header and the full cache key, so a hit
// is only ever the bytes stored under that exact key: the file name is a
// 64-bit hash, and two keys can share one.
static constexpr char kEntryMagic[4] = {'T', 'S', 'C', 'K'};

MediaDiskCache::MediaDiskCache(fs::path dir, std::uintmax_t max_bytes)
    : dir_(std::move(dir)), max_bytes_(max_bytes)
{
    std::error_code ec;
    // Decrypted media lives here: owner-only, whatever the process umask is.
    fs::create_directories(dir_, ec);
    fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::replace, ec);

    // A clear that was cut short (the app quit before finish_clear) leaves its
    // retired directory of decrypted media beside the cache; remove those now.
    const std::string prefix = dir_.filename().string() + ".clearing-";
    const fs::path parent = dir_.parent_path();
    for (const auto& de : fs::directory_iterator(parent.empty() ? fs::path(".") : parent, ec))
    {
        const std::string name = de.path().filename().string();
        if (name.starts_with(prefix))
        {
            std::error_code rm_ec;
            fs::remove_all(de.path(), rm_ec);
        }
    }
}

fs::path MediaDiskCache::path_for(const CacheKey& key) const
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(key.to_string())));
    return dir_ / buf;
}

std::vector<uint8_t> MediaDiskCache::load(const CacheKey& key) const
{
    const fs::path p = path_for(key);
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f.is_open())
    {
        ++misses_;
        return {};
    }
    const auto size = static_cast<std::streamsize>(f.tellg());
    const std::string id = key.to_string();
    const std::size_t header = sizeof(kEntryMagic) + sizeof(std::uint32_t) + id.size();
    if (size <= 0 || std::cmp_less_equal(size, header))
    {
        ++misses_;
        return {};
    }
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<std::size_t>(size));
    if (!f.read(reinterpret_cast<char*>(buf.data()), size))
    {
        ++misses_;
        return {};
    }
    // Only an entry stored under this exact key is a hit: magic, then the key's
    // length and bytes, then the payload.
    std::uint32_t id_len = 0;
    std::memcpy(&id_len, buf.data() + sizeof(kEntryMagic), sizeof(id_len));
    if (std::memcmp(buf.data(), kEntryMagic, sizeof(kEntryMagic)) != 0 || id_len != id.size() ||
        std::memcmp(buf.data() + sizeof(kEntryMagic) + sizeof(id_len), id.data(), id.size()) != 0)
    {
        ++misses_;
        return {};
    }
    ++hits_;
    return std::vector<uint8_t>(buf.begin() + static_cast<std::ptrdiff_t>(header), buf.end());
}

void MediaDiskCache::store(const CacheKey& key, const std::vector<uint8_t>& bytes) const
{
    if (bytes.empty())
    {
        return;
    }
    const fs::path dest = path_for(key);
    const fs::path tmp = fs::path(dest).replace_extension(".tmp");
    const std::string id = key.to_string();
    const auto id_len = static_cast<std::uint32_t>(id.size());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
        {
            return;
        }
        if (!f.write(kEntryMagic, sizeof(kEntryMagic)) ||
            !f.write(reinterpret_cast<const char*>(&id_len), sizeof(id_len)) ||
            !f.write(id.data(), static_cast<std::streamsize>(id.size())) ||
            !f.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size())))
        {
            f.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            return;
        }
    }
    std::error_code ec;
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                    ec);
    ec.clear();
    const std::uintmax_t new_size = fs::file_size(tmp, ec);
    const bool sized = !ec;
    ec.clear();
    const std::uintmax_t old_size = fs::file_size(dest, ec); // 0 when replacing nothing
    const bool replaced = !ec;
    ec.clear();
    fs::rename(tmp, dest, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        return;
    }
    if (!tracked_valid_.load(std::memory_order_acquire))
    {
        // Total unknown yet: learn it (and trim if already over) with one scan.
        prune_scan_(max_bytes_, max_bytes_ - max_bytes_ / 10, false);
        return;
    }
    if (sized)
    {
        tracked_bytes_.fetch_add(new_size, std::memory_order_relaxed);
    }
    if (replaced)
    {
        // Overwrote an existing entry: its bytes no longer count. Clamp at 0.
        std::uintmax_t cur = tracked_bytes_.load(std::memory_order_relaxed);
        while (!tracked_bytes_.compare_exchange_weak(cur, cur > old_size ? cur - old_size : 0,
                                                     std::memory_order_relaxed))
        {
        }
    }
    if (tracked_bytes_.load(std::memory_order_relaxed) > max_bytes_)
    {
        prune_scan_(max_bytes_, max_bytes_ - max_bytes_ / 10, false);
    }
}

void MediaDiskCache::evict(const CacheKey& key) const
{
    std::error_code ec;
    const fs::path p = path_for(key);
    const std::uintmax_t size = fs::file_size(p, ec);
    const bool sized = !ec;
    ec.clear();
    if (fs::remove(p, ec) && sized && tracked_valid_.load(std::memory_order_acquire))
    {
        std::uintmax_t cur = tracked_bytes_.load(std::memory_order_relaxed);
        while (!tracked_bytes_.compare_exchange_weak(cur, cur > size ? cur - size : 0,
                                                     std::memory_order_relaxed))
        {
        }
    }
}

void MediaDiskCache::prune(std::uintmax_t max_bytes) const
{
    prune_scan_(max_bytes, max_bytes, true);
}

void MediaDiskCache::prune_scan_(std::uintmax_t trigger, std::uintmax_t target, bool wait) const
{
    // One scan at a time. A writer that finds one running skips: that scan
    // resyncs the total, and the next store re-checks.
    std::unique_lock<std::mutex> lock(prune_mu_, std::try_to_lock);
    if (!lock.owns_lock())
    {
        if (!wait)
        {
            return; // automatic prune from store(): the running one covers it
        }
        lock.lock(); // explicit prune(): wait for its turn
    }
    struct Entry
    {
        fs::file_time_type mtime;
        std::uintmax_t size;
        fs::path path;
    };
    std::vector<Entry> entries;
    std::uintmax_t total = 0;

    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir_, ec))
    {
        if (!de.is_regular_file(ec))
        {
            continue;
        }
        const auto sz = de.file_size(ec);
        if (ec)
        {
            ec.clear();
            continue;
        }
        entries.push_back({de.last_write_time(ec), sz, de.path()});
        if (ec)
        {
            ec.clear();
            entries.back().mtime = {};
        }
        total += sz;
    }

    if (total <= trigger)
    {
        tracked_bytes_.store(total, std::memory_order_relaxed);
        tracked_valid_.store(true, std::memory_order_release);
        return;
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b)
              {
                  return a.mtime < b.mtime;
              });

    for (const auto& e : entries)
    {
        if (total <= target)
        {
            break;
        }
        fs::remove(e.path, ec);
        if (!ec)
        {
            total -= e.size;
        }
        ec.clear();
    }
    tracked_bytes_.store(total, std::memory_order_relaxed);
    tracked_valid_.store(true, std::memory_order_release);
}

std::uintmax_t MediaDiskCache::size_bytes() const
{
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir_, ec))
    {
        if (de.is_regular_file(ec))
        {
            total += de.file_size(ec);
        }
    }
    return total;
}

void MediaDiskCache::clear() const
{
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_, ec);
    fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::replace, ec);
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    tracked_bytes_.store(0, std::memory_order_relaxed);
    tracked_valid_.store(true, std::memory_order_release);
}

fs::path MediaDiskCache::retire_for_clear() const
{
    // The aside name only has to be unused; a counter and the clock make it so
    // even across runs that left a directory behind.
    static std::atomic<std::uint64_t> seq{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path retired(dir_.string() + ".clearing-" + std::to_string(seq.fetch_add(1)) + "-" +
                           std::to_string(stamp));
    std::error_code ec;
    fs::rename(dir_, retired, ec);
    if (ec)
    {
        return {};
    }
    // Writers that race this window either land in the aside copy, which is
    // deleted anyway, or miss the cache for the moment. Both are harmless.
    fs::create_directories(dir_, ec);
    fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::replace, ec);
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    tracked_bytes_.store(0, std::memory_order_relaxed);
    tracked_valid_.store(true, std::memory_order_release);
    return retired;
}

void MediaDiskCache::finish_clear(const fs::path& retired) const
{
    std::error_code ec;
    if (!retired.empty())
    {
        fs::remove_all(retired, ec);
        return;
    }
    for (const auto& de : fs::directory_iterator(dir_, ec))
    {
        if (de.is_regular_file(ec))
        {
            fs::remove(de.path(), ec);
        }
    }
    tracked_valid_.store(false, std::memory_order_release); // re-learn on next store
}

} // namespace tk
