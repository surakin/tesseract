#include <catch2/catch_test_macros.hpp>
#include <tesseract/avatar_disk_cache.h>

#include <filesystem>
#include <fstream>
#include <vector>

using tesseract::AvatarDiskCache;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* suffix) {
        path = std::filesystem::temp_directory_path() / suffix;
        std::filesystem::remove_all(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

TEST_CASE("AvatarDiskCache round-trips bytes", "[cache][avatar]") {
    TempDir d("tk_adc_roundtrip");
    AvatarDiskCache cache(d.path);
    const std::string mxc = "mxc://example.org/abcdef";
    const std::vector<uint8_t> data = {1, 2, 3, 4, 5};

    REQUIRE(cache.get(mxc).empty());
    cache.put(mxc, data);
    REQUIRE(cache.get(mxc) == data);
}

TEST_CASE("AvatarDiskCache all_keys returns stored URLs", "[cache][avatar]") {
    TempDir d("tk_adc_keys");
    AvatarDiskCache cache(d.path);
    const std::string m1 = "mxc://matrix.org/AAA";
    const std::string m2 = "mxc://matrix.org/BBB";
    const std::vector<uint8_t> bytes = {10, 20};

    cache.put(m1, bytes);
    cache.put(m2, bytes);

    auto keys = cache.all_keys();
    std::sort(keys.begin(), keys.end());
    REQUIRE(keys.size() == 2);
    CHECK(keys[0] == m1);
    CHECK(keys[1] == m2);
}

TEST_CASE("AvatarDiskCache remove erases entry", "[cache][avatar]") {
    TempDir d("tk_adc_remove");
    AvatarDiskCache cache(d.path);
    const std::string mxc = "mxc://example.org/remove";
    cache.put(mxc, std::vector<uint8_t>{1, 2, 3});
    REQUIRE(!cache.get(mxc).empty());

    cache.remove(mxc);
    REQUIRE(cache.get(mxc).empty());
    REQUIRE(cache.all_keys().empty());
}

TEST_CASE("AvatarDiskCache clear empties all entries", "[cache][avatar]") {
    TempDir d("tk_adc_clear");
    AvatarDiskCache cache(d.path);
    cache.put("mxc://a/1", std::vector<uint8_t>{1});
    cache.put("mxc://a/2", std::vector<uint8_t>{2});
    cache.put("mxc://a/3", std::vector<uint8_t>{3});
    REQUIRE(cache.all_keys().size() == 3);

    cache.clear();
    REQUIRE(cache.all_keys().empty());
}

TEST_CASE("AvatarDiskCache malformed filenames are skipped by all_keys",
          "[cache][avatar]") {
    TempDir d("tk_adc_malformed");
    AvatarDiskCache cache(d.path);
    cache.put("mxc://good/url", std::vector<uint8_t>{42});

    // Plant a .bin file whose name cannot be base64url-decoded.
    auto bad = d.path / "!!!not_valid_base64url!!!.bin";
    { std::ofstream f(bad, std::ios::binary); f << "junk"; }

    auto keys = cache.all_keys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "mxc://good/url");
}

TEST_CASE("AvatarDiskCache put is atomic: no partial file visible on miss",
          "[cache][avatar]") {
    TempDir d("tk_adc_atomic");
    AvatarDiskCache cache(d.path);
    const std::string mxc = "mxc://example.org/atomic";

    // Before put, no .bin or .tmp file should exist.
    REQUIRE(cache.get(mxc).empty());
    for (const auto& e : std::filesystem::directory_iterator(d.path))
        FAIL("unexpected file before put: " << e.path());

    const std::vector<uint8_t> payload(1024, 0xAB);
    cache.put(mxc, payload);

    // After put, exactly one .bin file, no .tmp remnant.
    int bin_count = 0, tmp_count = 0;
    for (const auto& e : std::filesystem::directory_iterator(d.path)) {
        if (e.path().extension() == ".bin") ++bin_count;
        if (e.path().extension() == ".tmp") ++tmp_count;
    }
    CHECK(bin_count == 1);
    CHECK(tmp_count == 0);
    CHECK(cache.get(mxc) == payload);
}
