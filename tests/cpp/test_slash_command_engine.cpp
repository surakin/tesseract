#include "views/SlashCommandEngine.h"
#include <catch2/catch_test_macros.hpp>

using tesseract::views::SlashCommandEngine;

TEST_CASE("find_prefix returns query on empty-line slash", "[slash][engine]")
{
    SlashCommandEngine e;
    auto m = e.find_prefix("/", 1);
    REQUIRE(m.has_value());
    REQUIRE(m->prefix.empty());
    REQUIRE(m->start == 0);
    REQUIRE(m->end == 1);
}

TEST_CASE("find_prefix returns query while typing", "[slash][engine]")
{
    SlashCommandEngine e;
    auto m = e.find_prefix("/me", 3);
    REQUIRE(m.has_value());
    REQUIRE(m->prefix == "me");
}

TEST_CASE("find_prefix stops at space (args entered)", "[slash][engine]")
{
    SlashCommandEngine e;
    auto m = e.find_prefix("/me hello", 9);
    REQUIRE(!m.has_value());
}

TEST_CASE("find_prefix rejects mid-message slash", "[slash][engine]")
{
    SlashCommandEngine e;
    auto m = e.find_prefix("hi /me", 6);
    REQUIRE(!m.has_value());
}

TEST_CASE("find_prefix rejects non-letter chars after slash", "[slash][engine]")
{
    SlashCommandEngine e;
    REQUIRE(!e.find_prefix("/9", 2).has_value());
    REQUIRE(!e.find_prefix("/!", 2).has_value());
}

TEST_CASE("lookup ranks exact then prefix matches", "[slash][engine]")
{
    SlashCommandEngine e;
    auto results = e.lookup("m", 8);
    REQUIRE(!results.empty());
    REQUIRE(results.front().name == "me");
}

TEST_CASE("lookup returns full list for empty prefix", "[slash][engine]")
{
    SlashCommandEngine e;
    auto results = e.lookup("", 8);
    // Exact: today the registry holds /me, /shrug, /slap, /spoiler,
    // /myroomnick and /myroomavatar. Update here whenever a command is added.
    REQUIRE(results.size() == 6);
}

TEST_CASE("lookup returns empty for non-matching prefix", "[slash][engine]")
{
    SlashCommandEngine e;
    auto results = e.lookup("zzzz", 8);
    REQUIRE(results.empty());
}
