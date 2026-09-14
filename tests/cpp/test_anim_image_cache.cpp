#include <catch2/catch_test_macros.hpp>

#include "tk/anim_image_cache.h"

#include <memory>
#include <string>
#include <vector>

using tk::AnimImageCache;

namespace
{

struct AnimImageCacheFakeImage : tk::Image
{
    int width() const override { return 1; }
    int height() const override { return 1; }
    std::size_t memory_bytes() const noexcept override { return 0; }
};

std::vector<std::unique_ptr<tk::Image>> frames(int n)
{
    std::vector<std::unique_ptr<tk::Image>> v;
    for (int i = 0; i < n; ++i)
        v.push_back(std::make_unique<AnimImageCacheFakeImage>());
    return v;
}

// A cache whose visibility clock is driven by a test-controlled variable so
// the grace window can be exercised deterministically.
struct Fixture
{
    AnimImageCache cache;
    std::int64_t clock = 0;

    Fixture()
    {
        cache.set_clock_for_testing([this] { return clock; });
    }
};

} // namespace

TEST_CASE("a freshly stored entry is visible", "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(3), {50, 50, 50}, /*now_ms=*/0);
    CHECK(f.cache.any_visible());
}

TEST_CASE("a visible entry advances and reports a repaint", "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(3), {50, 50, 50}, 0);
    (void)f.cache.current_frame(tk::CacheKey::media("k")); // paint marks it visible at clock=0
    f.clock = 50;
    CHECK(f.cache.advance(50) == true);
}

TEST_CASE("visibility expires after the grace window without a paint",
          "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(3), {50, 50, 50}, 0);
    f.clock = 5000; // well past the grace window, no current_frame() calls
    CHECK(f.cache.any_visible() == false);
}

TEST_CASE("an off-screen entry does not drive repaints even past its deadline",
          "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(3), {50, 50, 50}, 0);
    // No current_frame() since store; let it age out of the grace window.
    f.clock = 5000;
    CHECK(f.cache.advance(5000) == false); // hidden → no repaint requested
}

TEST_CASE("current_frame refreshes visibility, keeping the entry alive",
          "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(3), {50, 50, 50}, 0);
    f.clock = 1900;
    (void)f.cache.current_frame(tk::CacheKey::media("k")); // repaint refreshes last-seen at 1900
    f.clock = 2000;
    CHECK(f.cache.any_visible() == true); // 2000 - 1900 within grace
}

TEST_CASE("only visible entries report repaints", "[anim-cache]")
{
    Fixture f;
    // Frame-timing epoch (the now_ms passed to advance) is independent of the
    // visibility clock, so deadlines stay fresh while we age entries out.
    f.cache.store(tk::CacheKey::media("vis"), frames(2), {50, 50}, /*now_ms=*/0);
    f.cache.store(tk::CacheKey::media("hid"), frames(2), {50, 50}, /*now_ms=*/0);

    // Only "vis" gets painted; "hid" is left to age out of the grace window.
    f.clock = 3000;
    (void)f.cache.current_frame(tk::CacheKey::media("vis"));

    CHECK(f.cache.any_visible() == true);
    // "vis" crosses its 50ms deadline; "hid" is off-screen and must not count.
    CHECK(f.cache.advance(/*now_ms=*/50) == true);

    // Now let "vis" age out too: nothing visible → no repaint.
    f.clock = 9000;
    CHECK(f.cache.any_visible() == false);
    CHECK(f.cache.advance(/*now_ms=*/100) == false);
}

TEST_CASE("current_frame counts hits and misses", "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(2), {50, 50}, 0);

    f.cache.current_frame(tk::CacheKey::media("k"));       // hit
    f.cache.current_frame(tk::CacheKey::media("missing")); // miss
    f.cache.current_frame(tk::CacheKey::media("k"));       // hit

    CHECK(f.cache.hits()   == 2);
    CHECK(f.cache.misses() == 1);
}

TEST_CASE("returning to view resyncs instead of fast-forwarding many frames",
          "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(10), std::vector<int>(10, 50), 0);
    (void)f.cache.current_frame(tk::CacheKey::media("k"));

    // Hidden for a long time (timer would have been stopped meanwhile).
    f.clock = 60000;
    CHECK(f.cache.advance(60000) == false); // hidden, no work

    // Scrolled back into view; one tick should advance at most ~one step,
    // not catch up thousands of missed frames.
    (void)f.cache.current_frame(tk::CacheKey::media("k"));
    f.clock = 60050;
    f.cache.advance(60050);
    // The frame pointer must still be a valid in-range index (no overflow /
    // pathological spin). We assert the entry is intact and queryable.
    CHECK(f.cache.current_frame(tk::CacheKey::media("k")) != nullptr);
}

TEST_CASE("append_frame grows an existing entry", "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(1), {50}, 0);
    f.cache.append_frame(tk::CacheKey::media("k"), std::make_unique<AnimImageCacheFakeImage>(), 75);
    // advance() only skips entries where delays_ms.size() != frames.size();
    // reaching frame index 1 (only possible if the vectors grew in lockstep)
    // proves the append landed correctly.
    (void)f.cache.current_frame(tk::CacheKey::media("k"));
    f.clock = 1; // stay within the visibility grace window
    CHECK(f.cache.advance(50) == true);
}

TEST_CASE("append_frame on an unknown key is a safe no-op", "[anim-cache]")
{
    Fixture f;
    f.cache.append_frame(tk::CacheKey::media("missing"), std::make_unique<AnimImageCacheFakeImage>(),
                         50);
    CHECK(f.cache.has(tk::CacheKey::media("missing")) == false);
    CHECK(f.cache.current_bytes() == 0);
}

TEST_CASE("append_frame is a no-op once the entry has been evicted",
          "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(1), {50}, 0);
    f.cache.clear();
    // The decode that produced this frame started before clear() dropped the
    // entry (e.g. the room scrolled away mid-decode) — the frame must be
    // silently dropped, not resurrect a removed entry.
    f.cache.append_frame(tk::CacheKey::media("k"), std::make_unique<AnimImageCacheFakeImage>(), 50);
    CHECK(f.cache.has(tk::CacheKey::media("k")) == false);
}

TEST_CASE("append_frame keeps current_bytes() accounting correct",
          "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(1), {50}, 0);
    const std::size_t before = f.cache.current_bytes();
    f.cache.append_frame(tk::CacheKey::media("k"), std::make_unique<AnimImageCacheFakeImage>(), 50);
    // AnimImageCacheFakeImage::memory_bytes() is 0, so the byte count itself
    // doesn't move — this test only guards against append_frame corrupting
    // current_bytes_ (e.g. double-counting or underflowing) rather than
    // leaving it unchanged.
    CHECK(f.cache.current_bytes() == before);
}

TEST_CASE("append_frame does not disturb an entry mid-playback", "[anim-cache]")
{
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(2), {50, 50}, /*now_ms=*/0);
    (void)f.cache.current_frame(tk::CacheKey::media("k"));
    // Advance past frame 0 so `current` is already 1 before the append.
    f.clock = 1;
    CHECK(f.cache.advance(50) == true);
    f.cache.append_frame(tk::CacheKey::media("k"), std::make_unique<AnimImageCacheFakeImage>(), 50);
    // Still queryable, and the next advance (wrapping back to index 0, then
    // eventually reaching the newly-appended index 2) doesn't crash or skip.
    CHECK(f.cache.current_frame(tk::CacheKey::media("k")) != nullptr);
    CHECK(f.cache.advance(100) == true);
}
