#include <catch2/catch_test_macros.hpp>

#include "app/PresenceTracker.h"

#include <chrono>
#include <vector>

using tesseract::PresenceTracker;
using State = PresenceTracker::State;
using namespace std::chrono_literals;

namespace
{

// A small clock harness: the test advances `now` explicitly via `advance()`,
// and the tracker reads the current value through the injected provider.
struct FakeClock
{
    PresenceTracker::Clock::time_point now =
        PresenceTracker::Clock::time_point{} + 1h;

    PresenceTracker::NowProvider provider()
    {
        return [this] { return now; };
    }

    void advance(std::chrono::seconds s) { now += s; }
};

struct Recorder
{
    std::vector<State> seen;

    std::function<void(State)> sink()
    {
        return [this](State s) { seen.push_back(s); };
    }
};

} // namespace

TEST_CASE("PresenceTracker: starts Offline and only transitions after sync",
          "[presence][tracker]")
{
    FakeClock clk;
    PresenceTracker t{clk.provider()};
    Recorder rec;
    t.on_state_change = rec.sink();
    REQUIRE(t.current() == State::Offline);

    // notify_tick before sync should not transition.
    clk.advance(30min);
    t.notify_tick();
    REQUIRE(t.current() == State::Offline);
    REQUIRE(rec.seen.empty());
}

TEST_CASE("PresenceTracker: notify_sync_started fires Online exactly once",
          "[presence][tracker]")
{
    FakeClock clk;
    PresenceTracker t{clk.provider()};
    Recorder rec;
    t.on_state_change = rec.sink();

    t.notify_sync_started();
    REQUIRE(t.current() == State::Online);
    REQUIRE(rec.seen == std::vector<State>{State::Online});

    // Calling again is idempotent — already-Online stays put, no extra emit.
    t.notify_sync_started();
    REQUIRE(rec.seen == std::vector<State>{State::Online});
}

TEST_CASE("PresenceTracker: idle decay only after window becomes inactive",
          "[presence][tracker]")
{
    FakeClock clk;
    PresenceTracker t{clk.provider()};
    Recorder rec;
    t.on_state_change = rec.sink();

    t.notify_sync_started();
    REQUIRE(rec.seen.size() == 1);

    // Window stays active; ticks past the threshold should NOT decay.
    REQUIRE(t.window_active());
    clk.advance(t.idle_threshold() + 1min);
    t.notify_tick();
    REQUIRE(t.current() == State::Online);
    REQUIRE(rec.seen.size() == 1);

    // Window goes inactive. Reset the activity baseline at that moment by
    // calling notify_input so the threshold counts from "right after the
    // window lost focus", not from the original sync start.
    t.notify_input();
    t.notify_window_active(false);

    // Tick before deadline → no decay.
    clk.advance(t.idle_threshold() - 1min);
    t.notify_tick();
    REQUIRE(t.current() == State::Online);

    // Two more minutes past the threshold → Unavailable.
    clk.advance(2min);
    t.notify_tick();
    REQUIRE(t.current() == State::Unavailable);
    REQUIRE(rec.seen.back() == State::Unavailable);
}

TEST_CASE("PresenceTracker: input restores Online from Unavailable",
          "[presence][tracker]")
{
    FakeClock clk;
    PresenceTracker t{clk.provider()};
    Recorder rec;
    t.on_state_change = rec.sink();

    t.notify_sync_started();
    t.notify_window_active(false);
    clk.advance(t.idle_threshold() + 1min);
    t.notify_tick();
    REQUIRE(t.current() == State::Unavailable);

    // Keyboard / pointer input flips us back to Online.
    t.notify_input();
    REQUIRE(t.current() == State::Online);
    REQUIRE(rec.seen ==
            std::vector<State>{State::Online, State::Unavailable, State::Online});
}

TEST_CASE("PresenceTracker: window activation forces Online",
          "[presence][tracker]")
{
    FakeClock clk;
    PresenceTracker t{clk.provider()};
    Recorder rec;
    t.on_state_change = rec.sink();

    t.notify_sync_started();
    t.notify_window_active(false);
    clk.advance(t.idle_threshold() + 1min);
    t.notify_tick();
    REQUIRE(t.current() == State::Unavailable);

    t.notify_window_active(true);
    REQUIRE(t.current() == State::Online);
}

TEST_CASE("PresenceTracker: notify_logout forces Offline from any state",
          "[presence][tracker]")
{
    FakeClock clk;
    PresenceTracker t{clk.provider()};
    Recorder rec;
    t.on_state_change = rec.sink();

    t.notify_sync_started();
    t.notify_logout();
    REQUIRE(t.current() == State::Offline);
    REQUIRE(rec.seen.back() == State::Offline);

    // Subsequent ticks must not flip us back to Online — sync has stopped.
    clk.advance(1min);
    t.notify_tick();
    REQUIRE(t.current() == State::Offline);
}

TEST_CASE("PresenceTracker: identical transitions never re-fire",
          "[presence][tracker]")
{
    FakeClock clk;
    PresenceTracker t{clk.provider()};
    Recorder rec;
    t.on_state_change = rec.sink();

    t.notify_sync_started();
    // Several "becomes active" events while already Online — no extra emits.
    t.notify_window_active(true);
    t.notify_window_active(true);
    t.notify_input();
    REQUIRE(rec.seen == std::vector<State>{State::Online});
}

TEST_CASE("PresenceTracker: input during inactive window does not decay",
          "[presence][tracker]")
{
    // Regression: keyboard input through an accessibility tool while the
    // window doesn't have focus should still keep us Online — we don't
    // want to lie to other users about availability.
    FakeClock clk;
    PresenceTracker t{clk.provider()};
    Recorder rec;
    t.on_state_change = rec.sink();

    t.notify_sync_started();
    t.notify_window_active(false);
    // Keep tapping every minute, just below the threshold.
    for (int i = 0; i < 10; ++i)
    {
        clk.advance(1min);
        t.notify_input();
        t.notify_tick();
    }
    REQUIRE(t.current() == State::Online);
}
