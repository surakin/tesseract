#include <catch2/catch_test_macros.hpp>

#include "tk/interval_timer.h"

#include <functional>
#include <vector>

using tk::IntervalTimer;

namespace
{

// Records scheduled callbacks so the test can drive time manually. Mimics a
// one-shot "run fn after ms" primitive (post_to_ui_after_ / post_delayed).
struct FakeScheduler
{
    struct Pending
    {
        int ms;
        std::function<void()> fn;
    };
    std::vector<Pending> queue;

    IntervalTimer::OneShot as_fn()
    {
        return [this](int ms, std::function<void()> fn)
        { queue.push_back({ms, std::move(fn)}); };
    }

    // Fire every currently-queued callback once (FIFO), leaving anything they
    // re-schedule for the next call.
    void fire_due()
    {
        std::vector<Pending> batch;
        batch.swap(queue);
        for (auto& p : batch)
        {
            p.fn();
        }
    }
};

} // namespace

TEST_CASE("fires repeatedly at the configured interval", "[interval-timer]")
{
    FakeScheduler sched;
    int ticks = 0;
    IntervalTimer t(sched.as_fn(), 2000, [&] { ++ticks; });

    t.start();
    REQUIRE(sched.queue.size() == 1);
    CHECK(sched.queue[0].ms == 2000);

    sched.fire_due();
    CHECK(ticks == 1);
    REQUIRE(sched.queue.size() == 1); // re-armed itself

    sched.fire_due();
    sched.fire_due();
    CHECK(ticks == 3);
}

TEST_CASE("start() is idempotent", "[interval-timer]")
{
    FakeScheduler sched;
    int ticks = 0;
    IntervalTimer t(sched.as_fn(), 1000, [&] { ++ticks; });

    t.start();
    t.start();
    CHECK(sched.queue.size() == 1);
}

TEST_CASE("stop() makes a pending fire a no-op", "[interval-timer]")
{
    FakeScheduler sched;
    int ticks = 0;
    IntervalTimer t(sched.as_fn(), 1000, [&] { ++ticks; });

    t.start();
    t.stop();
    sched.fire_due(); // the already-queued closure must not run on_tick
    CHECK(ticks == 0);
    CHECK(sched.queue.empty()); // and must not re-arm
}

TEST_CASE("destruction makes a pending fire a no-op", "[interval-timer]")
{
    FakeScheduler sched;
    int ticks = 0;
    {
        IntervalTimer t(sched.as_fn(), 1000, [&] { ++ticks; });
        t.start();
    }
    sched.fire_due();
    CHECK(ticks == 0);
}

TEST_CASE("on_tick may stop the timer", "[interval-timer]")
{
    FakeScheduler sched;
    int ticks = 0;
    IntervalTimer t(sched.as_fn(), 1000,
                    [&]
                    {
                        ++ticks;
                        t.stop();
                    });

    t.start();
    sched.fire_due();
    CHECK(ticks == 1);
    CHECK(sched.queue.empty()); // did not re-arm after self-stop
}

TEST_CASE("set_interval applies on the next re-arm", "[interval-timer]")
{
    FakeScheduler sched;
    IntervalTimer t(sched.as_fn(), 1000, [] {});

    t.start();
    CHECK(sched.queue[0].ms == 1000);

    t.set_interval(250);
    sched.fire_due();
    REQUIRE(sched.queue.size() == 1);
    CHECK(sched.queue[0].ms == 250);
}
