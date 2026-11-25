#include "views/world/AssetCacheStore.h"

#include <tesseract/paths.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace tesseract::views::world
{

std::filesystem::path default_asset_cache_dir()
{
    return tesseract::cache_dir() / "world-assets";
}

namespace
{
namespace fs = std::filesystem;

constexpr char kMagic[4] = {'T', 'S', 'W', 'A'};
constexpr std::uint32_t kMaxResources = 4096;
constexpr std::uint64_t kMaxBlob = 256ull << 20;
constexpr std::uint64_t kMaxFile = 512ull << 20;
constexpr std::uint64_t kMaxText = 16ull << 20;
constexpr std::uint32_t kMaxPath = 4096;
constexpr std::size_t kDigestSize = 32;

bool valid_asset(const std::string& a)
{
    if (a.empty() || a.size() > 64)
    {
        return false;
    }
    for (char c : a)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
        {
            return false;
        }
    }
    return true;
}

bool valid_key(const std::string& k)
{
    if (k.empty() || k.size() > 128)
    {
        return false;
    }
    for (char c : k)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
        {
            return false;
        }
    }
    return true;
}

fs::path entry_path(const fs::path& dir, const std::string& asset, const std::string& key)
{
    return dir / (asset + "-" + key + ".twa");
}

void put(std::vector<std::uint8_t>& o, const void* p, std::size_t n)
{
    const auto* b = static_cast<const std::uint8_t*>(p);
    o.insert(o.end(), b, b + n);
}
void put32(std::vector<std::uint8_t>& o, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}
void put64(std::vector<std::uint8_t>& o, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
    {
        o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}
void put_text(std::vector<std::uint8_t>& o, const std::string& s)
{
    put64(o, s.size());
    put(o, s.data(), s.size());
}

/// Bounds-checked reader; any overrun or limit violation sets `bad`.
struct Cursor
{
    const std::uint8_t* p;
    std::size_t size;
    std::size_t pos = 0;
    bool bad = false;

    std::size_t remaining() const
    {
        return size - pos;
    }
    bool take(std::size_t n)
    {
        if (bad || n > remaining())
        {
            bad = true;
            return false;
        }
        return true;
    }
    std::uint32_t u32()
    {
        if (!take(4))
        {
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
        {
            v |= static_cast<std::uint32_t>(p[pos + i]) << (8 * i);
        }
        pos += 4;
        return v;
    }
    std::uint64_t u64()
    {
        if (!take(8))
        {
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
        {
            v |= static_cast<std::uint64_t>(p[pos + i]) << (8 * i);
        }
        pos += 8;
        return v;
    }
    // Reads `n` bytes after checking limit and remaining size.
    const std::uint8_t* bytes(std::uint64_t n, std::uint64_t limit)
    {
        if (bad || n > limit || n > remaining())
        {
            bad = true;
            return nullptr;
        }
        const std::uint8_t* r = p + pos;
        pos += static_cast<std::size_t>(n);
        return r;
    }
    std::string text(std::uint64_t limit)
    {
        std::uint64_t n = u64();
        const std::uint8_t* b = bytes(n, limit);
        return b ? std::string(reinterpret_cast<const char*>(b), static_cast<std::size_t>(n))
                 : std::string();
    }
};

std::optional<CachedImport> parse(const std::vector<std::uint8_t>& data)
{
    if (data.size() < sizeof(kMagic) + 4 + 4 + 8 + kDigestSize)
    {
        return std::nullopt;
    }
    const std::size_t body = data.size() - kDigestSize;
    const auto sum = sha256(data.data(), body);
    if (std::memcmp(sum.data(), data.data() + body, kDigestSize) != 0)
    {
        return std::nullopt;
    }

    Cursor c{data.data(), body};
    const std::uint8_t* magic = c.bytes(4, 4);
    if (!magic || std::memcmp(magic, kMagic, 4) != 0)
    {
        return std::nullopt;
    }
    if (c.u32() != kCacheLayoutVersion)
    {
        return std::nullopt;
    }
    const std::uint32_t count = c.u32();
    if (c.bad || count > kMaxResources)
    {
        return std::nullopt;
    }

    CachedImport e;
    e.level_id = c.u64();
    e.manifest = c.text(kMaxText);
    e.warnings = c.text(kMaxText);
    e.summary = c.text(kMaxText);
    // Each resource needs at least 36 bytes, so a bogus count fails here
    // before anything is reserved.
    if (c.bad || static_cast<std::uint64_t>(count) * 36 > c.remaining())
    {
        return std::nullopt;
    }
    e.resources.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        CachedResource r;
        r.id = c.u64();
        r.type = c.u32();
        const std::uint32_t plen = c.u32();
        if (c.bad || plen > kMaxPath)
        {
            return std::nullopt;
        }
        const std::uint8_t* pp = c.bytes(plen, kMaxPath);
        if (pp)
        {
            r.path.assign(reinterpret_cast<const char*>(pp), plen);
        }
        const std::uint8_t* h = c.bytes(16, 16);
        if (h)
        {
            std::memcpy(r.content_hash.data(), h, 16);
        }
        const std::uint64_t blen = c.u64();
        const std::uint8_t* bp = c.bytes(blen, kMaxBlob);
        if (c.bad)
        {
            return std::nullopt;
        }
        if (bp)
        {
            r.blob.assign(bp, bp + blen);
        }
        e.resources.push_back(std::move(r));
    }
    if (c.bad || c.remaining() != 0)
    {
        return std::nullopt;
    }
    bool found = false;
    for (const auto& r : e.resources)
    {
        found = found || r.id == e.level_id;
    }
    if (!found)
    {
        return std::nullopt;
    }
    return e;
}

std::vector<std::uint8_t> serialize(const CachedImport& e)
{
    std::vector<std::uint8_t> o;
    put(o, kMagic, 4);
    put32(o, kCacheLayoutVersion);
    put32(o, static_cast<std::uint32_t>(e.resources.size()));
    put64(o, e.level_id);
    put_text(o, e.manifest);
    put_text(o, e.warnings);
    put_text(o, e.summary);
    for (const auto& r : e.resources)
    {
        put64(o, r.id);
        put32(o, r.type);
        put32(o, static_cast<std::uint32_t>(r.path.size()));
        put(o, r.path.data(), r.path.size());
        put(o, r.content_hash.data(), r.content_hash.size());
        put64(o, r.blob.size());
        put(o, r.blob.data(), r.blob.size());
    }
    const auto sum = sha256(o.data(), o.size());
    put(o, sum.data(), sum.size());
    return o;
}

bool within_limits(const CachedImport& e)
{
    if (e.resources.size() > kMaxResources || e.manifest.size() > kMaxText ||
        e.warnings.size() > kMaxText || e.summary.size() > kMaxText)
    {
        return false;
    }
    for (const auto& r : e.resources)
    {
        if (r.path.size() > kMaxPath || r.blob.size() > kMaxBlob)
        {
            return false;
        }
    }
    return true;
}
} // namespace

std::string make_cache_key(const Sha256Digest& source, std::uint64_t options_fingerprint,
                           std::uint64_t format_version, const std::string& engine_stamp)
{
    const std::string text = "tsa1|" + to_hex(source) + "|" + std::to_string(options_fingerprint) +
                             "|" + std::to_string(format_version) + "|" + engine_stamp;
    return to_hex(sha256(text.data(), text.size())).substr(0, 32);
}

AssetCacheStore::AssetCacheStore(fs::path dir) : dir_(std::move(dir))
{
}

std::optional<CachedImport> AssetCacheStore::load(const std::string& asset,
                                                  const std::string& key) const
{
    try
    {
        if (!valid_asset(asset) || !valid_key(key))
        {
            return std::nullopt;
        }
        const fs::path p = entry_path(dir_, asset, key);
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        if (ec || size > kMaxFile)
        {
            return std::nullopt;
        }
        std::ifstream f(p, std::ios::binary);
        if (!f)
        {
            return std::nullopt;
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f || static_cast<std::uint64_t>(f.gcount()) != size)
        {
            return std::nullopt;
        }
        return parse(data);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool AssetCacheStore::store(const std::string& asset, const std::string& key,
                            const CachedImport& entry) const
{
    if (!valid_asset(asset) || !valid_key(key))
    {
        return false;
    }
    fs::path tmp;
    try
    {
        if (!within_limits(entry))
        {
            return false;
        }
        std::error_code ec;
        fs::create_directories(dir_, ec);
        if (ec)
        {
            return false;
        }

        static std::atomic<std::uint64_t> counter{0};
#ifdef _WIN32
        const auto pid = static_cast<unsigned long>(_getpid());
#else
        const auto pid = static_cast<unsigned long>(::getpid());
#endif
        const fs::path final_path = entry_path(dir_, asset, key);
        tmp = dir_ / (final_path.filename().string() + "." + std::to_string(pid) + "." +
                      std::to_string(counter.fetch_add(1)) + ".tmp");

        const auto bytes = serialize(entry);
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                return false;
            }
            f.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
            f.flush();
            if (!f)
            {
                f.close();
                fs::remove(tmp, ec);
                return false;
            }
        }
        fs::rename(tmp, final_path, ec);
        if (ec)
        {
            fs::remove(tmp, ec);
            return false;
        }

        // Cleanup is best-effort: other keys of this asset, and old temp files.
        const std::string prefix = asset + "-";
        const std::string keep = final_path.filename().string();
        const auto now = fs::file_time_type::clock::now();
        for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            std::error_code e2;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".twa") == 0 && name != keep &&
                name.compare(0, prefix.size(), prefix) == 0)
            {
                fs::remove(it->path(), e2);
            }
            else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
            {
                const auto mt = fs::last_write_time(it->path(), e2);
                if (!e2 && now - mt > std::chrono::hours(1))
                {
                    fs::remove(it->path(), e2);
                }
            }
        }
        return true;
    }
    catch (...)
    {
        std::error_code ec;
        if (!tmp.empty())
        {
            fs::remove(tmp, ec);
        }
        return false;
    }
}

void AssetCacheStore::remove(const std::string& asset, const std::string& key) const
{
    try
    {
        if (!valid_asset(asset) || !valid_key(key))
        {
            return;
        }
        std::error_code ec;
        fs::remove(entry_path(dir_, asset, key), ec);
    }
    catch (...)
    {
    }
}

} // namespace tesseract::views::world
