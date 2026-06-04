#include <catch2/catch_test_macros.hpp>

#include "views/GifEngine.h"

using tesseract::views::GifEngine;

TEST_CASE("GifEngine matches /gif <query>", "[view][gif][engine]")
{
    GifEngine e;

    SECTION("plain query")
    {
        auto m = e.match("/gif cat");
        REQUIRE(m.has_value());
        CHECK(*m == "cat");
    }
    SECTION("multi-word query, trimmed")
    {
        auto m = e.match("/gif  funny cat  ");
        REQUIRE(m.has_value());
        CHECK(*m == "funny cat");
    }
    SECTION("single char (search-as-you-type)")
    {
        auto m = e.match("/gif c");
        REQUIRE(m.has_value());
        CHECK(*m == "c");
    }
}

TEST_CASE("GifEngine rejects non-commands", "[view][gif][engine]")
{
    GifEngine e;
    CHECK_FALSE(e.match("").has_value());
    CHECK_FALSE(e.match("hello").has_value());
    CHECK_FALSE(e.match("/gif").has_value());      // no space, no query
    CHECK_FALSE(e.match("/gif ").has_value());     // prefix only, empty query
    CHECK_FALSE(e.match("/gif   ").has_value());   // whitespace-only query
    CHECK_FALSE(e.match("a /gif cat").has_value()); // not at composer start
    CHECK_FALSE(e.match("/giftcat").has_value());   // different command
}
