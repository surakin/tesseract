#include "views/SlashCommands.h"
#include <tesseract/client.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("available_commands lists me and shrug", "[slash]")
{
    const auto& cmds = tesseract::available_commands();
    REQUIRE(cmds.size() >= 2);

    auto by_name = [&](const std::string& n)
    {
        for (const auto& c : cmds)
        {
            if (c.name == n)
            {
                return &c;
            }
        }
        return static_cast<const tesseract::SlashCommandDescriptor*>(nullptr);
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
    REQUIRE(m->formatted_body == "<span data-mx-spoiler=\"ending\">he wins</span>");
}

TEST_CASE("build_spoiler_message renders inline markdown", "[slash][spoiler]")
{
    auto m = tesseract::build_spoiler_message("**boom**");
    REQUIRE(m.has_value());
    REQUIRE(m->formatted_body == "<span data-mx-spoiler><strong>boom</strong></span>");
}

TEST_CASE("build_spoiler_message escapes HTML in content and reason", "[slash][spoiler]")
{
    auto m = tesseract::build_spoiler_message("a < b & c");
    REQUIRE(m.has_value());
    REQUIRE(m->formatted_body == "<span data-mx-spoiler>a &lt; b &amp; c</span>");
    // Plain body keeps the original characters verbatim.
    REQUIRE(m->body == "(Spoiler) a < b & c");

    auto r = tesseract::build_spoiler_message("(say \"hi\" & <b>) text");
    REQUIRE(r.has_value());
    REQUIRE(r->formatted_body == "<span data-mx-spoiler=\"say &quot;hi&quot; &amp; &lt;b>\">text"
                                 "</span>");
}

TEST_CASE("build_spoiler_message no-ops on whitespace-only content", "[slash][spoiler]")
{
    REQUIRE(!tesseract::build_spoiler_message("   ").has_value());
    REQUIRE(!tesseract::build_spoiler_message("(reason)   ").has_value());
    REQUIRE(!tesseract::build_spoiler_message("").has_value());
}

namespace
{
std::optional<tesseract::SlashCommandInvocation> parse(const std::string& text)
{
    return tesseract::parse_slash_command(text, text, {});
}
} // namespace

TEST_CASE("parse_slash_command only matches whole built-in names", "[slash]")
{
    REQUIRE(!parse("/inviteextra @a:b").has_value());
    REQUIRE(!parse("/menu").has_value());
    REQUIRE(!parse("/nosuchcommand x").has_value());
    REQUIRE(!parse("hello").has_value());
    REQUIRE(!parse("/").has_value());
    REQUIRE(parse("/invite").has_value());
    REQUIRE(parse("/me waves").has_value());
}

TEST_CASE("parse_slash_command gives empty args for whitespace-only args", "[slash]")
{
    auto inv = parse("/invite    ");
    REQUIRE(inv.has_value());
    REQUIRE(inv->name == "invite");
    REQUIRE(inv->args_text.empty());
    REQUIRE(inv->args.empty());
}

TEST_CASE("parse_slash_command splits a single argument", "[slash]")
{
    auto inv = parse("/invite @bob:example.org");
    REQUIRE(inv.has_value());
    REQUIRE(inv->args == std::vector<std::string>{"@bob:example.org"});
}

TEST_CASE("parse_slash_command splits multiple whitespace-delimited arguments", "[slash]")
{
    auto inv = parse("/invite @bob:example.org come chat");
    REQUIRE(inv.has_value());
    REQUIRE(inv->args == std::vector<std::string>{"@bob:example.org", "come", "chat"});
    REQUIRE(inv->args_text == "@bob:example.org come chat");
}

TEST_CASE("parse_slash_command keeps a quoted span as a single argument", "[slash]")
{
    auto inv = parse("/invite @bob:example.org \"come chat with us\"");
    REQUIRE(inv.has_value());
    REQUIRE(inv->args == std::vector<std::string>{"@bob:example.org", "come chat with us"});

    auto inv2 = parse("/invite @bob:example.org 'come chat with us'");
    REQUIRE(inv2.has_value());
    REQUIRE(inv2->args == std::vector<std::string>{"@bob:example.org", "come chat with us"});
}

TEST_CASE("parse_slash_command reads args from the plain text, not the body", "[slash]")
{
    // The body carries a mention pill's display name; the plain text its ID.
    auto inv = tesseract::parse_slash_command("/dm @alice:example.org", "/dm Alice", "<p>x</p>");
    REQUIRE(inv.has_value());
    REQUIRE(inv->args == std::vector<std::string>{"@alice:example.org"});
    REQUIRE(inv->body == "/dm Alice");
    REQUIRE(inv->formatted_body == "<p>x</p>");
}

TEST_CASE("every built-in is either a send command or shell-handled", "[slash]")
{
    // Commands without a `send` handler are run by ShellBase's slash UI handlers
    // (or are popup-only, like /gif and /sticker); find_command must see them all.
    for (const auto& c : tesseract::available_commands())
    {
        const auto* found = tesseract::find_command(c.name);
        REQUIRE(found != nullptr);
        REQUIRE(found->name == c.name);
    }
    REQUIRE(tesseract::find_command("nosuch") == nullptr);
}

TEST_CASE("the myroomavatar command routes by argument scheme", "[slash][myroomavatar]")
{
    tesseract::Client client; // not logged in: SDK calls fail with "not logged in"

    SECTION("a bare word is rejected before reaching the client")
    {
        auto r = tesseract::dispatch_compose_send(client, "!r:x", "/myroomavatar nonsense", "");
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.message == "expected an mxc:// or http(s) image URL, or \"reset\"");
    }
    SECTION("no argument is an error")
    {
        auto r = tesseract::dispatch_compose_send(client, "!r:x", "/myroomavatar   ", "");
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.message.find("no image URL provided") == 0);
    }
    SECTION("http(s) URLs (any case) reach the URL path")
    {
        for (const char* body : {"/myroomavatar https://example.com/a.png",
                                 "/myroomavatar HTTP://example.com/a.gif  "})
        {
            auto r = tesseract::dispatch_compose_send(client, "!r:x", body, "");
            REQUIRE_FALSE(r.ok);
            REQUIRE(r.message == "not logged in");
        }
    }
    SECTION("mxc URIs still reach the mxc path")
    {
        auto r =
            tesseract::dispatch_compose_send(client, "!r:x", "/myroomavatar mxc://srv/abc", "");
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.message == "not logged in");
    }
}

TEST_CASE("the myroomavatar command reset reaches the client; other args keep their routes",
          "[slash][myroomavatar]")
{
    tesseract::Client client; // not logged in: SDK calls fail with "not logged in"
    for (const char* body : {"/myroomavatar reset", "/myroomavatar RESET  ", "/myroomavatar Reset"})
    {
        auto r = tesseract::dispatch_compose_send(client, "!r:x", body, "");
        REQUIRE_FALSE(r.ok);
        CHECK(r.message == "not logged in"); // the reset path, not "expected an mxc..."
    }
    // "reset" must be the whole argument.
    auto r = tesseract::dispatch_compose_send(client, "!r:x", "/myroomavatar resets", "");
    REQUIRE_FALSE(r.ok);
    CHECK(r.message.find("expected an mxc://") == 0);
}

TEST_CASE("slash_success_message confirms only /myroomavatar", "[slash][myroomavatar]")
{
    CHECK(tesseract::slash_success_message("/myroomavatar https://x/a.png") ==
          "Avatar updated for this room");
    CHECK(tesseract::slash_success_message("/myroomavatar mxc://x/y") ==
          "Avatar updated for this room");
    CHECK(tesseract::slash_success_message("/myroomavatar reset") == "Avatar reset for this room");
    CHECK(tesseract::slash_success_message("/myroomavatar  RESET") == "Avatar reset for this room");
    CHECK_FALSE(tesseract::slash_success_message("/myroomavatar").has_value());
    CHECK_FALSE(tesseract::slash_success_message("/me waves").has_value());
    CHECK_FALSE(tesseract::slash_success_message("hello").has_value());
}

TEST_CASE("strip_command_from_html keeps formatting and mention pills", "[slash]")
{
    using tesseract::strip_command_from_html;
    CHECK(strip_command_from_html("<p>/me <em>waves</em></p>", "me") == "<p><em>waves</em></p>");
    CHECK(strip_command_from_html("/me <a href=\"https://matrix.to/#/@a:b\">A</a>", "me") ==
          "<a href=\"https://matrix.to/#/@a:b\">A</a>");
    CHECK(strip_command_from_html("<p>/shrug hi</p>", "shrug") == "<p>hi</p>");
    CHECK(strip_command_from_html("<p>hello</p>", "me").empty());
    CHECK(strip_command_from_html("", "me").empty());
}

TEST_CASE("append_to_html adds text inside the last paragraph", "[slash]")
{
    using tesseract::append_to_html;
    CHECK(append_to_html("<p>hi</p>", " x") == "<p>hi x</p>");
    CHECK(append_to_html("hi", " x") == "hi x");
}
