#include <catch2/catch_test_macros.hpp>

#include "app/PowerPolicy.h"

#include <chrono>
#include <vector>

using tesseract::PowerPolicy;
using Pref = PowerPolicy::Pref;
using namespace std::chrono_literals;

namespace
{

struct PolicyClock
{
    PowerPolicy::Clock::time_point now =
        PowerPolicy::Clock::time_point{} + 1h;

    PowerPolicy::NowProvider provider()
    {
        return [this] { return now; };
    }

    void advance(std::chrono::seconds s) { now += s; }
};

struct PolicyRecorder
{
    std::vector<bool> seen;

    std::function<void(bool)> sink()
    {
        return [this](bool a) { seen.push_back(a); };
    }
};

} // namespace

TEST_CASE("PowerPolicy: Off is never active for any signal combination",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();
    p.set_pref(Pref::Off);

    p.notify_os_power_saver(true);
    p.notify_on_battery(true);
    clk.advance(1h);
    p.notify_tick();

    REQUIRE_FALSE(p.active());
    REQUIRE(rec.seen.empty());
    REQUIRE_FALSE(p.has_pending());
}

TEST_CASE("PowerPolicy: On is active immediately and ignores signals",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();

    p.set_pref(Pref::On);
    REQUIRE(p.active());
    REQUIRE(rec.seen == std::vector<bool>{true});

    p.notify_os_power_saver(false);
    p.notify_on_battery(false);
    p.notify_tick();
    REQUIRE(p.active());
    REQUIRE(rec.seen == std::vector<bool>{true});
    REQUIRE_FALSE(p.has_pending());
}

TEST_CASE("PowerPolicy: Auto activates on power-saver only after the debounce",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();
    p.set_pref(Pref::Auto);

    p.notify_os_power_saver(true);
    REQUIRE_FALSE(p.active());
    REQUIRE(p.has_pending());
    REQUIRE(rec.seen.empty());

    clk.advance(p.debounce());
    p.notify_tick();
    REQUIRE(p.active());
    REQUIRE(rec.seen == std::vector<bool>{true});
    REQUIRE_FALSE(p.has_pending());
}

TEST_CASE("PowerPolicy: Auto signal that reverts before settling never emits",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();
    p.set_pref(Pref::Auto);

    p.notify_on_battery(true);
    REQUIRE(p.has_pending());
    clk.advance(10s);
    p.notify_on_battery(false);

    REQUIRE_FALSE(p.has_pending());
    clk.advance(1h);
    p.notify_tick();
    REQUIRE_FALSE(p.active());
    REQUIRE(rec.seen.empty());
}

TEST_CASE("PowerPolicy: Auto deactivates after both signals clear and settle",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();
    p.set_pref(Pref::Auto);

    p.notify_os_power_saver(true);
    clk.advance(p.debounce());
    p.notify_tick();
    REQUIRE(p.active());

    p.notify_os_power_saver(false);
    p.notify_on_battery(false);
    REQUIRE(p.has_pending());
    clk.advance(p.debounce());
    p.notify_tick();

    REQUIRE_FALSE(p.active());
    REQUIRE(rec.seen == std::vector<bool>{true, false});
}

TEST_CASE("PowerPolicy: Auto stays active while one signal remains set",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();
    p.set_pref(Pref::Auto);

    p.notify_on_battery(true);
    p.notify_os_power_saver(true);
    clk.advance(p.debounce());
    p.notify_tick();
    REQUIRE(p.active());
    REQUIRE(rec.seen == std::vector<bool>{true});

    // Unplugged→plugged, but the energy-saver profile is still on.
    p.notify_on_battery(false);
    REQUIRE_FALSE(p.has_pending());
    clk.advance(1h);
    p.notify_tick();
    REQUIRE(p.active());
    REQUIRE(rec.seen == std::vector<bool>{true});
}

TEST_CASE("PowerPolicy: repeated ticks with no change never re-fire",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();
    p.set_pref(Pref::On);

    for (int i = 0; i < 5; ++i)
    {
        clk.advance(1h);
        p.notify_tick();
    }
    REQUIRE(rec.seen == std::vector<bool>{true});
}

TEST_CASE("PowerPolicy: set_pref(On) supersedes a pending Auto deactivation",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();
    p.set_pref(Pref::Auto);

    p.notify_os_power_saver(true);
    clk.advance(p.debounce());
    p.notify_tick();
    REQUIRE(p.active());

    // Signal drops — a deactivation is now debouncing.
    p.notify_os_power_saver(false);
    REQUIRE(p.has_pending());

    p.set_pref(Pref::On);
    REQUIRE(p.active());
    REQUIRE_FALSE(p.has_pending());
    REQUIRE(rec.seen == std::vector<bool>{true});
}

TEST_CASE("PowerPolicy: switching back to Auto applies immediately",
          "[power][policy]")
{
    PolicyClock clk;
    PowerPolicy p{clk.provider()};
    PolicyRecorder rec;
    p.on_mode_change = rec.sink();

    p.set_pref(Pref::On);
    REQUIRE(p.active());

    // Both signals clear; Auto should resolve to inactive with no debounce.
    p.set_pref(Pref::Auto);
    REQUIRE_FALSE(p.active());
    REQUIRE_FALSE(p.has_pending());
    REQUIRE(rec.seen == std::vector<bool>{true, false});
}
