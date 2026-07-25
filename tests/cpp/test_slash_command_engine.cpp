#include "views/SlashCommandEngine.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>

using tesseract::views::SlashCommandEngine;
using tesseract::CommandDescription;

namespace
{
const std::vector<CommandDescription> kNoBotCommands;

CommandDescription make_bot_command(std::string command, std::string sender,
                                     bool valid = true)
{
    CommandDescription d;
    d.command = std::move(command);
    d.sender = std::move(sender);
    d.sender_display_name = d.sender;
    d.valid = valid;
    return d;
}
}  // namespace

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
    auto results = e.lookup("m", kNoBotCommands, 8);
    REQUIRE(!results.empty());
    REQUIRE(results.front().name == "me");
}

TEST_CASE("lookup returns full list for empty prefix", "[slash][engine]")
{
    SlashCommandEngine e;
    auto results = e.lookup("", kNoBotCommands, 12);
    // Exact: /me, /shrug, /slap, /spoiler, /myroomnick, /myroomavatar,
    // /join, /leave, /invite, /gif, /selfie, /location. Update here whenever
    // a command is added.
    REQUIRE(results.size() == 12);
}

TEST_CASE("lookup returns empty for non-matching prefix", "[slash][engine]")
{
    SlashCommandEngine e;
    auto results = e.lookup("zzzz", kNoBotCommands, 8);
    REQUIRE(results.empty());
}

TEST_CASE("lookup includes a bot command with no name collision", "[slash][engine][bot]")
{
    SlashCommandEngine e;
    std::vector<CommandDescription> bots = {make_bot_command("ban", "@mod:h")};
    auto results = e.lookup("ban", bots, 8);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].name == "ban");
    REQUIRE_FALSE(results[0].is_builtin);
    REQUIRE(results[0].bot_sender_id == "@mod:h");
}

TEST_CASE("lookup drops a bot command shadowed by a built-in", "[slash][engine][bot]")
{
    SlashCommandEngine e;
    // "me" is a built-in; a bot advertising the same name must be dropped,
    // not merely ranked behind it.
    std::vector<CommandDescription> bots = {make_bot_command("me", "@bot:h")};
    auto results = e.lookup("me", bots, 8);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].is_builtin);
    REQUIRE(results[0].name == "me");
}

TEST_CASE("lookup keeps two different bots' same-named command distinct", "[slash][engine][bot]")
{
    SlashCommandEngine e;
    std::vector<CommandDescription> bots = {
        make_bot_command("ban", "@mod1:h"),
        make_bot_command("ban", "@mod2:h"),
    };
    auto results = e.lookup("ban", bots, 8);
    REQUIRE(results.size() == 2);
    REQUIRE_FALSE(results[0].is_builtin);
    REQUIRE_FALSE(results[1].is_builtin);
    REQUIRE(results[0].bot_sender_id != results[1].bot_sender_id);
}

TEST_CASE("lookup skips invalid bot command descriptions", "[slash][engine][bot]")
{
    SlashCommandEngine e;
    std::vector<CommandDescription> bots = {make_bot_command("ban", "@mod:h", /*valid=*/false)};
    auto results = e.lookup("ban", bots, 8);
    REQUIRE(results.empty());
}

TEST_CASE("lookup prefix-matches bot commands after built-ins", "[slash][engine][bot]")
{
    SlashCommandEngine e;
    std::vector<CommandDescription> bots = {make_bot_command("meow", "@bot:h")};
    auto results = e.lookup("me", bots, 8);
    // "me" (built-in, exact) first, then "meow" (bot, prefix match).
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].name == "me");
    REQUIRE(results[0].is_builtin);
    REQUIRE(results[1].name == "meow");
    REQUIRE_FALSE(results[1].is_builtin);
}
