#include "app/SlashCommands.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("available_commands lists me and shrug", "[slash]")
{
    const auto& cmds = tesseract::available_commands();
    REQUIRE(cmds.size() >= 2);

    auto by_name = [&](const std::string& n) {
        for (const auto& c : cmds) if (c.name == n) return &c;
        return (const tesseract::SlashCommandDescriptor*) nullptr;
    };

    const auto* me = by_name("me");
    REQUIRE(me != nullptr);
    REQUIRE(me->args_hint == "<action>");

    const auto* shrug = by_name("shrug");
    REQUIRE(shrug != nullptr);
    REQUIRE(shrug->args_hint.empty()); // /shrug takes no args

    const auto* location = by_name("location");
    REQUIRE(location != nullptr);
    REQUIRE(location->args_hint.empty()); // /location takes no args

    const auto* slap = by_name("slap");
    REQUIRE(slap != nullptr);
    REQUIRE(slap->args_hint == "<target>");

    const auto* spoiler = by_name("spoiler");
    REQUIRE(spoiler != nullptr);
    REQUIRE(spoiler->args_hint == "[(reason)] <text>");

    const auto* invite = by_name("invite");
    REQUIRE(invite != nullptr);
    REQUIRE(invite->args_hint == "<@user:server> [reason]");
}

TEST_CASE("build_spoiler_message wraps plain content", "[slash][spoiler]")
{
    auto m = tesseract::build_spoiler_message("the dog dies");
    REQUIRE(m.has_value());
    REQUIRE(m->body == "(Spoiler) the dog dies");
    REQUIRE(m->formatted_body == "<span data-mx-spoiler>the dog dies</span>");
}

TEST_CASE("build_spoiler_message extracts a reason", "[slash][spoiler]")
{
    auto m = tesseract::build_spoiler_message("(ending) he wins");
    REQUIRE(m.has_value());
    REQUIRE(m->body == "(Spoiler: ending) he wins");
    REQUIRE(m->formatted_body ==
            "<span data-mx-spoiler=\"ending\">he wins</span>");
}

TEST_CASE("build_spoiler_message renders inline markdown", "[slash][spoiler]")
{
    auto m = tesseract::build_spoiler_message("**boom**");
    REQUIRE(m.has_value());
    REQUIRE(m->formatted_body ==
            "<span data-mx-spoiler><strong>boom</strong></span>");
}

TEST_CASE("build_spoiler_message escapes HTML in content and reason",
          "[slash][spoiler]")
{
    auto m = tesseract::build_spoiler_message("a < b & c");
    REQUIRE(m.has_value());
    REQUIRE(m->formatted_body ==
            "<span data-mx-spoiler>a &lt; b &amp; c</span>");
    // Plain body keeps the original characters verbatim.
    REQUIRE(m->body == "(Spoiler) a < b & c");

    auto r = tesseract::build_spoiler_message("(say \"hi\" & <b>) text");
    REQUIRE(r.has_value());
    REQUIRE(r->formatted_body ==
            "<span data-mx-spoiler=\"say &quot;hi&quot; &amp; &lt;b>\">text"
            "</span>");
}

TEST_CASE("build_spoiler_message no-ops on whitespace-only content",
          "[slash][spoiler]")
{
    REQUIRE(!tesseract::build_spoiler_message("   ").has_value());
    REQUIRE(!tesseract::build_spoiler_message("(reason)   ").has_value());
    REQUIRE(!tesseract::build_spoiler_message("").has_value());
}

TEST_CASE("parse_slash_args returns nullopt for a non-matching command",
          "[slash]")
{
    REQUIRE(!tesseract::parse_slash_args("/join #room:server", "invite").has_value());
    REQUIRE(!tesseract::parse_slash_args("/invite", "invite").has_value());
    REQUIRE(!tesseract::parse_slash_args("/inviteextra @a:b", "invite").has_value());
}

TEST_CASE("parse_slash_args returns an empty vector for whitespace-only args",
          "[slash]")
{
    auto args = tesseract::parse_slash_args("/invite    ", "invite");
    REQUIRE(args.has_value());
    REQUIRE(args->empty());
}

TEST_CASE("parse_slash_args splits a single argument", "[slash]")
{
    auto args = tesseract::parse_slash_args("/invite @bob:example.org", "invite");
    REQUIRE(args.has_value());
    REQUIRE(*args == std::vector<std::string>{"@bob:example.org"});
}

TEST_CASE("parse_slash_args splits multiple whitespace-delimited arguments",
          "[slash]")
{
    auto args = tesseract::parse_slash_args("/invite @bob:example.org come chat", "invite");
    REQUIRE(args.has_value());
    REQUIRE(*args == std::vector<std::string>{"@bob:example.org", "come", "chat"});
}

TEST_CASE("parse_slash_args keeps a quoted span as a single argument",
          "[slash]")
{
    auto args = tesseract::parse_slash_args(
        "/invite @bob:example.org \"come chat with us\"", "invite");
    REQUIRE(args.has_value());
    REQUIRE(*args ==
            std::vector<std::string>{"@bob:example.org", "come chat with us"});

    auto args2 = tesseract::parse_slash_args(
        "/invite @bob:example.org 'come chat with us'", "invite");
    REQUIRE(args2.has_value());
    REQUIRE(*args2 ==
            std::vector<std::string>{"@bob:example.org", "come chat with us"});
}

