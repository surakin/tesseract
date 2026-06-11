#include <catch2/catch_test_macros.hpp>

#include "app/status_links.h"

#include <string>

using tesseract::parse_status_links;
using tesseract::status_has_links;
using tesseract::status_plain_text;

TEST_CASE("status_links: plain text yields one verbatim segment")
{
    auto segs = parse_status_links("Connected");
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].text == "Connected");
    CHECK(segs[0].url.empty());
    CHECK_FALSE(status_has_links(segs));
}

TEST_CASE("status_links: empty input yields one empty segment")
{
    auto segs = parse_status_links("");
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].text.empty());
    CHECK(segs[0].url.empty());
    CHECK_FALSE(status_has_links(segs));
}

TEST_CASE("status_links: single link splits into three segments")
{
    auto segs = parse_status_links(
        "Update available \xe2\x80\x94 [Download](https://example.com/d) now");
    REQUIRE(segs.size() == 3);
    CHECK(segs[0].text == "Update available \xe2\x80\x94 ");
    CHECK(segs[0].url.empty());
    CHECK(segs[1].text == "Download");
    CHECK(segs[1].url == "https://example.com/d");
    CHECK(segs[2].text == " now");
    CHECK(segs[2].url.empty());
    CHECK(status_has_links(segs));
}

TEST_CASE("status_links: link at start and end of string")
{
    auto front = parse_status_links("[a](http://x) tail");
    REQUIRE(front.size() == 2);
    CHECK(front[0].url == "http://x");
    CHECK(front[1].text == " tail");

    auto back = parse_status_links("head [b](https://y)");
    REQUIRE(back.size() == 2);
    CHECK(back[0].text == "head ");
    CHECK(back[1].url == "https://y");
}

TEST_CASE("status_links: multiple links")
{
    auto segs = parse_status_links("[a](http://1)-[b](http://2)");
    REQUIRE(segs.size() == 3);
    CHECK(segs[0].url == "http://1");
    CHECK(segs[1].text == "-");
    CHECK(segs[1].url.empty());
    CHECK(segs[2].url == "http://2");
}

TEST_CASE("status_links: status_plain_text drops the markup")
{
    auto segs = parse_status_links("Update \xe2\x80\x94 [Download](https://e.com/d)!");
    CHECK(status_plain_text(segs) == "Update \xe2\x80\x94 Download!");
}

TEST_CASE("status_links: non-http schemes stay literal")
{
    const std::string cases[] = {
        "[x](javascript:alert(1))",
        "[x](file:///etc/passwd)",
        "[x](ftp://host/f)",
        "[x](mailto:a@b.c)",
        "[x](httpx://host)",
    };
    for (const auto& msg : cases)
    {
        auto segs = parse_status_links(msg);
        REQUIRE(segs.size() == 1);
        CHECK(segs[0].text == msg);
        CHECK_FALSE(status_has_links(segs));
    }
}

TEST_CASE("status_links: scheme match is case-insensitive")
{
    auto segs = parse_status_links("[x](HTTPS://Host/Path)");
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].url == "HTTPS://Host/Path");
    CHECK(status_has_links(segs));
}

TEST_CASE("status_links: malformed candidates stay literal")
{
    const std::string cases[] = {
        "[no paren]",
        "[unclosed](http://a",
        "](http://a)",
        "[](http://a)",
        "[x]()",
        "[x](http://a b)", // whitespace in url
        "see array[0] (index) for details",
    };
    for (const auto& msg : cases)
    {
        auto segs = parse_status_links(msg);
        REQUIRE(segs.size() == 1);
        CHECK(segs[0].text == msg);
        CHECK_FALSE(status_has_links(segs));
    }
}

TEST_CASE("status_links: label may contain brackets")
{
    // "](\" is matched at its first occurrence, so a bracketed label works.
    auto segs = parse_status_links("[a[b]](http://c)");
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].text == "a[b]");
    CHECK(segs[0].url == "http://c");
}

TEST_CASE("status_links: url ends at the first closing paren")
{
    // Documented limitation: ')' inside a url is unsupported.
    auto segs = parse_status_links("[x](http://a/(b))");
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].url == "http://a/(b");
    CHECK(segs[1].text == ")");
}

TEST_CASE("status_links: rejected candidate does not hide a later valid link")
{
    auto segs = parse_status_links("[bad](ftp://x) then [ok](http://y)");
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].text == "[bad](ftp://x) then ");
    CHECK(segs[0].url.empty());
    CHECK(segs[1].text == "ok");
    CHECK(segs[1].url == "http://y");
}
