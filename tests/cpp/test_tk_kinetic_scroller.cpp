#include <catch2/catch_test_macros.hpp>

#include "tk/kinetic_scroller.h"

#include <chrono>
#include <thread>

using tk::KineticScroller;

namespace
{

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Feeds a burst of same-direction touchpad samples a few ms apart, mimicking
// a real two-finger scroll gesture, then waits past the idle-gap threshold
// so the next step() resolves the gesture into a fling (or not).
void feed_touchpad_flick(KineticScroller& k, float dy_per_sample, int count)
{
    for (int i = 0; i < count; ++i)
    {
        k.on_wheel_delta(dy_per_sample, /*is_touchpad=*/true);
        sleep_ms(8);
    }
}

} // namespace

TEST_CASE("KineticScroller arms a fling after a fast touchpad flick", "[tk][kinetic_scroller]")
{
    KineticScroller k;
    feed_touchpad_flick(k, 20.0f, 6); // ~2.5 px/ms average — well over the arm threshold
    REQUIRE(k.active());
    REQUIRE_FALSE(k.is_flinging()); // still watching until the idle gap elapses

    sleep_ms(150); // past kIdleGapMs
    float delta = k.step();
    REQUIRE(k.is_flinging());
    // The resolving step decays for the idle gap itself and returns that
    // delta immediately — no extra dead frame before motion starts (a
    // visible pause here is exactly the bug this test guards against).
    REQUIRE(delta > 0.0f); // same sign as the flick direction

    sleep_ms(16);
    float coast = k.step();
    REQUIRE(k.is_flinging());
    REQUIRE(coast > 0.0f); // same sign as the flick direction
}

TEST_CASE("KineticScroller does not arm a fling after a slow, deliberate scroll", "[tk][kinetic_scroller]")
{
    KineticScroller k;
    // Small deltas spaced far enough apart to average out well under the
    // minimum fling velocity.
    k.on_wheel_delta(1.0f, true);
    sleep_ms(60);
    k.on_wheel_delta(1.0f, true);
    sleep_ms(60);
    k.on_wheel_delta(1.0f, true);

    REQUIRE(k.active());
    sleep_ms(150);
    k.step();
    REQUIRE_FALSE(k.is_flinging());
    REQUIRE_FALSE(k.active());
}

TEST_CASE("KineticScroller ignores physical mouse-wheel notches", "[tk][kinetic_scroller]")
{
    KineticScroller k;
    k.on_wheel_delta(90.0f, /*is_touchpad=*/false);
    REQUIRE_FALSE(k.active());
    REQUIRE_FALSE(k.is_flinging());

    sleep_ms(150);
    REQUIRE(k.step() == 0.0f);
    REQUIRE_FALSE(k.is_flinging());
}

TEST_CASE("KineticScroller cancel() stops an in-flight fling immediately", "[tk][kinetic_scroller]")
{
    KineticScroller k;
    feed_touchpad_flick(k, 20.0f, 6);
    sleep_ms(150);
    k.step();
    REQUIRE(k.is_flinging());

    k.cancel();
    REQUIRE_FALSE(k.is_flinging());
    REQUIRE_FALSE(k.active());
    REQUIRE(k.step() == 0.0f);
}

TEST_CASE("KineticScroller: a new sample cancels an in-flight fling instead of stacking", "[tk][kinetic_scroller]")
{
    KineticScroller k;
    feed_touchpad_flick(k, 20.0f, 6);
    sleep_ms(150);
    k.step();
    REQUIRE(k.is_flinging());

    // User grabs the content again mid-coast.
    k.on_wheel_delta(-5.0f, true);
    REQUIRE_FALSE(k.is_flinging());
    REQUIRE(k.active()); // now watching the new gesture instead
}
