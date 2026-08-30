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

std::unique_ptr<tk::Image> img(std::size_t bytes)
{
    return std::make_unique<PixmapCacheFakeImage>(bytes);
}

} // namespace

TEST_CASE("store retains the image; peek/acquire return it", "[pixmap-cache]")
{
    PixmapCache c;
    tk::ImageRef ref = c.store("a", img(100));
    REQUIRE(ref);
    CHECK(c.contains("a"));
    CHECK(c.current_bytes() == 100);
    CHECK(c.peek("a") == ref.get());
    CHECK(c.acquire("a").get() == ref.get());
    CHECK(c.peek("missing") == nullptr);
    CHECK(c.acquire("missing") == nullptr);
}

TEST_CASE("sweep evicts only expired, unreferenced entries", "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store("a", img(100));            // return discarded → cache-only
    tk::ImageRef pinned = c.store("b", img(100)); // pinned by `pinned`

    now += seconds{10};
    c.sweep();
    CHECK(c.contains("a")); // not yet expired
    CHECK(c.contains("b"));

    now += seconds{31};
    c.sweep();
    CHECK_FALSE(c.contains("a")); // expired + unreferenced → gone
    CHECK(c.contains("b"));       // expired but pinned → kept
    CHECK(c.current_bytes() == 100);

    pinned.reset(); // last external ref dropped
    now += seconds{31};
    c.sweep();
    CHECK_FALSE(c.contains("b"));
    CHECK(c.current_bytes() == 0);
}

TEST_CASE("peek resets the TTL clock", "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store("a", img(100));
    now += seconds{20};
    CHECK(c.peek("a") != nullptr); // resets last_use to 20s

    now += seconds{20}; // 40s since store, 20s since the peek
    c.sweep();
    CHECK(c.contains("a")); // kept — peek kept it warm

    now += seconds{31};
    c.sweep();
    CHECK_FALSE(c.contains("a"));
}

TEST_CASE("over-budget sweep evicts LRU unreferenced, never pinned",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(/*max_bytes=*/250, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store("a", img(100));
    now += seconds{1};
    c.store("b", img(100));
    now += seconds{1};
    tk::ImageRef pinned = c.store("c", img(100));
    now += seconds{1};

    // 300 > 250 and nothing is TTL-expired, so the budget pass evicts the
    // oldest UNREFERENCED entry ("a") until under budget. "c" is pinned.
    c.sweep();
    CHECK_FALSE(c.contains("a"));
    CHECK(c.contains("b"));
    CHECK(c.contains("c"));
    CHECK(c.current_bytes() == 200);
}

TEST_CASE("clear drops cache refs; outstanding handles stay alive",
          "[pixmap-cache]")
{
    PixmapCache c;
    tk::ImageRef held = c.store("x", img(100));
    CHECK(c.current_bytes() == 100);

    c.clear();
    CHECK_FALSE(c.contains("x"));
    CHECK(c.current_bytes() == 0);

    REQUIRE(held); // image kept alive by the outstanding handle
    CHECK(held->memory_bytes() == 100);
}

TEST_CASE("evict removes only unreferenced entries", "[pixmap-cache]")
{
    PixmapCache c;
    tk::ImageRef pinned = c.store("p", img(50));
    c.store("u", img(50)); // cache-only

    c.evict("p"); // pinned → no-op
    c.evict("u"); // unreferenced → removed
    CHECK(c.contains("p"));
    CHECK_FALSE(c.contains("u"));
    CHECK(c.current_bytes() == 50);
}

TEST_CASE("acquire counts hits and misses", "[pixmap-cache]")
{
    PixmapCache c;
    c.store("a", img(100));

    c.acquire("a");       // hit
    c.acquire("missing"); // miss
    c.acquire("a");       // hit

    CHECK(c.hits()   == 2);
    CHECK(c.misses() == 1);
}

TEST_CASE("peek counts hits and misses", "[pixmap-cache]")
{
    PixmapCache c;
    c.store("a", img(100));

    c.peek("a");    // hit
    c.peek("nope"); // miss

    CHECK(c.hits()   == 1);
    CHECK(c.misses() == 1);
}

TEST_CASE("clear resets hit/miss counters", "[pixmap-cache]")
{
    PixmapCache c;
    c.store("a", img(100));
    c.acquire("a");       // hit
    c.acquire("missing"); // miss
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

    c.store("a", img(100)); // gen 0, last_use = now(0)
    c.peek("a");

    // Generation-stale but NOT time-stale → kept (protects a just-fetched
    // image whose widget has not painted yet).
    c.advance_generation();
    c.advance_generation();
    c.retain_recent(2);
    CHECK(c.contains("a"));

    // Now also time-stale → evicted.
    now += seconds{31};
    c.retain_recent(2);
    CHECK_FALSE(c.contains("a"));
    CHECK(c.current_bytes() == 0);
}

TEST_CASE("retain_recent keeps a time-stale entry still marked this generation",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });

    c.store("a", img(100));
    now += seconds{31}; // time-stale
    c.advance_generation();
    c.peek("a");        // but re-marked this generation
    c.retain_recent(2);
    CHECK(c.contains("a"));
}

TEST_CASE("retain_recent spares an entry re-peeked each generation",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });
    c.store("a", img(100));

    for (int i = 0; i < 10; ++i)
    {
        now += seconds{5}; // wall-clock keeps advancing past the TTL
        c.advance_generation();
        c.peek("a");       // marks it live at the new gen
        c.retain_recent(2);
    }
    CHECK(c.contains("a"));
}

TEST_CASE("retain_recent never evicts a pinned entry", "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });
    tk::ImageRef pinned = c.store("a", img(100)); // held by `pinned`

    for (int i = 0; i < 5; ++i)
    {
        now += seconds{31};
        c.advance_generation();
        c.retain_recent(2); // gen- + time-stale, but use_count() > 1 → kept
    }
    CHECK(c.contains("a"));
}

TEST_CASE("retain_recent drops the cache ref but a held handle survives",
          "[pixmap-cache]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    PixmapCache c(64u * 1024 * 1024, seconds{30});
    c.set_clock_for_testing([&] { return now; });
    tk::ImageRef held = c.acquire("missing"); // null
    c.store("a", img(100));
    held = c.acquire("a"); // use_count() == 2

    // Not evicted while held.
    now += seconds{31};
    c.advance_generation();
    c.advance_generation();
    c.retain_recent(2);
    CHECK(c.contains("a"));

    // Once released, the next retain reclaims it.
    held.reset();
    c.advance_generation();
    c.retain_recent(2);
    CHECK_FALSE(c.contains("a"));
}
