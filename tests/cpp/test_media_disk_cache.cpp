#include <catch2/catch_test_macros.hpp>

#include "tk/media_disk_cache.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// Unique per test case: ctest runs cases as parallel processes, and a shared
// path lets one case delete another's cache directory mid-test.
struct TmpDir
{
    fs::path path;
    TmpDir()
        : path(fs::temp_directory_path() /
               ("tesseract_test_media_disk_cache_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpDir()
    {
        fs::remove_all(path);
    }
};

} // namespace

TEST_CASE("load returns empty vector on cache miss", "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    CHECK(c.load(tk::CacheKey::media("absent")).empty());
}

TEST_CASE("load counts hits and misses", "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    std::vector<uint8_t> bytes{1, 2, 3};
    c.store(tk::CacheKey::media("k"), bytes);

    c.load(tk::CacheKey::media("k"));       // hit
    c.load(tk::CacheKey::media("missing")); // miss
    c.load(tk::CacheKey::media("k"));       // hit

    CHECK(c.hits() == 2);
    CHECK(c.misses() == 1);
}

TEST_CASE("clear resets hit/miss counters", "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    std::vector<uint8_t> bytes{1, 2, 3};
    c.store(tk::CacheKey::media("k"), bytes);
    c.load(tk::CacheKey::media("k"));       // hit
    c.load(tk::CacheKey::media("missing")); // miss
    REQUIRE(c.hits() == 1);

    c.clear();
    CHECK(c.hits() == 0);
    CHECK(c.misses() == 0);
}

TEST_CASE("retire_for_clear moves the entries aside and leaves an empty cache",
          "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    c.store(tk::CacheKey::media("k"), {1, 2, 3});
    c.load(tk::CacheKey::media("k"));       // hit
    c.load(tk::CacheKey::media("missing")); // miss

    const auto retired = c.retire_for_clear();
    REQUIRE_FALSE(retired.empty());
    CHECK(fs::exists(retired));
    CHECK(c.hits() == 0);
    CHECK(c.misses() == 0);
    CHECK(c.load(tk::CacheKey::media("k")).empty());
    CHECK(c.size_bytes() == 0);

    // The aside copy still holds the old entry until it is finished off.
    CHECK(!fs::is_empty(retired));
    c.finish_clear(retired);
    CHECK_FALSE(fs::exists(retired));

    // The cache is usable straight after.
    c.store(tk::CacheKey::media("k"), {9});
    CHECK(c.load(tk::CacheKey::media("k")) == std::vector<uint8_t>{9});
}

TEST_CASE("finish_clear with no aside copy removes the live entries", "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    c.store(tk::CacheKey::media("a"), {1});
    c.store(tk::CacheKey::media("b"), {2});
    REQUIRE(c.size_bytes() > 0);

    c.finish_clear({});
    CHECK(c.size_bytes() == 0);
    CHECK(fs::is_directory(tmp.path));
}

namespace
{
// The single entry file in `dir`, found by listing (the name is private).
fs::path only_file(const fs::path& dir)
{
    fs::path found;
    int count = 0;
    for (const auto& de : fs::directory_iterator(dir))
    {
        found = de.path();
        ++count;
    }
    REQUIRE(count == 1);
    return found;
}
} // namespace

TEST_CASE("an entry is served only under the key it was stored with", "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    const auto a = tk::CacheKey::media("mxc://example.org/a");
    const auto b = tk::CacheKey::media("mxc://example.org/b");

    // Simulate a 64-bit name collision: a's entry is moved to the name b uses.
    c.store(b, {9, 8});
    const fs::path b_file = only_file(tmp.path);
    fs::remove(b_file);
    c.store(a, {1, 2, 3});
    fs::rename(only_file(tmp.path), b_file);
    CHECK(c.load(b).empty());

    // An ordinary store under b still round-trips.
    c.store(b, {9, 8});
    CHECK(c.load(b) == std::vector<std::uint8_t>{9, 8});
}

TEST_CASE("opening the cache removes a retired directory an earlier clear left behind",
          "[media-disk-cache]")
{
    TmpDir tmp;
    const fs::path leftover(tmp.path.string() + ".clearing-1-2");
    fs::create_directories(leftover);
    std::ofstream(leftover / "entry") << "decrypted";
    tk::MediaDiskCache c(tmp.path);
    CHECK_FALSE(fs::exists(leftover));
    CHECK(fs::is_directory(tmp.path));
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("cache directory and entries are owner-only", "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    c.store(tk::CacheKey::media("mxc://example.org/p"), {7});
    const auto dir_perms = fs::status(tmp.path).permissions();
    CHECK((dir_perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    const auto file_perms = fs::status(only_file(tmp.path)).permissions();
    CHECK((file_perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
}
#endif
