#include <catch2/catch_test_macros.hpp>

#include "views/timeline_column.h"

using tesseract::Settings;
using tesseract::views::timeline_column;
using A = Settings::TimelineColumnAnchor;

TEST_CASE("timeline column: 50% of a wide timeline, left and centred", "[timeline_column]")
{
    const auto l = timeline_column(1600, 50, A::Left);
    CHECK(l.x == 0.0f);
    CHECK(l.w == 800.0f);
    const auto c = timeline_column(1600, 50, A::Center);
    CHECK(c.x == 400.0f);
    CHECK(c.w == 800.0f);
}

TEST_CASE("timeline column: the 468px floor wins over a small percentage", "[timeline_column]")
{
    const auto c = timeline_column(1000, 30, A::Left);
    CHECK(c.w == 468.0f);
}

TEST_CASE("timeline column: a timeline narrower than the floor is used whole", "[timeline_column]")
{
    const auto c = timeline_column(300, 30, A::Center);
    CHECK(c.x == 0.0f);
    CHECK(c.w == 300.0f);
}

TEST_CASE("timeline column: 100% is the identity for any anchor", "[timeline_column]")
{
    for (A a : {A::Left, A::Center})
    {
        const auto c = timeline_column(1234, 100, a);
        CHECK(c.x == 0.0f);
        CHECK(c.w == 1234.0f);
    }
}

TEST_CASE("timeline column: zero or negative width gives an empty column", "[timeline_column]")
{
    for (float w : {0.0f, -5.0f})
    {
        const auto c = timeline_column(w, 50, A::Center);
        CHECK(c.x == 0.0f);
        CHECK(c.w == 0.0f);
    }
}

TEST_CASE("timeline column: out-of-range percentages are clamped to 30..100", "[timeline_column]")
{
    CHECK(timeline_column(2000, 10, A::Left).w == 600.0f);
    CHECK(timeline_column(2000, 500, A::Left).w == 2000.0f);
}
