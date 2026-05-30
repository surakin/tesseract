#include <catch2/catch_test_macros.hpp>

#include "tk/media_disk_cache.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

struct TmpDir
{
    fs::path path;
    TmpDir()
        : path(fs::temp_directory_path() / "tesseract_test_media_disk_cache")
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
    CHECK(c.load("absent").empty());
}

TEST_CASE("load counts hits and misses", "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    std::vector<uint8_t> bytes{1, 2, 3};
    c.store("k", bytes);

    c.load("k");       // hit
    c.load("missing"); // miss
    c.load("k");       // hit

    CHECK(c.hits()   == 2);
    CHECK(c.misses() == 1);
}

TEST_CASE("clear resets hit/miss counters", "[media-disk-cache]")
{
    TmpDir tmp;
    tk::MediaDiskCache c(tmp.path);
    std::vector<uint8_t> bytes{1, 2, 3};
    c.store("k", bytes);
    c.load("k");       // hit
    c.load("missing"); // miss
    REQUIRE(c.hits() == 1);

    c.clear();
    CHECK(c.hits()   == 0);
    CHECK(c.misses() == 0);
}
