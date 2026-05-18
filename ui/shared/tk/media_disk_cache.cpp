#include "tk/media_disk_cache.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
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

MediaDiskCache::MediaDiskCache(fs::path dir) : dir_(std::move(dir))
{
    std::error_code ec;
    fs::create_directories(dir_, ec);
}

fs::path MediaDiskCache::path_for(const std::string& key) const
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(key)));
    return dir_ / buf;
}

std::vector<uint8_t> MediaDiskCache::load(const std::string& key) const
{
    const fs::path p = path_for(key);
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f.is_open())
    {
        return {};
    }
    const auto size = static_cast<std::streamsize>(f.tellg());
    if (size <= 0)
    {
        return {};
    }
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<std::size_t>(size));
    if (!f.read(reinterpret_cast<char*>(buf.data()), size))
    {
        return {};
    }
    return buf;
}

void MediaDiskCache::store(const std::string& key,
                           const std::vector<uint8_t>& bytes) const
{
    if (bytes.empty())
    {
        return;
    }
    const fs::path dest = path_for(key);
    const fs::path tmp = fs::path(dest).replace_extension(".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
        {
            return;
        }
        if (!f.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size())))
        {
            std::error_code ec;
            fs::remove(tmp, ec);
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
    }
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

} // namespace tk
