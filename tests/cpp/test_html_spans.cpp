#include <catch2/catch_test_macros.hpp>

#include "views/html_spans.h"

using tesseract::views::autolink_plain_to_spans;

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
