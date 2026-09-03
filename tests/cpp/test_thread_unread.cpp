#include <catch2/catch_test_macros.hpp>

#include "views/thread_unread.h"

using tesseract::ThreadInfo;
using tesseract::views::aggregate_threads;
using tesseract::views::ThreadDot;
using tesseract::views::thread_dot_for;

TEST_CASE("thread_dot_for precedence", "[thread][unread]")
{
    CHECK(thread_dot_for(false, false) == ThreadDot::None);
    CHECK(thread_dot_for(false, true) == ThreadDot::None); // mention only matters when unread
    CHECK(thread_dot_for(true, false) == ThreadDot::Unread);
    CHECK(thread_dot_for(true, true) == ThreadDot::Mention);
}

namespace
{
ThreadInfo thread(bool unread, bool mentions_me)
{
    ThreadInfo t;
    t.unread = unread;
    t.mentions_me = mentions_me;
    return t;
}
} // namespace

TEST_CASE("aggregate_threads: nothing unread", "[thread][unread]")
{
    auto agg = aggregate_threads({thread(false, false), thread(false, true)});
    CHECK_FALSE(agg.any_unread);
    CHECK_FALSE(agg.any_mention);
}

TEST_CASE("aggregate_threads: a quiet unread", "[thread][unread]")
{
    auto agg = aggregate_threads({thread(false, false), thread(true, false)});
    CHECK(agg.any_unread);
    CHECK_FALSE(agg.any_mention);
}

TEST_CASE("aggregate_threads: an unread reply pings", "[thread][unread]")
{
    auto agg = aggregate_threads({thread(true, false), thread(true, true)});
    CHECK(agg.any_unread);
    CHECK(agg.any_mention);
}

TEST_CASE("aggregate_threads: mention flag ignored on a read thread",
          "[thread][unread]")
{
    // mentions_me true but unread false — must not promote the header dot.
    auto agg = aggregate_threads({thread(false, true)});
    CHECK_FALSE(agg.any_unread);
    CHECK_FALSE(agg.any_mention);
}

TEST_CASE("aggregate_threads: empty list", "[thread][unread]")
{
    auto agg = aggregate_threads({});
    CHECK_FALSE(agg.any_unread);
    CHECK_FALSE(agg.any_mention);
}
