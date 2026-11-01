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

MediaDiskCache::MediaDiskCache(fs::path dir) : dir_(std::move(dir))
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
        if (name.compare(0, prefix.size(), prefix) == 0)
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
    if (size <= 0 || static_cast<std::size_t>(size) <= header)
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
    fs::rename(tmp, dest, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
    }
}

void MediaDiskCache::evict(const CacheKey& key) const
{
    std::error_code ec;
    fs::remove(path_for(key), ec);
}

void MediaDiskCache::prune(std::uintmax_t max_bytes) const
{
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

    if (total <= max_bytes)
    {
        return;
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b)
              {
                  return a.mtime < b.mtime;
              });

    for (const auto& e : entries)
    {
        if (total <= max_bytes)
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
}

} // namespace tk
