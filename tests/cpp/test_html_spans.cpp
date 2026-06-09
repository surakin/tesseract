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

// Concatenate the text of every span — the decoded plain text of the body.
std::string joined_text(const std::vector<tk::TextSpan>& spans)
{
    std::string out;
    for (const auto& s : spans)
        out += s.text;
    return out;
}

// The UTF-8 encoding of U+FFFD REPLACEMENT CHARACTER.
constexpr const char* kReplacement = "\xEF\xBF\xBD";
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

// --- Gap 1: invalid numeric character references --------------------------
//
// Numeric character references that name a non-scalar Unicode value (lone
// surrogate, NUL, codepoint > U+10FFFF, or a disallowed C0 control) must be
// mapped to U+FFFD rather than emitted as invalid/unsafe UTF-8.

TEST_CASE("entity: lone surrogate decodes to U+FFFD", "[html_spans][entity]")
{
    // &#xD800; is the low end of the surrogate range.
    CHECK(joined_text(html_to_spans("&#xD800;", false)) == kReplacement);
    // &#xDFFF; the high end.
    CHECK(joined_text(html_to_spans("&#xDFFF;", false)) == kReplacement);
    // Decimal form of a surrogate (55296 == 0xD800).
    CHECK(joined_text(html_to_spans("&#55296;", false)) == kReplacement);
}

TEST_CASE("entity: NUL decodes to U+FFFD", "[html_spans][entity]")
{
    auto t = joined_text(html_to_spans("&#0;", false));
    CHECK(t == kReplacement);
    // It must NOT be an embedded NUL byte.
    CHECK(t.find('\0') == std::string::npos);
}

TEST_CASE("entity: out-of-range codepoint decodes to U+FFFD",
          "[html_spans][entity]")
{
    // &#x110000; is one past the highest Unicode scalar (U+10FFFF).
    CHECK(joined_text(html_to_spans("&#x110000;", false)) == kReplacement);
    // A wildly out-of-range value as well.
    CHECK(joined_text(html_to_spans("&#x7FFFFFFF;", false)) == kReplacement);
}

TEST_CASE("entity: disallowed C0 control decodes to U+FFFD",
          "[html_spans][entity]")
{
    // &#7; is BEL — a disallowed C0 control.
    CHECK(joined_text(html_to_spans("&#7;", false)) == kReplacement);
    // U+001B ESC.
    CHECK(joined_text(html_to_spans("&#x1B;", false)) == kReplacement);
}

TEST_CASE("entity: allowed whitespace controls survive intact",
          "[html_spans][entity]")
{
    // Tab, newline and carriage return are legal and must pass through.
    // (A non-letter sentinel keeps the LF/CR off the trailing edge, where
    // html_to_spans intentionally trims newlines.)
    CHECK(joined_text(html_to_spans("&#9;", false)) == "\t");
    CHECK(joined_text(html_to_spans("a&#10;b", false)) == "a\nb");
    CHECK(joined_text(html_to_spans("a&#13;b", false)) == "a\rb");
    // A normal ASCII character is unaffected.
    CHECK(joined_text(html_to_spans("&#65;", false)) == "A");
    // A valid astral codepoint (U+1F600 GRINNING FACE) is unaffected.
    CHECK(joined_text(html_to_spans("&#x1F600;", false)) ==
          "\xF0\x9F\x98\x80");
}

// --- Gap 2: formatting-stack balance under the depth cap ------------------
//
// Opening tags past kMaxTagDepth (64) are flattened rather than pushed, but
// their matching close tags used to still pop a real frame — corrupting the
// styling of text that follows the over-deep section. The over-deep input
// must yield the same trailing styling as a shallow baseline.

TEST_CASE("depth-cap: dropped opens do not pop legitimate outer frames",
          "[html_spans][depth]")
{
    // Outer <b> wraps a deeply nested run of <i> opens that overflow the
    // depth cap, then all the <i> close. Text after the nested section,
    // still inside <b>, must remain bold.
    std::string html = "<b>start ";
    constexpr int kOverflow = 200; // well past kMaxTagDepth (64)
    for (int i = 0; i < kOverflow; ++i)
        html += "<i>";
    html += "deep";
    for (int i = 0; i < kOverflow; ++i)
        html += "</i>";
    html += " tail</b>";

    auto spans = html_to_spans(html, false);

    // The text after the nested section ("tail") must still be bold.
    bool found_tail = false;
    for (const auto& s : spans)
    {
        if (s.text.find("tail") != std::string::npos)
        {
            found_tail = true;
            CHECK(s.bold); // would be false under the bug
        }
    }
    CHECK(found_tail);

    // And "start" (before the overflow) is bold too — sanity baseline.
    for (const auto& s : spans)
        if (s.text.find("start") != std::string::npos)
            CHECK(s.bold);
}
