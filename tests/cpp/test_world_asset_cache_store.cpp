#include <catch2/catch_test_macros.hpp>

#include "views/world/AssetCacheStore.h"

#include <tesseract/hash.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace world_asset_cache_tests
{
using namespace tesseract::views::world;
namespace fs = std::filesystem;

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("tess_wac_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(n++));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

CachedImport sample()
{
    CachedImport e;
    CachedResource a;
    a.id = 11;
    a.type = 2;
    a.path = "textures/a.png";
    for (int i = 0; i < 16; ++i)
    {
        a.content_hash[i] = static_cast<std::uint8_t>(i + 1);
    }
    a.blob = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CachedResource b;
    b.id = 22;
    b.type = 5;
    b.path = "level.bin";
    b.content_hash[0] = 0xAB;
    b.blob = std::vector<std::uint8_t>(300, 0x5A);
    e.resources = {a, b};
    e.level_id = 22;
    e.manifest = "{\"m\":1}";
    e.warnings = "w1\nw2";
    e.summary = "two resources";
    return e;
}

std::vector<std::uint8_t> slurp(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void spit(const fs::path& p, const std::vector<std::uint8_t>& d)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

fs::path file_for(const TempDir& d, const std::string& asset, const std::string& key)
{
    return d.path / (asset + "-" + key + ".twa");
}

tesseract::Sha256Digest digest_of(const std::string& s)
{
    return tesseract::sha256(s.data(), s.size());
}

void resign(std::vector<std::uint8_t>& d)
{
    d.resize(d.size() - 32);
    const auto sum = tesseract::sha256(d.data(), d.size());
    d.insert(d.end(), sum.begin(), sum.end());
}

void patch(std::vector<std::uint8_t>& d, std::size_t at, std::uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
    {
        d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

TEST_CASE("assetcache: round trip", "[world][assetcache]")
{
    TempDir d;
    AssetCacheStore s(d.path);
    CachedImport e = sample();
    REQUIRE(s.store("level_1", "k1", e));
    auto got = s.load("level_1", "k1");
    REQUIRE(got.has_value());
    REQUIRE(got->level_id == e.level_id);
    REQUIRE(got->manifest == e.manifest);
    REQUIRE(got->warnings == e.warnings);
    REQUIRE(got->summary == e.summary);
    REQUIRE(got->resources.size() == e.resources.size());
    for (std::size_t i = 0; i < e.resources.size(); ++i)
    {
        REQUIRE(got->resources[i].id == e.resources[i].id);
        REQUIRE(got->resources[i].type == e.resources[i].type);
        REQUIRE(got->resources[i].path == e.resources[i].path);
        REQUIRE(got->resources[i].content_hash == e.resources[i].content_hash);
        REQUIRE(got->resources[i].blob == e.resources[i].blob);
    }
    REQUIRE_FALSE(s.load("level_1", "other").has_value());
}

TEST_CASE("assetcache: cache key", "[world][assetcache]")
{
    auto src = digest_of("a");
    std::string base = make_cache_key(src, 1, 2, "stamp");
    REQUIRE(base.size() == 32);
    for (char c : base)
    {
        REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
    REQUIRE(base == make_cache_key(src, 1, 2, "stamp"));
    REQUIRE(base != make_cache_key(digest_of("b"), 1, 2, "stamp"));
    REQUIRE(base != make_cache_key(src, 3, 2, "stamp"));
    REQUIRE(base != make_cache_key(src, 1, 4, "stamp"));
    REQUIRE(base != make_cache_key(src, 1, 2, "stamp2"));
}

TEST_CASE("assetcache: second key replaces the first for the same asset", "[world][assetcache]")
{
    TempDir d;
    AssetCacheStore s(d.path);
    REQUIRE(s.store("a", "k1", sample()));
    REQUIRE(s.store("b", "k1", sample()));
    REQUIRE(s.store("a", "k2", sample()));
    REQUIRE_FALSE(fs::exists(file_for(d, "a", "k1")));
    REQUIRE(fs::exists(file_for(d, "a", "k2")));
    REQUIRE(fs::exists(file_for(d, "b", "k1")));
    s.remove("a", "k2");
    REQUIRE_FALSE(fs::exists(file_for(d, "a", "k2")));
}

TEST_CASE("assetcache: corrupt files give nullopt", "[world][assetcache]")
{
    TempDir d;
    AssetCacheStore s(d.path);
    REQUIRE(s.store("a", "k", sample()));
    const auto path = file_for(d, "a", "k");
    const auto good = slurp(path);
    REQUIRE(good.size() > 100);

    SECTION("truncated at 20 offsets")
    {
        for (int i = 0; i < 20; ++i)
        {
            std::size_t at = good.size() * static_cast<std::size_t>(i) / 20;
            spit(path, std::vector<std::uint8_t>(good.begin(),
                                                 good.begin() + static_cast<std::ptrdiff_t>(at)));
            REQUIRE_NOTHROW(s.load("a", "k"));
            REQUIRE_FALSE(s.load("a", "k").has_value());
        }
    }
    SECTION("flipped blob byte")
    {
        auto bad = good;
        bad[good.size() - 32 - 5] ^= 0xFF;
        spit(path, bad);
        REQUIRE_FALSE(s.load("a", "k").has_value());
    }
    SECTION("version changed")
    {
        auto bad = good;
        bad[4] = 99;
        spit(path, bad);
        REQUIRE_FALSE(s.load("a", "k").has_value());
    }
    SECTION("huge resource count")
    {
        auto bad = good;
        bad[8] = 0x00;
        bad[9] = 0x28;
        bad[10] = 0x6B;
        bad[11] = 0xEE; // 4,000,000,000
        spit(path, bad);
        REQUIRE_FALSE(s.load("a", "k").has_value());
    }
    SECTION("level id not a resource id")
    {
        CachedImport e = sample();
        e.level_id = 999;
        REQUIRE(s.store("a", "k", e));
        REQUIRE_FALSE(s.load("a", "k").has_value());
    }
}

TEST_CASE("assetcache: re-signed files hit the limit checks", "[world][assetcache]")
{
    TempDir d;
    AssetCacheStore s(d.path);
    const CachedImport e = sample();
    REQUIRE(s.store("a", "k", e));
    const auto path = file_for(d, "a", "k");
    const auto good = slurp(path);

    // Field offsets for sample(): header 20 bytes, then three length-prefixed texts.
    const std::size_t manifest_len_at = 20;
    const std::size_t res0 =
        20 + 8 + e.manifest.size() + 8 + e.warnings.size() + 8 + e.summary.size();
    const std::size_t path_len_at = res0 + 8 + 4;
    const std::size_t blob_len_at = path_len_at + 4 + e.resources[0].path.size() + 16;

    auto check = [&](const char* what, std::vector<std::uint8_t> bad)
    {
        INFO(what);
        resign(bad);
        spit(path, bad);
        REQUIRE_FALSE(s.load("a", "k").has_value());
    };

    // Control: a re-signed unmodified file still loads.
    {
        auto same = good;
        resign(same);
        spit(path, same);
        REQUIRE(s.load("a", "k").has_value());
    }

    auto b = good;
    patch(b, 8, 4000000000ull, 4);
    check("count 4e9", b);
    b = good;
    patch(b, 8, 4097, 4);
    check("count 4097", b);
    b = good;
    patch(b, 4, 99, 4);
    check("version 99", b);
    b = good;
    patch(b, manifest_len_at, 0x7FFFFFFFFFFFFFFFull, 8);
    check("manifest length huge", b);
    b = good;
    patch(b, manifest_len_at, 17ull << 20, 8);
    check("manifest length over 16 MiB", b);
    b = good;
    patch(b, path_len_at, 5000, 4);
    check("path length 5000", b);
    b = good;
    patch(b, blob_len_at, 1ull << 40, 8);
    check("blob length 2^40", b);
    b = good;
    patch(b, blob_len_at, 257ull << 20, 8);
    check("blob length over 256 MiB", b);
    b = good;
    b.insert(b.end() - 32, {1, 2, 3});
    check("trailing junk", b);
}

TEST_CASE("assetcache: unusable directory", "[world][assetcache]")
{
    TempDir d;
    spit(d.path / "file", {1, 2, 3});
    AssetCacheStore s(d.path / "file" / "sub");
    REQUIRE_FALSE(s.store("a", "k", sample()));
    REQUIRE_FALSE(s.load("a", "k").has_value());
}

TEST_CASE("assetcache: stale tmp ignored then removed", "[world][assetcache]")
{
    TempDir d;
    AssetCacheStore s(d.path);
    const auto tmp = d.path / "a-k.1.tmp";
    spit(tmp, {1, 2, 3});
    fs::last_write_time(tmp, fs::file_time_type::clock::now() - std::chrono::hours(3));
    REQUIRE_FALSE(s.load("a", "k").has_value());
    REQUIRE(s.store("a", "k", sample()));
    REQUIRE_FALSE(fs::exists(tmp));
}

TEST_CASE("assetcache: bad asset names refused", "[world][assetcache]")
{
    TempDir d;
    AssetCacheStore s(d.path);
    for (const char* n : {"../x", "", "A"})
    {
        REQUIRE_FALSE(s.store(n, "k", sample()));
        REQUIRE_FALSE(s.load(n, "k").has_value());
    }
}

TEST_CASE("assetcache: second store instance reads the entry", "[world][assetcache]")
{
    TempDir d;
    REQUIRE(AssetCacheStore(d.path).store("a", "k", sample()));
    auto got = AssetCacheStore(d.path).load("a", "k");
    REQUIRE(got.has_value());
    REQUIRE(got->summary == "two resources");
}

TEST_CASE("assetcache: 64-character asset names round trip, 65 are refused", "[world][assetcache]")
{
    TempDir d;
    AssetCacheStore s(d.path);
    const CachedImport e = sample();
    const std::string ok(64, 'a');
    REQUIRE(s.store(ok, "k1", e));
    REQUIRE(s.load(ok, "k1").has_value());
    const std::string too_long(65, 'a');
    REQUIRE_FALSE(s.store(too_long, "k1", e));
    REQUIRE_FALSE(s.load(too_long, "k1").has_value());
}

} // namespace world_asset_cache_tests
