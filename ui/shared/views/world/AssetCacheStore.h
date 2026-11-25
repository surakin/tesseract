#pragma once

#include <tesseract/hash.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tesseract::views::world
{

inline constexpr std::uint32_t kCacheLayoutVersion = 1;

/// The directory the minted world assets are kept in (under the cache directory).
std::filesystem::path default_asset_cache_dir();

/// How one asset was loaded (see WorldAssetLoader).
struct AssetLoadReport
{
    std::string name;
    enum class Source
    {
        Imported,
        Cache
    } source = Source::Imported;
    bool stored = false;
    double milliseconds = 0;
    std::string note;
};

/// One imported engine resource, as stored in the cache.
struct CachedResource
{
    std::uint64_t id = 0;
    std::uint32_t type = 0;
    std::string path;
    std::array<std::uint8_t, 16> content_hash{};
    std::vector<std::uint8_t> blob;
};

/// A whole import result; `resources` are in import order, dependencies first.
struct CachedImport
{
    std::vector<CachedResource> resources;
    std::uint64_t level_id = 0;
    std::string manifest, warnings, summary;
};

/// 32 lower-case hex characters keyed on the source bytes, import options,
/// format version and engine stamp.
std::string make_cache_key(const Sha256Digest& source, std::uint64_t options_fingerprint,
                           std::uint64_t format_version, const std::string& engine_stamp);

/// On-disk cache of imported worlds: `<dir>/<asset>-<key>.twa`. File contents
/// are untrusted; every failure yields nullopt/false, never an exception.
class AssetCacheStore
{
public:
    explicit AssetCacheStore(std::filesystem::path dir);

    std::optional<CachedImport> load(const std::string& asset, const std::string& key) const;
    bool store(const std::string& asset, const std::string& key, const CachedImport& entry) const;
    void remove(const std::string& asset, const std::string& key) const;

private:
    std::filesystem::path dir_;
};

} // namespace tesseract::views::world
