#include <catch2/catch_test_macros.hpp>

#include "views/html_spans.h"

using tesseract::views::autolink_plain_to_spans;
using tesseract::views::html_to_spans;

namespace
{
// Find the span whose text matches `needle` (or the first mention span).
const tk::TextSpan* mention_span(const std::vector<tk::TextSpan>& spans)
{
    for (const auto& s : spans)
        if (s.is_mention)
            return &s;
    return nullptr;
}
} // namespace

// autolink_plain_to_spans turns bare http(s):// URLs in a plain-text body
// into hyperlink spans (url set) so MessageListView can hit-test and fire
// on_link_clicked. Returning {} signals "no link — use the cheap path".

TEST_CASE("autolink: no URL returns empty (cheap-path signal)",
          "[html_spans][autolink]")
{
    CHECK(autolink_plain_to_spans("just some text, no link").empty());
    CHECK(autolink_plain_to_spans("").empty());
    CHECK(autolink_plain_to_spans("ftp://example.org not http").empty());
}

TEST_CASE("autolink: a bare URL becomes a single hyperlink span",
          "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans("https://example.org/path");
    REQUIRE(s.size() == 1);
    CHECK(s[0].text == "https://example.org/path");
    CHECK(s[0].url == "https://example.org/path");
}

TEST_CASE("autolink: text around a URL splits into plain + link + plain",
          "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans("see https://example.org now");
    REQUIRE(s.size() == 3);
    CHECK(s[0].text == "see ");
    CHECK(s[0].url.empty());
    CHECK(s[1].text == "https://example.org");
    CHECK(s[1].url == "https://example.org");
    CHECK(s[2].text == " now");
    CHECK(s[2].url.empty());
}

TEST_CASE("autolink: trailing prose punctuation stays out of the link",
          "[html_spans][autolink]")
{
    // The stripped '.' must remain as visible (non-link) trailing text.
    auto s = autolink_plain_to_spans("look at https://example.org/x.");
    REQUIRE(s.size() == 3);
    CHECK(s[0].text == "look at ");
    CHECK(s[0].url.empty());
    CHECK(s[1].text == "https://example.org/x");
    CHECK(s[1].url == "https://example.org/x");
    CHECK(s[2].text == ".");
    CHECK(s[2].url.empty());

    auto s2 = autolink_plain_to_spans("(https://example.org)");
    REQUIRE(s2.size() == 3);
    CHECK(s2[0].text == "(");
    CHECK(s2[1].text == "https://example.org");
    CHECK(s2[1].url == "https://example.org");
    CHECK(s2[2].text == ")");
}

TEST_CASE("autolink: word-boundary guard rejects mid-token schemes",
          "[html_spans][autolink]")
{
    // "https://" embedded inside a larger token must not be linked.
    CHECK(autolink_plain_to_spans("ahttps://example.org").empty());
    CHECK(autolink_plain_to_spans("x123https://example.org").empty());
}

TEST_CASE("autolink: http:// works and a scheme with no host does not",
          "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans("http://a.b");
    REQUIRE(s.size() == 1);
    CHECK(s[0].url == "http://a.b");

    CHECK(autolink_plain_to_spans("http://").empty());
    CHECK(autolink_plain_to_spans("https:// not a link").empty());
}

TEST_CASE("autolink: multiple URLs each become their own span",
          "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans("https://a.org and https://b.org");
    REQUIRE(s.size() == 3);
    CHECK(s[0].url == "https://a.org");
    CHECK(s[1].text == " and ");
    CHECK(s[1].url.empty());
    CHECK(s[2].url == "https://b.org");
}

// --- mention pills ------------------------------------------------------

TEST_CASE("mention: matrix.to user link is flagged as a mention pill",
          "[html_spans][mention]")
{
    auto s = html_to_spans(
        "hi <a href=\"https://matrix.to/#/@alice:example.org\">Alice</a>",
        false);
    const tk::TextSpan* m = mention_span(s);
    REQUIRE(m != nullptr);
    // The display name is present (a leading NBSP avatar-slot pad may prefix it).
    CHECK(m->text.find("Alice") != std::string::npos);
    CHECK(m->is_mention);
    CHECK(m->has_background);
    CHECK(m->has_color);
    // The url is retained for hit-testing.
    CHECK(m->url == "https://matrix.to/#/@alice:example.org");
}

TEST_CASE("mention: @room sentinel link is a mention pill",
          "[html_spans][mention]")
{
    auto s = html_to_spans(
        "<a href=\"https://matrix.to/#/@room\">@room</a>", false);
    const tk::TextSpan* m = mention_span(s);
    REQUIRE(m != nullptr);
    CHECK(m->is_mention);
}

TEST_CASE("mention: room/event/alias permalinks are NOT mentions",
          "[html_spans][mention]")
{
    // Room id (!), alias (#) and event ($) links must stay plain links.
    auto room = html_to_spans(
        "<a href=\"https://matrix.to/#/!abc:example.org\">room</a>", false);
    CHECK(mention_span(room) == nullptr);

    auto alias = html_to_spans(
        "<a href=\"https://matrix.to/#/#general:example.org\">#general</a>",
        false);
    CHECK(mention_span(alias) == nullptr);

    auto ev = html_to_spans(
        "<a href=\"https://matrix.to/#/!r:e.org/$evt\">link</a>", false);
    CHECK(mention_span(ev) == nullptr);
}

TEST_CASE("mention: ordinary http links are NOT mentions",
          "[html_spans][mention]")
{
    auto s = html_to_spans(
        "see <a href=\"https://example.org/x\">this</a>", false);
    const tk::TextSpan* m = mention_span(s);
    CHECK(m == nullptr);
    // But it's still a link span.
    bool has_link = false;
    for (const auto& sp : s)
        if (sp.url == "https://example.org/x")
            has_link = true;
    CHECK(has_link);
}

TEST_CASE("mention: literal @room becomes a mention pill", "[html_spans][mention]")
{
    auto s = html_to_spans("heads up @room please", false);
    const tk::TextSpan* m = mention_span(s);
    REQUIRE(m != nullptr);
    CHECK(m->text == "@room");
    CHECK(m->is_mention);
    CHECK(m->has_background);
    // Surrounding text is preserved as separate, non-mention spans.
    std::string joined;
    for (const auto& sp : s)
        joined += sp.text;
    CHECK(joined == "heads up @room please");
}

TEST_CASE("mention: @room inside a word is NOT a pill", "[html_spans][mention]")
{
    auto s = html_to_spans("my @roommate is here", false);
    CHECK(mention_span(s) == nullptr);
}

TEST_CASE("mention: dark theme uses different pill colours than light",
          "[html_spans][mention]")
{
    const char* html =
        "<a href=\"https://matrix.to/#/@a:b.org\">A</a>";
    auto light = html_to_spans(html, false);
    auto dark = html_to_spans(html, true);
    const tk::TextSpan* lm = mention_span(light);
    const tk::TextSpan* dm = mention_span(dark);
    REQUIRE(lm != nullptr);
    REQUIRE(dm != nullptr);
    bool differ = lm->background.r != dm->background.r ||
                  lm->background.g != dm->background.g ||
                  lm->background.b != dm->background.b;
    CHECK(differ);
}
