#include <catch2/catch_test_macros.hpp>

#include "tk/compressed_image_cache.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using tk::CompressedImageCache;

namespace
{

std::vector<std::uint8_t> blob(std::size_t n, std::uint8_t fill = 0xAB)
{
    return std::vector<std::uint8_t>(n, fill);
}

} // namespace

TEST_CASE("put/get round-trips the bytes", "[compressed-image-cache]")
{
    CompressedImageCache c;
    c.put(tk::CacheKey::media("a"), blob(100, 0x11));

    auto got = c.get(tk::CacheKey::media("a"));
    REQUIRE(got);
    CHECK(got->size() == 100);
    CHECK(got->front() == 0x11);
    CHECK(c.current_bytes() == 100);
    CHECK(c.hits() == 1);

    CHECK(c.get(tk::CacheKey::media("missing")) == nullptr);
    CHECK(c.misses() == 1);
}

TEST_CASE("empty and oversized entries are ignored", "[compressed-image-cache]")
{
    CompressedImageCache c(/*max_bytes=*/1024, /*max_entry_bytes=*/256);
    c.put(tk::CacheKey::media("empty"), {});
    c.put(tk::CacheKey::media("huge"), blob(257));

    CHECK(c.get(tk::CacheKey::media("empty")) == nullptr);
    CHECK(c.get(tk::CacheKey::media("huge")) == nullptr);
    CHECK(c.size() == 0);
    CHECK(c.current_bytes() == 0);
}

TEST_CASE("over-budget put evicts least-recently-used", "[compressed-image-cache]")
{
    CompressedImageCache c(/*max_bytes=*/300, /*max_entry_bytes=*/1024);
    c.put(tk::CacheKey::media("a"), blob(100));
    c.put(tk::CacheKey::media("b"), blob(100));
    c.put(tk::CacheKey::media("c"), blob(100));
    CHECK(c.current_bytes() == 300);

    // Touch "a" so "b" becomes the LRU victim.
    REQUIRE(c.get(tk::CacheKey::media("a")));

    c.put(tk::CacheKey::media("d"), blob(100)); // pushes over budget → evict "b"
    CHECK(c.current_bytes() == 300);
    CHECK(c.get(tk::CacheKey::media("b")) == nullptr);
    CHECK(c.get(tk::CacheKey::media("a")));
    CHECK(c.get(tk::CacheKey::media("c")));
    CHECK(c.get(tk::CacheKey::media("d")));
}

TEST_CASE("replace updates byte accounting", "[compressed-image-cache]")
{
    CompressedImageCache c(/*max_bytes=*/1024, /*max_entry_bytes=*/1024);
    c.put(tk::CacheKey::media("a"), blob(100));
    c.put(tk::CacheKey::media("a"), blob(40));
    CHECK(c.current_bytes() == 40);
    CHECK(c.size() == 1);
    CHECK(c.get(tk::CacheKey::media("a"))->size() == 40);
}

TEST_CASE("held buffer outlives evict() and clear()", "[compressed-image-cache]")
{
    CompressedImageCache c;
    c.put(tk::CacheKey::media("a"), blob(10, 0x7E));
    auto held = c.get(tk::CacheKey::media("a"));
    REQUIRE(held);

    c.evict(tk::CacheKey::media("a"));
    CHECK(c.get(tk::CacheKey::media("a")) == nullptr);
    CHECK(held->front() == 0x7E); // still valid

    c.put(tk::CacheKey::media("b"), blob(10));
    c.clear();
    CHECK(c.get(tk::CacheKey::media("b")) == nullptr);
    CHECK(c.current_bytes() == 0);
    CHECK(held->front() == 0x7E); // still valid
}

TEST_CASE("concurrent get/put stays consistent", "[compressed-image-cache]")
{
    CompressedImageCache c(/*max_bytes=*/64u * 1024, /*max_entry_bytes=*/4096);
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back(
            [&, t]()
            {
                while (!go.load())
                {
                }
                for (int i = 0; i < 2000; ++i)
                {
                    const std::string key =
                        "k" + std::to_string((t * 7 + i) % 64);
                    if (i % 3 == 0)
                    {
                        c.put(tk::CacheKey::media(key), blob(128 + (i % 512)));
                    }
                    else
                    {
                        auto b = c.get(tk::CacheKey::media(key));
                        if (b)
                        {
                            // Touch the buffer to catch use-after-free under TSan/ASan.
                            volatile std::uint8_t x = b->empty() ? 0 : b->front();
                            (void) x;
                        }
                    }
                }
            });
    }
    go.store(true);
    for (auto& th : threads)
    {
        th.join();
    }

    CHECK(c.current_bytes() <= c.max_bytes()); // budget held under contention
    CHECK(c.size() <= 64);
}
