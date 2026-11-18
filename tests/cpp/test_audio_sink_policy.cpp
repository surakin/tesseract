#include <catch2/catch_test_macros.hpp>

#include "tk/audio_sink_policy.h"

using namespace std::chrono_literals;

TEST_CASE("sink rebuild policy: a healthy sink is never rebuilt", "[audio]")
{
    tk::SinkRebuildPolicy p;
    const auto t = tk::SinkRebuildPolicy::Clock::now();
    CHECK_FALSE(p.should_rebuild(true, t));
    CHECK_FALSE(p.should_rebuild(true, t + 10s));
}

TEST_CASE("sink rebuild policy: a dead sink is rebuilt at once, then at most once per backoff",
          "[audio]")
{
    tk::SinkRebuildPolicy p(1000ms);
    const auto t = tk::SinkRebuildPolicy::Clock::now();
    CHECK(p.should_rebuild(false, t));
    CHECK_FALSE(p.should_rebuild(false, t + 10ms)); // the next frame
    CHECK_FALSE(p.should_rebuild(false, t + 999ms));
    CHECK(p.should_rebuild(false, t + 1000ms));
    CHECK_FALSE(p.should_rebuild(false, t + 1010ms));
}

TEST_CASE("sink rebuild policy: recovery re-arms the immediate rebuild", "[audio]")
{
    tk::SinkRebuildPolicy p(1000ms);
    const auto t = tk::SinkRebuildPolicy::Clock::now();
    CHECK(p.should_rebuild(false, t));
    CHECK_FALSE(p.should_rebuild(true, t + 20ms)); // rebuilt and healthy again
    CHECK(p.should_rebuild(false, t + 30ms));      // dies again: no waiting out the old backoff
}
