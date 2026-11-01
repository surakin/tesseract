#include "app/SendPipeline.h"
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include <functional>
#include <string>
#include <utility>
#include <vector>

using tesseract::SendPipeline;

namespace
{

// Drives a SendPipeline by hand: prepare / send tasks and delayed UI
// callbacks are captured and run explicitly; post_ui runs inline (the tests
// are the UI thread).
struct Harness
{
    std::vector<std::function<void()>> prepares;
    std::vector<std::function<void()>> sends;
    std::vector<std::function<void()>> timers;
    std::vector<std::pair<std::string, bool>> busy_events;
    std::vector<std::string> sent; // "<room>:<label>:<prepared>"

    SendPipeline pipeline{SendPipeline::Hooks{
        [this](std::function<void()> fn)
        {
            prepares.push_back(std::move(fn));
        },
        [this](std::function<void()> fn)
        {
            sends.push_back(std::move(fn));
        },
        [](std::function<void()> fn)
        {
            fn();
        },
        [this](int ms, std::function<void()> fn)
        {
            REQUIRE(ms == SendPipeline::kBusyDelayMs);
            timers.push_back(std::move(fn));
        },
        [this](const std::string& room, bool busy)
        {
            busy_events.emplace_back(room, busy);
        },
    }};

    SendPipeline::Send recorder(const std::string& room, const std::string& label)
    {
        return [this, room, label](const std::string& prepared)
        {
            sent.push_back(room + ":" + label + ":" + prepared);
        };
    }

    // Run the FIFO `&mut` worker until it is empty (a send's completion may
    // not enqueue more, but be robust anyway).
    void drain_sends()
    {
        while (!sends.empty())
        {
            auto fn = std::move(sends.front());
            sends.erase(sends.begin());
            fn();
        }
    }

    void fire_timers()
    {
        auto t = std::move(timers);
        timers.clear();
        for (auto& fn : t)
        {
            fn();
        }
    }
};

} // namespace

TEST_CASE("SendPipeline sends without a prepare step go straight to the worker", "[send_pipeline]")
{
    Harness h;
    h.pipeline.submit("!a", nullptr, h.recorder("!a", "one"));
    REQUIRE(h.prepares.empty());
    REQUIRE(h.sends.size() == 1);
    REQUIRE(h.pipeline.outstanding("!a") == 1);
    h.drain_sends();
    REQUIRE(h.sent == std::vector<std::string>{"!a:one:"});
    REQUIRE(h.pipeline.outstanding("!a") == 0);
}

TEST_CASE("SendPipeline counts a throwing send as done and keeps the room moving",
          "[send_pipeline]")
{
    Harness h;
    h.pipeline.submit("!a", nullptr,
                      [](const std::string&)
                      {
                          throw std::runtime_error("boom");
                      });
    h.pipeline.submit("!a", nullptr, h.recorder("!a", "two"));
    REQUIRE(h.sends.size() == 2);
    h.drain_sends();
    // The throw did not stall the queue: the next message still went out, and
    // nothing is left outstanding.
    REQUIRE(h.sent == std::vector<std::string>{"!a:two:"});
    REQUIRE(h.pipeline.outstanding("!a") == 0);
}

TEST_CASE("SendPipeline passes the prepare result to the send", "[send_pipeline]")
{
    Harness h;
    h.pipeline.submit(
        "!a",
        []
        {
            return std::string("PREVIEWS");
        },
        h.recorder("!a", "one"));
    REQUIRE(h.sends.empty());
    REQUIRE(h.prepares.size() == 1);
    h.prepares[0]();
    REQUIRE(h.sends.size() == 1);
    h.drain_sends();
    REQUIRE(h.sent == std::vector<std::string>{"!a:one:PREVIEWS"});
}

TEST_CASE("SendPipeline keeps per-room order behind a slow prepare", "[send_pipeline]")
{
    Harness h;
    h.pipeline.submit(
        "!a",
        []
        {
            return std::string("P1");
        },
        h.recorder("!a", "first"));
    h.pipeline.submit("!a", nullptr, h.recorder("!a", "second"));
    h.pipeline.submit(
        "!a",
        []
        {
            return std::string("P3");
        },
        h.recorder("!a", "third"));

    // Nothing may go out while the head is still preparing.
    REQUIRE(h.sends.empty());

    // The third prepare finishing first still doesn't overtake.
    h.prepares[1]();
    REQUIRE(h.sends.empty());

    h.prepares[0]();
    REQUIRE(h.sends.size() == 3);
    h.drain_sends();
    REQUIRE(h.sent == std::vector<std::string>{"!a:first:P1", "!a:second:", "!a:third:P3"});
}

TEST_CASE("SendPipeline rooms do not wait for each other", "[send_pipeline]")
{
    Harness h;
    h.pipeline.submit(
        "!a",
        []
        {
            return std::string("P");
        },
        h.recorder("!a", "slow"));
    h.pipeline.submit("!b", nullptr, h.recorder("!b", "fast"));
    REQUIRE(h.sends.size() == 1);
    h.drain_sends();
    REQUIRE(h.sent == std::vector<std::string>{"!b:fast:"});
    h.prepares[0]();
    h.drain_sends();
    REQUIRE(h.sent == std::vector<std::string>{"!b:fast:", "!a:slow:P"});
}

TEST_CASE("SendPipeline reports busy only after the delay, idle when done", "[send_pipeline]")
{
    Harness h;
    h.pipeline.submit(
        "!a",
        []
        {
            return std::string();
        },
        h.recorder("!a", "one"));
    REQUIRE(h.timers.size() == 1);
    REQUIRE_FALSE(h.pipeline.is_busy("!a"));
    REQUIRE(h.busy_events.empty());

    // A second send in the same burst doesn't arm another timer.
    h.pipeline.submit("!a", nullptr, h.recorder("!a", "two"));
    REQUIRE(h.timers.size() == 1);

    h.fire_timers();
    REQUIRE(h.pipeline.is_busy("!a"));
    REQUIRE(h.busy_events == std::vector<std::pair<std::string, bool>>{{"!a", true}});

    h.prepares[0]();
    h.drain_sends();
    REQUIRE_FALSE(h.pipeline.is_busy("!a"));
    REQUIRE(h.busy_events ==
            std::vector<std::pair<std::string, bool>>{{"!a", true}, {"!a", false}});
}

TEST_CASE("SendPipeline fast sends never report busy", "[send_pipeline]")
{
    Harness h;
    h.pipeline.submit("!a", nullptr, h.recorder("!a", "one"));
    h.drain_sends();
    h.fire_timers();
    REQUIRE_FALSE(h.pipeline.is_busy("!a"));
    REQUIRE(h.busy_events.empty());
}

TEST_CASE("SendPipeline stale timer from an earlier burst is ignored", "[send_pipeline]")
{
    Harness h;
    // Burst 1 finishes before its timer fires.
    h.pipeline.submit("!a", nullptr, h.recorder("!a", "one"));
    h.drain_sends();
    auto stale = std::move(h.timers);
    h.timers.clear();

    // Burst 2 starts and is still in flight when burst 1's timer fires.
    h.pipeline.submit(
        "!a",
        []
        {
            return std::string();
        },
        h.recorder("!a", "two"));
    for (auto& fn : stale)
    {
        fn();
    }
    REQUIRE_FALSE(h.pipeline.is_busy("!a"));
    REQUIRE(h.busy_events.empty());

    // Burst 2's own timer marks it busy.
    h.fire_timers();
    REQUIRE(h.pipeline.is_busy("!a"));
}
