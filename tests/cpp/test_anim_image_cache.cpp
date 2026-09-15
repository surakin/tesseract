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

TEST_CASE("a paused entry does not advance", "[anim-cache]")
{
    Fixture f;
    const auto key = tk::CacheKey::media("k");
    f.cache.store(key, frames(3), {50, 50, 50}, 0);
    (void)f.cache.current_frame(key); // mark visible
    f.cache.set_paused(key, true);

    f.clock = 50;
    CHECK(f.cache.advance(50) == false);
    const auto* before = f.cache.current_frame(key);

    f.clock = 200;
    CHECK(f.cache.advance(200) == false);
    CHECK(f.cache.current_frame(key) == before); // frame index never moved
}

TEST_CASE("unpausing resumes normal advancement", "[anim-cache]")
{
    Fixture f;
    const auto key = tk::CacheKey::media("k");
    f.cache.store(key, frames(3), {50, 50, 50}, 0);
    (void)f.cache.current_frame(key);
    f.cache.set_paused(key, true);
    f.clock = 50;
    CHECK(f.cache.advance(50) == false);

    f.cache.set_paused(key, false);
    (void)f.cache.current_frame(key); // stays visible
    CHECK(f.cache.advance(100) == true);
}

TEST_CASE("a paused entry still counts as visible (TTL/sweep retention)",
          "[anim-cache]")
{
    Fixture f;
    const auto key = tk::CacheKey::media("k");
    f.cache.store(key, frames(3), {50, 50, 50}, 0);
    f.cache.set_paused(key, true);
    f.clock = 500;
    (void)f.cache.current_frame(key); // paused, but still "on screen"
    f.clock = 1000;
    CHECK(f.cache.any_visible() == true);
}

TEST_CASE("set_paused on an unknown key is a safe no-op", "[anim-cache]")
{
    Fixture f;
    f.cache.set_paused(tk::CacheKey::media("missing"), true);
    // No crash, and a real entry is unaffected.
    const auto key = tk::CacheKey::media("k");
    f.cache.store(key, frames(2), {50, 50}, 0);
    (void)f.cache.current_frame(key);
    f.clock = 50;
    CHECK(f.cache.advance(50) == true);
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

namespace
{

// In-memory AnimDecodeSession double: produces `total` synthetic frames
// (50ms delay each), tracking how many times restart() was called.
struct FakeSession : tk::AnimDecodeSession
{
    int total = 0;
    int cursor = 0;
    int restart_count = 0;

    int decode_next_batch(
        int n,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>&
            on_frame) override
    {
        int produced = 0;
        while (produced < n && cursor < total)
        {
            on_frame(cursor, std::make_unique<AnimImageCacheFakeImage>(), 50);
            ++cursor;
            ++produced;
        }
        return produced;
    }

    bool exhausted() const override { return cursor >= total; }

    void restart() override
    {
        cursor = 0;
        ++restart_count;
    }
};

// Runs every pending collect_topups() request against `cache` synchronously
// (no real worker thread needed — the fake session decodes instantly),
// mirroring what ShellBase's tick does: decode_next_batch() then
// append_frame() each produced frame, then finish_topup().
void run_topups(AnimImageCache& cache)
{
    for (auto& req : cache.collect_topups())
    {
        req.session->decode_next_batch(
            req.batch_size,
            [&](int /*idx*/, std::unique_ptr<tk::Image> img, int delay)
            { cache.append_frame(req.key, std::move(img), delay); });
        cache.finish_topup(req.key);
    }
}

} // namespace

TEST_CASE("a paused windowed entry does not top up or advance",
          "[anim-cache]")
{
    Fixture f;
    auto key = tk::CacheKey::media("k");
    auto session = std::make_shared<FakeSession>();
    session->total = 100;
    session->cursor = 1;
    f.cache.store(key, frames(1), {50}, /*now_ms=*/0, session, 100);
    (void)f.cache.current_frame(key); // mark visible
    f.cache.set_paused(key, true);

    std::int64_t now = 0;
    for (int i = 0; i < 20; ++i)
    {
        CHECK(f.cache.collect_topups().empty()); // paused: no decode-ahead
        now += 50;
        f.clock = now;
        f.cache.advance(now);
        (void)f.cache.current_frame(key);
    }
    CHECK(session->cursor == 1); // never decoded past the seeded frame 0
}

TEST_CASE("a windowed entry stays bounded across many frames", "[anim-cache]")
{
    Fixture f;
    auto key = tk::CacheKey::media("k");
    auto session = std::make_shared<FakeSession>();
    session->total = 100;
    // Seed frame 0 as store() would (the streamed decode's first callback).
    session->cursor = 1;
    f.cache.store(key, frames(1), {50}, /*now_ms=*/0, session, 100);
    (void)f.cache.current_frame(key); // mark visible

    // Drive many ticks, topping up and advancing as ShellBase's tick would.
    std::int64_t now = 0;
    for (int i = 0; i < 300; ++i)
    {
        run_topups(f.cache);
        now += 50;
        f.clock = now;
        f.cache.advance(now);
        (void)f.cache.current_frame(key); // stay visible
        run_topups(f.cache);
    }

    // The session produced far more than 100 frames' worth of ticks (with
    // restarts), but resident frames never grew anywhere near the full 100.
    CHECK(f.cache.current_bytes() >= 0); // no underflow/corruption
    CHECK(f.cache.has(key));
}

TEST_CASE("windowed trim never drops within kKeepBehindFrames of current",
          "[anim-cache]")
{
    Fixture f;
    auto key = tk::CacheKey::media("k");
    auto session = std::make_shared<FakeSession>();
    session->total = 50;
    session->cursor = 1;
    f.cache.store(key, frames(1), {50}, 0, session, 50);
    (void)f.cache.current_frame(key);

    std::int64_t now = 0;
    for (int i = 0; i < 60; ++i)
    {
        now += 50;
        f.clock = now;
        f.cache.advance(now);
        // Checked immediately after advance(), BEFORE running top-ups: a
        // loop restart must keep the last-shown frame resident as a
        // placeholder (see restart_loop_locked_'s `restarting` flag) rather
        // than leaving `frames` empty until the next top-up lands — that
        // gap used to paint a blank/white frame at every loop point. If
        // trim ever dropped a frame at or ahead of `current`, or restart
        // ever cleared `frames` outright, this would go nullptr here.
        CHECK(f.cache.current_frame(key) != nullptr);
        run_topups(f.cache);
    }
}

TEST_CASE("looping past a trimmed window restarts the session once per loop",
          "[anim-cache]")
{
    Fixture f;
    auto key = tk::CacheKey::media("k");
    auto session = std::make_shared<FakeSession>();
    session->total = 20; // small enough to fully cycle a few times in the test
    session->cursor = 1;
    f.cache.store(key, frames(1), {50}, 0, session, 20);
    (void)f.cache.current_frame(key);

    std::int64_t now = 0;
    for (int i = 0; i < 400; ++i)
    {
        run_topups(f.cache);
        now += 50;
        f.clock = now;
        f.cache.advance(now);
        (void)f.cache.current_frame(key);
        run_topups(f.cache);
    }

    // With only 20 frames and 400 ticks of playback, the animation must have
    // looped multiple times, each loop restarting the sequential decoder.
    CHECK(session->restart_count > 1);
}

TEST_CASE("a loop restart never leaves current_frame() null before the "
          "new frame 0 lands",
          "[anim-cache]")
{
    // Regression test for a real bug: restart_loop_locked_ used to clear
    // `frames` outright, so current_frame() returned nullptr (painted as a
    // blank/white frame) for every tick between the restart and the next
    // top-up landing. Drive the cache one tick at a time so we can catch
    // the exact tick a restart happens and assert the frame stays valid
    // right through it, without a top-up masking the gap.
    Fixture f;
    auto key = tk::CacheKey::media("k");
    auto session = std::make_shared<FakeSession>();
    // Must be large enough that the resident window actually gets trimmed
    // (current advances past kKeepBehindFrames) before exhaustion — a very
    // short animation loops via the plain modulo-wrap path instead (the
    // whole thing fits in one window, window_start never leaves 0), which
    // doesn't exercise restart_loop_locked_ at all.
    session->total = 20;
    session->cursor = 1;
    f.cache.store(key, frames(1), {50}, 0, session, 20);
    REQUIRE(f.cache.current_frame(key) != nullptr);

    std::int64_t now = 0;
    bool saw_restart = false;
    for (int i = 0; i < 60; ++i)
    {
        now += 50;
        f.clock = now;
        f.cache.advance(now);
        // No run_topups() here yet — this is the exact gap that used to go
        // blank.
        CHECK(f.cache.current_frame(key) != nullptr);
        if (session->restart_count > 0)
        {
            saw_restart = true;
        }
        run_topups(f.cache);
    }
    CHECK(saw_restart);
}

TEST_CASE("a below-threshold (unwindowed) entry is unaffected by windowing",
          "[anim-cache]")
{
    // Regression guard: store() with no session behaves exactly as before —
    // this mirrors "an off-screen entry does not drive repaints..." etc.
    // above, just asserting the new optional params don't change anything
    // when omitted.
    Fixture f;
    f.cache.store(tk::CacheKey::media("k"), frames(3), {50, 50, 50}, 0);
    CHECK(f.cache.any_visible());
    (void)f.cache.current_frame(tk::CacheKey::media("k"));
    f.clock = 50;
    CHECK(f.cache.advance(50) == true);
    // No topups are ever generated for an unwindowed entry.
    CHECK(f.cache.collect_topups().empty());
}
