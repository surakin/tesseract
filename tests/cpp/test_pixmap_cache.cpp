#include <catch2/catch_test_macros.hpp>

#include "tk/pixmap_cache.h"

#include <chrono>
#include <memory>

using tk::PixmapCache;

namespace
{

struct PixmapCacheFakeImage : tk::Image
{
    explicit PixmapCacheFakeImage(std::size_t bytes) : bytes_(bytes) {}
    int width() const override { return 1; }
    int height() const override { return 1; }
    std::size_t memory_bytes() const override { return bytes_; }
    std::size_t bytes_;
};

struct GrowingImage : tk::Image
{
    int width() const override { return 1; }
    int height() const override { return 1; }
    std::size_t memory_bytes() const override { return bytes; }
    std::size_t bytes = 100;
};

std::unique_ptr<tk::Image> img(std::size_t bytes)
{
    return std::make_unique<PixmapCacheFakeImage>(bytes);
}

} // namespace

TEST_CASE("store retains the image; peek/acquire return it", "[pixmap-cache]")
{
    PixmapCache c;
    tk::ImageRef ref = c.store(tk::CacheKey::media("a"), img(100));
    REQUIRE(ref);
    CHECK(c.contains(tk::CacheKey::media("a")));
    CHECK(c.current_bytes() == 100);
    CHECK(c.peek(tk::CacheKey::media("a")) == ref.get());
    CHECK(c.acquire(tk::CacheKey::media("a")).get() == ref.get());
    CHECK(c.peek(tk::CacheKey::media("missing")) == nullptr);
    CHECK(c.acquire(tk::CacheKey::media("missing")) == nullptr);
}

TEST_CASE("sweep evicts only expired, unreferenced entries", "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store(tk::CacheKey::media("a"), img(100));            // return discarded → cache-only
    tk::ImageRef pinned = c.store(tk::CacheKey::media("b"), img(100)); // pinned by `pinned`

    now += seconds{10};
    c.sweep();
    CHECK(c.contains(tk::CacheKey::media("a"))); // not yet expired
    CHECK(c.contains(tk::CacheKey::media("b")));

    now += seconds{31};
    c.sweep();
    CHECK_FALSE(c.contains(tk::CacheKey::media("a"))); // expired + unreferenced → gone
    CHECK(c.contains(tk::CacheKey::media("b")));       // expired but pinned → kept
    CHECK(c.current_bytes() == 100);

    pinned.reset(); // last external ref dropped
    now += seconds{31};
    c.sweep();
    CHECK_FALSE(c.contains(tk::CacheKey::media("b")));
    CHECK(c.current_bytes() == 0);
}

TEST_CASE("peek resets the TTL clock", "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store(tk::CacheKey::media("a"), img(100));
    now += seconds{20};
    CHECK(c.peek(tk::CacheKey::media("a")) != nullptr); // resets last_use to 20s

    now += seconds{20}; // 40s since store, 20s since the peek
    c.sweep();
    CHECK(c.contains(tk::CacheKey::media("a"))); // kept — peek kept it warm

    now += seconds{31};
    c.sweep();
    CHECK_FALSE(c.contains(tk::CacheKey::media("a")));
}

TEST_CASE("over-budget sweep evicts LRU unreferenced, never pinned",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(/*max_bytes=*/250, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store(tk::CacheKey::media("a"), img(100));
    now += seconds{1};
    c.store(tk::CacheKey::media("b"), img(100));
    now += seconds{1};
    tk::ImageRef pinned = c.store(tk::CacheKey::media("c"), img(100));
    now += seconds{1};

    // 300 > 250 and nothing is TTL-expired, so the budget pass evicts the
    // oldest UNREFERENCED entry ("a") until under budget. "c" is pinned.
    c.sweep();
    CHECK_FALSE(c.contains(tk::CacheKey::media("a")));
    CHECK(c.contains(tk::CacheKey::media("b")));
    CHECK(c.contains(tk::CacheKey::media("c")));
    CHECK(c.current_bytes() == 200);
}

TEST_CASE("clear drops cache refs; outstanding handles stay alive",
          "[pixmap-cache]")
{
    PixmapCache c;
    tk::ImageRef held = c.store(tk::CacheKey::media("x"), img(100));
    CHECK(c.current_bytes() == 100);

    c.clear();
    CHECK_FALSE(c.contains(tk::CacheKey::media("x")));
    CHECK(c.current_bytes() == 0);

    REQUIRE(held); // image kept alive by the outstanding handle
    CHECK(held->memory_bytes() == 100);
}

TEST_CASE("evict removes only unreferenced entries", "[pixmap-cache]")
{
    PixmapCache c;
    tk::ImageRef pinned = c.store(tk::CacheKey::media("p"), img(50));
    c.store(tk::CacheKey::media("u"), img(50)); // cache-only

    c.evict(tk::CacheKey::media("p")); // pinned → no-op
    c.evict(tk::CacheKey::media("u")); // unreferenced → removed
    CHECK(c.contains(tk::CacheKey::media("p")));
    CHECK_FALSE(c.contains(tk::CacheKey::media("u")));
    CHECK(c.current_bytes() == 50);
}

TEST_CASE("acquire counts hits and misses", "[pixmap-cache]")
{
    PixmapCache c;
    c.store(tk::CacheKey::media("a"), img(100));

    c.acquire(tk::CacheKey::media("a"));       // hit
    c.acquire(tk::CacheKey::media("missing")); // miss
    c.acquire(tk::CacheKey::media("a"));       // hit

    CHECK(c.hits()   == 2);
    CHECK(c.misses() == 1);
}

TEST_CASE("peek counts hits and misses", "[pixmap-cache]")
{
    PixmapCache c;
    c.store(tk::CacheKey::media("a"), img(100));

    c.peek(tk::CacheKey::media("a"));    // hit
    c.peek(tk::CacheKey::media("nope")); // miss

    CHECK(c.hits()   == 1);
    CHECK(c.misses() == 1);
}

TEST_CASE("clear resets hit/miss counters", "[pixmap-cache]")
{
    PixmapCache c;
    c.store(tk::CacheKey::media("a"), img(100));
    c.acquire(tk::CacheKey::media("a"));       // hit
    c.acquire(tk::CacheKey::media("missing")); // miss
    REQUIRE(c.hits() == 1);

    c.clear();
    CHECK(c.hits()   == 0);
    CHECK(c.misses() == 0);
}

TEST_CASE("retain_recent evicts only once BOTH generation- and time-stale",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store(tk::CacheKey::media("a"), img(100)); // gen 0, last_use = now(0)
    c.peek(tk::CacheKey::media("a"));

    // Generation-stale but NOT time-stale → kept (protects a just-fetched
    // image whose widget has not painted yet).
    c.advance_generation();
    c.advance_generation();
    c.retain_recent(2);
    CHECK(c.contains(tk::CacheKey::media("a")));

    // Now also time-stale → evicted.
    now += seconds{31};
    c.retain_recent(2);
    CHECK_FALSE(c.contains(tk::CacheKey::media("a")));
    CHECK(c.current_bytes() == 0);
}

TEST_CASE("retain_recent keeps a time-stale entry still marked this generation",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store(tk::CacheKey::media("a"), img(100));
    now += seconds{31}; // time-stale
    c.advance_generation();
    c.peek(tk::CacheKey::media("a"));        // but re-marked this generation
    c.retain_recent(2);
    CHECK(c.contains(tk::CacheKey::media("a")));
}

TEST_CASE("retain_recent spares an entry re-peeked each generation",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });
    c.store(tk::CacheKey::media("a"), img(100));

    for (int i = 0; i < 10; ++i)
    {
        now += seconds{5}; // wall-clock keeps advancing past the TTL
        c.advance_generation();
        c.peek(tk::CacheKey::media("a"));       // marks it live at the new gen
        c.retain_recent(2);
    }
    CHECK(c.contains(tk::CacheKey::media("a")));
}

TEST_CASE("retain_recent never evicts a pinned entry", "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });
    tk::ImageRef pinned = c.store(tk::CacheKey::media("a"), img(100)); // held by `pinned`

    for (int i = 0; i < 5; ++i)
    {
        now += seconds{31};
        c.advance_generation();
        c.retain_recent(2); // gen- + time-stale, but use_count() > 1 → kept
    }
    CHECK(c.contains(tk::CacheKey::media("a")));
}

TEST_CASE("retain_recent drops the cache ref but a held handle survives",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });
    tk::ImageRef held = c.acquire(tk::CacheKey::media("missing")); // null
    c.store(tk::CacheKey::media("a"), img(100));
    held = c.acquire(tk::CacheKey::media("a")); // use_count() == 2

    // Not evicted while held.
    now += seconds{31};
    c.advance_generation();
    c.advance_generation();
    c.retain_recent(2);
    CHECK(c.contains(tk::CacheKey::media("a")));

    // Once released, the next retain reclaims it.
    held.reset();
    c.advance_generation();
    c.retain_recent(2);
    CHECK_FALSE(c.contains(tk::CacheKey::media("a")));
}

TEST_CASE("current_bytes tracks an image that grows after store",
          "[pixmap-cache]")
{
    PixmapCache c;
    auto g = std::make_unique<GrowingImage>();
    GrowingImage* raw = g.get();
    c.store(tk::CacheKey::media("g"), std::move(g));
    CHECK(c.current_bytes() == 100);
    raw->bytes = 700; // backend memoised extra scaled copies while painting
    CHECK(c.current_bytes() == 700);
}

TEST_CASE("sweep budgets against live image sizes", "[pixmap-cache]")
{
    using namespace std::chrono;
    PixmapCache c(/*max_bytes=*/500, seconds{30});
    auto g = std::make_unique<GrowingImage>();
    GrowingImage* raw = g.get();
    c.store(tk::CacheKey::media("g"), std::move(g));
    raw->bytes = 900;
    c.sweep(); // stored size (100) fit, live size (900) does not
    CHECK_FALSE(c.contains(tk::CacheKey::media("g")));
}

TEST_CASE("total_bytes_all_instances sums live caches", "[pixmap-cache]")
{
    const std::size_t base = PixmapCache::total_bytes_all_instances();
    {
        PixmapCache a, b;
        a.store(tk::CacheKey::media("a"), img(100));
        b.store(tk::CacheKey::media("b"), img(50));
        CHECK(PixmapCache::total_bytes_all_instances() == base + 150);
    }
    CHECK(PixmapCache::total_bytes_all_instances() == base);
}
