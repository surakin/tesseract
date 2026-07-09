#include <catch2/catch_test_macros.hpp>

#include "views/html_spans.h"

using tesseract::views::autolink_plain_to_spans;
using tesseract::views::BodyBlock;
using tesseract::views::html_to_blocks;
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

// True when any span carries a (clickable) url — i.e. the body was linkified.
bool any_link(const std::vector<tk::TextSpan>& spans)
{
    for (const auto& s : spans)
        if (!s.url.empty())
            return true;
    return false;
}

// Concatenate the text of every span across every block — the decoded plain
// text of a whole html_to_blocks() result.
std::string joined_block_text(const std::vector<BodyBlock>& blocks)
{
    std::string out;
    for (const auto& b : blocks)
        out += joined_text(b.spans);
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
    auto s = autolink_plain_to_spans("http://example.org");
    REQUIRE(s.size() == 1);
    CHECK(s[0].url == "http://example.org");

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

// --- matrix: URIs -------------------------------------------------------

TEST_CASE("autolink: bare matrix:u/ URI is linkified", "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans("join matrix:u/alice:example.org today");
    REQUIRE(s.size() == 3);
    CHECK(s[1].url   == "matrix:u/alice:example.org");
    CHECK(s[1].text  == "matrix:u/alice:example.org");
}

TEST_CASE("autolink: matrix:r/ room alias is linkified", "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans("try matrix:r/general:example.org");
    REQUIRE(s.size() == 2);
    CHECK(s[1].url == "matrix:r/general:example.org");
}

TEST_CASE("autolink: matrix:roomid/ room ID is linkified", "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans("matrix:roomid/abc123:example.com");
    REQUIRE(s.size() == 1);
    CHECK(s[0].url == "matrix:roomid/abc123:example.com");
}

TEST_CASE("autolink: matrix: word boundary guard — preceded by alphanumeric not linked",
          "[html_spans][autolink]")
{
    CHECK(autolink_plain_to_spans("xmatrix:r/general:example.org").empty());
    CHECK(autolink_plain_to_spans("1matrix:r/general:example.org").empty());
}

TEST_CASE("autolink: matrix: URI trailing punctuation stripped", "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans("see matrix:r/general:example.org.");
    REQUIRE(s.size() >= 1);
    bool found = false;
    for (const auto& span : s)
    {
        if (!span.url.empty())
        {
            CHECK(span.url == "matrix:r/general:example.org");
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("autolink: unknown matrix: segment not linked", "[html_spans][autolink]")
{
    CHECK(autolink_plain_to_spans("matrix:bogus/whatever").empty());
}

TEST_CASE("autolink: mixed http and matrix: URIs each become spans",
          "[html_spans][autolink]")
{
    auto s = autolink_plain_to_spans(
        "see https://example.org and matrix:u/alice:example.org");
    REQUIRE(s.size() >= 2);
    bool has_http = false, has_matrix = false;
    for (const auto& span : s)
    {
        if (span.url == "https://example.org")  has_http   = true;
        if (span.url == "matrix:u/alice:example.org") has_matrix = true;
    }
    CHECK(has_http);
    CHECK(has_matrix);
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

TEST_CASE("whitespace: raw newlines and spaces in text nodes collapse",
          "[html_spans][whitespace]")
{
    // Literal \n between words collapses to a single space.
    CHECK(joined_text(html_to_spans("a\nb", false)) == "a b");
    // Multiple spaces/newlines collapse to one space.
    CHECK(joined_text(html_to_spans("a\n   b", false)) == "a b");
    // Newlines around inline tags are preserved as inter-word spacing.
    CHECK(joined_text(html_to_spans("pushed\n        <a href=\"https://x\">1 commit</a>\n    to",
                                    false)) == "pushed 1 commit to");
    // Trailing whitespace in the last span is emitted but gets trimmed by the
    // existing trailing-newline cleanup only if it is a '\n' — a trailing
    // space that collapsed from raw whitespace is left as-is (harmless).
    CHECK(joined_text(html_to_spans("hello ", false)) == "hello ");
}

TEST_CASE("br: self-closed and bare forms both produce a line break",
          "[html_spans][br]")
{
    // Real formatted_body is round-tripped through an HTML5 sanitizer
    // (matrix-sdk-ui's Timeline sanitizer, or sdk/src/html_sanitize.rs) by
    // the time it reaches here, which re-serializes self-closed <br /> as
    // bare <br> — both forms must produce an embedded '\n', not just the
    // self-closed one Tesseract's own composer happens to send.
    CHECK(joined_text(html_to_spans("A<br />B", false)) == "A\nB");
    CHECK(joined_text(html_to_spans("A<br>B", false)) == "A\nB");
    CHECK(joined_block_text(html_to_blocks("A<br />B", false)) == "A\nB");
    CHECK(joined_block_text(html_to_blocks("A<br>B", false)) == "A\nB");

    // The exact repro: three lines, pretty-printed with a literal '\n' right
    // after each <br /> (this is what markdown_to_html actually emits). The
    // collapsible whitespace at the start of each new line must vanish, not
    // become a stray leading space in front of the next line's text.
    const char* repro = "A<br />\nA<br />\nA";
    CHECK(joined_text(html_to_spans(repro, false)) == "A\nA\nA");
    CHECK(joined_block_text(html_to_blocks(repro, false)) == "A\nA\nA");

    // Same repro, but as it actually arrives after HTML5 sanitization strips
    // the self-closing slash.
    const char* repro_bare = "A<br>\nA<br>\nA";
    CHECK(joined_text(html_to_spans(repro_bare, false)) == "A\nA\nA");
    CHECK(joined_block_text(html_to_blocks(repro_bare, false)) == "A\nA\nA");

    // A bare <br> must not be mistaken for an opening tag awaiting a </br>
    // that never arrives — text after it keeps default (unstyled) formatting
    // rather than picking up some corrupted stack state.
    auto spans = html_to_spans("A<br>B<br>C", false);
    for (const auto& s : spans)
    {
        CHECK_FALSE(s.bold);
        CHECK_FALSE(s.italic);
    }
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

TEST_CASE("depth-cap: pathological nesting stays bounded and never crashes",
          "[html_spans][depth]")
{
    // The parser is iterative (no recursion) and the formatting stack is
    // hard-capped at kMaxTagDepth, so even a hostile body of tens of
    // thousands of tags must complete in bounded memory without a stack
    // overflow. Reaching the end of each CHECK is the assertion.
    constexpr int kHuge = 100000;

    // A massive UNCLOSED nest — the worst case for an unbounded formatting
    // stack (every open would push a frame).
    {
        std::string html;
        html.reserve(static_cast<std::size_t>(kHuge) * 3 + 4);
        for (int i = 0; i < kHuge; ++i)
            html += "<b>";
        html += "deep";
        CHECK_NOTHROW(html_to_spans(html, false));
    }

    // A massive balanced nest, then trailing text — exercises both the push
    // cap and the matching-close absorption at scale.
    {
        std::string html;
        html.reserve(static_cast<std::size_t>(kHuge) * 7 + 8);
        for (int i = 0; i < kHuge; ++i)
            html += "<i>";
        html += "x";
        for (int i = 0; i < kHuge; ++i)
            html += "</i>";
        html += "tail";
        auto spans = html_to_spans(html, false);
        // The body is still rendered (text survives the over-deep section).
        CHECK(joined_text(spans).find("tail") != std::string::npos);
    }

    // Far more CLOSE tags than opens must not underflow the stack (it never
    // pops below the base frame).
    {
        std::string html = "<b>x</b>";
        for (int i = 0; i < kHuge; ++i)
            html += "</b>";
        html += "after";
        CHECK_NOTHROW(html_to_spans(html, false));
    }
}

// --- Gap 3: hostile <a href> schemes are never linkified ------------------
//
// The renderer emits inert text spans (it is not a web view), so the one
// place an incoming body can smuggle an actionable payload is the href of a
// link the user might click. parse_tag keeps href ONLY when it begins with
// http:// or https:// — every other scheme must be dropped so the run is
// plain, unclickable text. A regression here would let a crafted message ship
// a `javascript:` / `data:` link straight to on_link_clicked.

TEST_CASE("hostile: javascript: href is not turned into a link",
          "[html_spans][hostile]")
{
    auto s = html_to_spans("<a href=\"javascript:alert(1)\">click</a>", false);
    CHECK_FALSE(any_link(s));          // no clickable url survives
    CHECK(joined_text(s) == "click");  // the visible text is preserved, inert
}

TEST_CASE("hostile: data:, vbscript:, file: and relative hrefs are dropped",
          "[html_spans][hostile]")
{
    const char* hostile[] = {
        "<a href=\"data:text/html,<script>alert(1)</script>\">x</a>",
        "<a href=\"vbscript:msgbox(1)\">x</a>",
        "<a href=\"file:///etc/passwd\">x</a>",
        "<a href=\"/relative/path\">x</a>",
        "<a href=\"mailto:a@b.org\">x</a>",
        // Leading whitespace must not sneak a scheme past the http(s) prefix
        // check (the value is taken verbatim from the attribute).
        "<a href=\" javascript:alert(1)\">x</a>",
    };
    for (const char* h : hostile)
    {
        auto s = html_to_spans(h, false);
        INFO(h);
        CHECK_FALSE(any_link(s));
    }
}

TEST_CASE("hostile: a legitimate https link still works (fail-open is narrow)",
          "[html_spans][hostile]")
{
    // The allowlist must not be so tight it breaks ordinary links.
    auto s = html_to_spans("<a href=\"https://example.org/x\">ok</a>", false);
    REQUIRE(any_link(s));
    bool found = false;
    for (const auto& sp : s)
        if (sp.url == "https://example.org/x" && sp.text.find("ok") !=
                                                      std::string::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("hostile: entity-encoded scheme stays inert (attrs are not decoded)",
          "[html_spans][hostile]")
{
    // extract_attr returns the raw attribute bytes; it does NOT run HTML
    // entity decoding, so an encoded "javascript:" never resolves to the
    // real scheme and can't be linkified.
    auto s = html_to_spans(
        "<a href=\"&#106;avascript:alert(1)\">x</a>", false);
    CHECK_FALSE(any_link(s));
}

// --- Gap 4: dangerous/unknown tags are stripped, content stays inert -------
//
// <script>, <style>, <iframe>, <img onerror=…> etc. are not in the allowlist.
// Unknown tags are dropped while their text content is preserved as plain
// text — crucially WITHOUT any url, color, or other actionable attribute. The
// payload becomes visible inert text, never something the UI acts on.

TEST_CASE("hostile: <script> body is preserved as inert plain text",
          "[html_spans][hostile]")
{
    auto s = html_to_spans("<script>alert(1)</script>", false);
    CHECK_FALSE(any_link(s));
    // The unknown tag is stripped; its text content remains, harmless.
    CHECK(joined_text(s) == "alert(1)");
}

TEST_CASE("hostile: <img onerror> / <iframe> carry nothing actionable",
          "[html_spans][hostile]")
{
    auto img = html_to_spans(
        "<img src=\"x\" onerror=\"alert(1)\">caption", false);
    CHECK_FALSE(any_link(img));
    CHECK(joined_text(img) == "caption");

    auto frame = html_to_spans(
        "<iframe src=\"javascript:alert(1)\">fallback</iframe>", false);
    CHECK_FALSE(any_link(frame));
    CHECK(joined_text(frame) == "fallback");
}

TEST_CASE("hostile: <style> content is not linkified",
          "[html_spans][hostile]")
{
    auto s = html_to_spans(
        "<style>a{background:url(javascript:alert(1))}</style>body", false);
    CHECK_FALSE(any_link(s));
    // The trailing real body text survives.
    CHECK(joined_text(s).find("body") != std::string::npos);
}

TEST_CASE("hostile: HTML comments and DOCTYPE are skipped without leaking",
          "[html_spans][hostile]")
{
    auto s = html_to_spans(
        "<!-- <a href=\"javascript:alert(1)\">x</a> -->visible", false);
    CHECK_FALSE(any_link(s));
    CHECK(joined_text(s) == "visible");

    auto d = html_to_spans("<!DOCTYPE html>hi", false);
    CHECK(joined_text(d) == "hi");
}

// --- Gap 5: malformed / truncated input is panic-safe ---------------------
//
// Incoming formatted_body is fully attacker-controlled and may be malformed.
// The parser must never read past the buffer, loop forever, or crash on
// unterminated tags, unbalanced quotes, or truncated entities. Reaching the
// end of each CHECK (no crash / hang) IS the assertion; we also assert the
// safety invariant that no hostile href slipped through.

TEST_CASE("malformed: unterminated tags and stray '<' do not crash",
          "[html_spans][malformed]")
{
    // No closing '>' on the tag — parse_tag must stop at end, not overrun.
    CHECK_NOTHROW(html_to_spans("<a href=\"http://x", false));
    CHECK_NOTHROW(html_to_spans("<b>bold but never closed", false));
    CHECK_NOTHROW(html_to_spans("a < b < c", false));
    CHECK_NOTHROW(html_to_spans("<<<<>>>>", false));
    CHECK_NOTHROW(html_to_spans("<", false));
    CHECK_NOTHROW(html_to_spans("</", false));
    // Unbalanced quote inside an attribute must not run off the end.
    CHECK_NOTHROW(html_to_spans("<a href=\"http://x>still text", false));
}

TEST_CASE("malformed: an unterminated http href is not exposed as a link",
          "[html_spans][malformed]")
{
    // The tag never closes, so it should not yield a usable link span.
    auto s = html_to_spans("<a href=\"javascript:alert(1)", false);
    CHECK_FALSE(any_link(s));
}

TEST_CASE("malformed: truncated and empty entities do not crash",
          "[html_spans][malformed]")
{
    CHECK_NOTHROW(html_to_spans("&", false));
    CHECK_NOTHROW(html_to_spans("&#", false));
    CHECK_NOTHROW(html_to_spans("&#x", false));
    CHECK_NOTHROW(html_to_spans("&#;", false));
    CHECK_NOTHROW(html_to_spans("&#x;", false));
    CHECK_NOTHROW(html_to_spans("&;", false));
    CHECK_NOTHROW(html_to_spans("&#x110000", false)); // no terminating ';'
    CHECK_NOTHROW(html_to_spans("&nosuchentity;", false));
    // A bare, undecodable '&' is emitted literally.
    CHECK(joined_text(html_to_spans("AT&T", false)) == "AT&T");
}

TEST_CASE("malformed: unterminated <pre> code block is still emitted",
          "[html_spans][malformed]")
{
    // A <pre> with no closing </pre> must flush its buffer at end-of-input
    // rather than silently dropping the captured text.
    auto s = html_to_spans("<pre>let x = 1;", false);
    CHECK(joined_text(s).find("let x = 1;") != std::string::npos);
}

// ── html_to_blocks ────────────────────────────────────────────────────────────
//
// Tests for the block-level parser that drives structured rendering of
// headings, lists, blockquotes, and tables.

namespace
{
// Concatenate the text of every span in a block.
std::string joined_block_text(const BodyBlock& b)
{
    std::string out;
    for (const auto& sp : b.spans)
        out += sp.text;
    return out;
}

// Find the first block of the given kind, or nullptr.
const BodyBlock* find_block(const std::vector<BodyBlock>& blocks,
                            BodyBlock::Kind                kind)
{
    for (const auto& b : blocks)
        if (b.kind == kind)
            return &b;
    return nullptr;
}
} // namespace

TEST_CASE("blocks: plain paragraph produces one Paragraph block",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks("hello world", false);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].kind == BodyBlock::Kind::Paragraph);
    CHECK(joined_block_text(blocks[0]) == "hello world");
}

TEST_CASE("blocks: empty HTML produces no blocks", "[html_spans][blocks]")
{
    CHECK(html_to_blocks("", false).empty());
}

TEST_CASE("blocks: h1 produces a Heading block with level 1",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks("<h1>Title</h1>", false);
    const BodyBlock* h = find_block(blocks, BodyBlock::Kind::Heading);
    REQUIRE(h != nullptr);
    CHECK(h->level == 1);
    CHECK(joined_block_text(*h) == "Title");
}

TEST_CASE("blocks: h1 through h6 produce correct levels",
          "[html_spans][blocks]")
{
    for (int lv = 1; lv <= 6; ++lv)
    {
        std::string html =
            "<h" + std::to_string(lv) + ">H</h" + std::to_string(lv) + ">";
        auto blocks = html_to_blocks(html, false);
        const BodyBlock* h = find_block(blocks, BodyBlock::Kind::Heading);
        REQUIRE(h != nullptr);
        CHECK(h->level == lv);
    }
}

TEST_CASE("blocks: heading spans are semibold", "[html_spans][blocks]")
{
    auto blocks = html_to_blocks("<h2>Semibold heading</h2>", false);
    const BodyBlock* h = find_block(blocks, BodyBlock::Kind::Heading);
    REQUIRE(h != nullptr);
    REQUIRE_FALSE(h->spans.empty());
    for (const auto& sp : h->spans)
        if (!sp.text.empty())
            CHECK(sp.semibold);
}

TEST_CASE("blocks: heading followed by paragraph produces two blocks",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks("<h1>Title</h1><p>body</p>", false);
    bool has_heading = false, has_para = false;
    for (const auto& b : blocks)
    {
        if (b.kind == BodyBlock::Kind::Heading)   has_heading = true;
        if (b.kind == BodyBlock::Kind::Paragraph) has_para    = true;
    }
    CHECK(has_heading);
    CHECK(has_para);
}

TEST_CASE("blocks: unordered list produces UnorderedItem blocks",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks("<ul><li>one</li><li>two</li></ul>", false);
    int count = 0;
    for (const auto& b : blocks)
        if (b.kind == BodyBlock::Kind::UnorderedItem)
            ++count;
    REQUIRE(count == 2);
    // Text of each item is preserved.
    bool saw_one = false, saw_two = false;
    for (const auto& b : blocks)
    {
        if (b.kind != BodyBlock::Kind::UnorderedItem) continue;
        std::string t = joined_block_text(b);
        if (t.find("one") != std::string::npos) saw_one = true;
        if (t.find("two") != std::string::npos) saw_two = true;
    }
    CHECK(saw_one);
    CHECK(saw_two);
}

TEST_CASE("blocks: ordered list produces OrderedItem blocks with 1-based indices",
          "[html_spans][blocks]")
{
    auto blocks =
        html_to_blocks("<ol><li>first</li><li>second</li><li>third</li></ol>",
                       false);
    int idx = 1;
    for (const auto& b : blocks)
    {
        if (b.kind != BodyBlock::Kind::OrderedItem) continue;
        CHECK(b.index == idx++);
    }
    CHECK(idx == 4); // consumed all 3 items
}

TEST_CASE("blocks: list items carry the nesting level", "[html_spans][blocks]")
{
    // Top-level items are level 1; nested items are level 2.
    auto blocks = html_to_blocks(
        "<ul><li>outer<ul><li>inner</li></ul></li></ul>", false);
    int depth1 = 0, depth2 = 0;
    for (const auto& b : blocks)
    {
        if (b.kind != BodyBlock::Kind::UnorderedItem) continue;
        if (b.level == 1) ++depth1;
        if (b.level == 2) ++depth2;
    }
    CHECK(depth1 >= 1);
    CHECK(depth2 >= 1);
}

TEST_CASE("blocks: blockquote produces a Blockquote block",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks("<blockquote>quoted text</blockquote>", false);
    const BodyBlock* bq = find_block(blocks, BodyBlock::Kind::Blockquote);
    REQUIRE(bq != nullptr);
    CHECK(joined_block_text(*bq).find("quoted") != std::string::npos);
}

TEST_CASE("blocks: blockquote level is 1 for a top-level quote",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks("<blockquote>q</blockquote>", false);
    const BodyBlock* bq = find_block(blocks, BodyBlock::Kind::Blockquote);
    REQUIRE(bq != nullptr);
    CHECK(bq->level == 1);
}

TEST_CASE("blocks: nested blockquote produces level-2 block",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks(
        "<blockquote>outer<blockquote>inner</blockquote></blockquote>", false);
    const BodyBlock* inner = nullptr;
    for (const auto& b : blocks)
        if (b.kind == BodyBlock::Kind::Blockquote && b.level == 2)
            inner = &b;
    REQUIRE(inner != nullptr);
    CHECK(joined_block_text(*inner).find("inner") != std::string::npos);
}

TEST_CASE("blocks: blockquote with inner <p> (pulldown-cmark output) works",
          "[html_spans][blocks]")
{
    // pulldown-cmark wraps blockquote body in <p>…</p>.
    auto blocks =
        html_to_blocks("<blockquote>\n<p>quoted</p>\n</blockquote>", false);
    const BodyBlock* bq = find_block(blocks, BodyBlock::Kind::Blockquote);
    REQUIRE(bq != nullptr);
    CHECK(joined_block_text(*bq).find("quoted") != std::string::npos);
}

TEST_CASE("blocks: inline formatting within list items is preserved",
          "[html_spans][blocks]")
{
    auto blocks =
        html_to_blocks("<ul><li><b>bold</b> text</li></ul>", false);
    const BodyBlock* item = find_block(blocks, BodyBlock::Kind::UnorderedItem);
    REQUIRE(item != nullptr);
    bool has_bold = false;
    for (const auto& sp : item->spans)
        if (sp.bold) has_bold = true;
    CHECK(has_bold);
}

TEST_CASE("blocks: inline formatting within blockquote is preserved",
          "[html_spans][blocks]")
{
    auto blocks =
        html_to_blocks("<blockquote><em>italic</em></blockquote>", false);
    const BodyBlock* bq = find_block(blocks, BodyBlock::Kind::Blockquote);
    REQUIRE(bq != nullptr);
    bool has_italic = false;
    for (const auto& sp : bq->spans)
        if (sp.italic) has_italic = true;
    CHECK(has_italic);
}

TEST_CASE("blocks: table row produces a TableRow block", "[html_spans][blocks]")
{
    auto blocks = html_to_blocks(
        "<table><tr><td>A</td><td>B</td></tr></table>", false);
    const BodyBlock* row = find_block(blocks, BodyBlock::Kind::TableRow);
    REQUIRE(row != nullptr);
    std::string text = joined_block_text(*row);
    CHECK(text.find('A') != std::string::npos);
    CHECK(text.find('B') != std::string::npos);
    // Cells must be separated by '│'.
    CHECK(text.find("\xe2\x94\x82") != std::string::npos); // U+2502 BOX DRAWINGS LIGHT VERTICAL
}

TEST_CASE("blocks: table header row has index=1 and bold cells",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks(
        "<table><thead><tr><th>Name</th><th>Age</th></tr></thead></table>",
        false);
    const BodyBlock* row = find_block(blocks, BodyBlock::Kind::TableRow);
    REQUIRE(row != nullptr);
    CHECK(row->index == 1); // header row
    bool has_bold = false;
    for (const auto& sp : row->spans)
        if (sp.bold) has_bold = true;
    CHECK(has_bold);
}

TEST_CASE("blocks: link within a list item survives as a hyperlink span",
          "[html_spans][blocks]")
{
    auto blocks = html_to_blocks(
        "<ul><li><a href=\"https://example.org\">link</a></li></ul>", false);
    const BodyBlock* item = find_block(blocks, BodyBlock::Kind::UnorderedItem);
    REQUIRE(item != nullptr);
    bool has_url = false;
    for (const auto& sp : item->spans)
        if (!sp.url.empty()) has_url = true;
    CHECK(has_url);
}

TEST_CASE("blocks: indented list-item content has no leading space span",
          "[html_spans][blocks]")
{
    // Indented HTML whitespace (\n + spaces) between <li> and the first
    // inline element must be stripped so that all canvas backends share
    // the same byte offsets when painting span backgrounds.
    auto blocks = html_to_blocks(
        "<ul><li>\n    <a href=\"https://example.org\"><code>abc</code></a>"
        "\n</li></ul>",
        false);
    const BodyBlock* item = find_block(blocks, BodyBlock::Kind::UnorderedItem);
    REQUIRE(item != nullptr);
    REQUIRE(!item->spans.empty());
    CHECK(item->spans.front().text.front() != ' ');
}

// ── <img data-mx-emoticon> (MSC2545 inline custom emoticon) ─────────────────

TEST_CASE("img: self-closed MSC2545 emoticon becomes an image span",
          "[html_spans][img]")
{
    auto s = html_to_spans(
        "hi <img data-mx-emoticon src=\"mxc://x.org/abc\" alt=\":wave:\" "
        "title=\":wave:\" height=\"32\"/> there",
        false);
    bool found = false;
    for (const auto& sp : s)
    {
        if (sp.is_image)
        {
            found = true;
            CHECK(sp.image_mxc == "mxc://x.org/abc");
            CHECK(sp.image_alt == ":wave:");
            CHECK(sp.text.empty());
        }
    }
    CHECK(found);
}

TEST_CASE("img: bare (non-self-closed) MSC2545 emoticon is also recognised "
          "and does not corrupt later tag nesting",
          "[html_spans][img]")
{
    // A bare <img> (no trailing slash) must not be pushed onto the
    // formatting stack expecting a </img> that never arrives — confirmed by
    // checking that <b> after it still toggles bold correctly.
    auto s = html_to_spans(
        "<img data-mx-emoticon src=\"mxc://x.org/abc\" alt=\":wave:\"> "
        "<b>bold</b> plain",
        false);
    bool found_image = false;
    bool found_bold = false;
    bool found_plain = false;
    for (const auto& sp : s)
    {
        if (sp.is_image)
        {
            found_image = true;
            CHECK(sp.image_mxc == "mxc://x.org/abc");
        }
        if (sp.bold && sp.text == "bold")
        {
            found_bold = true;
        }
        if (!sp.bold && sp.text.find("plain") != std::string::npos)
        {
            found_plain = true;
        }
    }
    CHECK(found_image);
    CHECK(found_bold);
    CHECK(found_plain);
}

TEST_CASE("img: without data-mx-emoticon falls back to alt text",
          "[html_spans][img]")
{
    auto s = html_to_spans(
        "<img src=\"mxc://x.org/abc\" alt=\"a plain image\"/>", false);
    for (const auto& sp : s)
        CHECK_FALSE(sp.is_image);
    CHECK(joined_text(s).find("a plain image") != std::string::npos);
}

TEST_CASE("img: a block-leading emoticon (no preceding text, no <p> "
          "wrapper) is not silently dropped by commit_block()'s leading-"
          "whitespace trim",
          "[html_spans][img][blocks]")
{
    // Regression test for a real Element-sent message: commit_block() trims
    // CSS-collapsible leading whitespace from the first span in a block by
    // checking find_first_not_of(' ') == npos — which an EMPTY string also
    // satisfies vacuously, so an image span (legitimately empty text) with
    // nothing before it was being erased outright before ever reaching
    // paint.
    auto blocks = html_to_blocks(
        "<img data-mx-emoticon "
        "src=\"mxc://gnomos.org/7237e619d21c4054078c8bf4c915574705d69081\" "
        "alt=\":cacodemon:\" title=\":cacodemon:\" height=\"32\"/> oh",
        false);
    REQUIRE(!blocks.empty());
    bool found_image = false;
    for (auto& b : blocks)
        for (auto& sp : b.spans)
            if (sp.is_image)
                found_image = true;
    CHECK(found_image);
}

TEST_CASE("img: non-mxc src is rejected even with data-mx-emoticon",
          "[html_spans][img]")
{
    auto s = html_to_spans(
        "<img data-mx-emoticon src=\"https://evil.example/x.png\" "
        "alt=\":wave:\"/>",
        false);
    for (const auto& sp : s)
        CHECK_FALSE(sp.is_image);
    CHECK(joined_text(s).find(":wave:") != std::string::npos);
}

TEST_CASE("img: alt falls back to title when alt is absent",
          "[html_spans][img]")
{
    auto s = html_to_spans(
        "<img data-mx-emoticon src=\"mxc://x.org/abc\" title=\":smile:\"/>",
        false);
    bool found = false;
    for (const auto& sp : s)
    {
        if (sp.is_image)
        {
            found = true;
            CHECK(sp.image_alt == ":smile:");
        }
    }
    CHECK(found);
}

TEST_CASE("img: recognised inside html_to_blocks paragraphs too",
          "[html_spans][img][blocks]")
{
    auto blocks = html_to_blocks(
        "<p>hi <img data-mx-emoticon src=\"mxc://x.org/abc\" "
        "alt=\":wave:\"/></p>",
        false);
    const BodyBlock* p = find_block(blocks, BodyBlock::Kind::Paragraph);
    REQUIRE(p != nullptr);
    bool found = false;
    for (const auto& sp : p->spans)
        if (sp.is_image)
            found = true;
    CHECK(found);
}
