#include <catch2/catch_test_macros.hpp>
#include "tesseract/client.h"
#include <string>

// Regression guard for the nlohmann-based parse_url_preview (Phase 4.11): the
// hand-rolled JSON scanner it replaced did UTF-16 surrogate decoding and a
// quote-agnostic integer scan by hand; these cases pin the behavior that must
// survive the swap.

using tesseract::Client;

TEST_CASE("parse_url_preview: full OpenGraph blob", "[url_preview]")
{
    const std::string json = R"({
        "og:title": "Example",
        "og:description": "A description",
        "og:image": "mxc://example.org/abc123",
        "og:image:width": 640,
        "og:image:height": 480
    })";
    Client::UrlPreview p = Client::parse_url_preview(json);
    CHECK(p.title == "Example");
    CHECK(p.description == "A description");
    CHECK(p.image_mxc == "mxc://example.org/abc123");
    CHECK(p.image_w == 640);
    CHECK(p.image_h == 480);
    CHECK(p.failed == false);
}

TEST_CASE("parse_url_preview: empty json yields failed", "[url_preview]")
{
    Client::UrlPreview p = Client::parse_url_preview("");
    CHECK(p.failed == true);
    CHECK(p.title.empty());
}

TEST_CASE("parse_url_preview: contentless blob yields failed", "[url_preview]")
{
    // No og:title/og:description -> has_content() is false -> failed=true,
    // even though an image is present.
    const std::string json = R"({"og:image":"mxc://h/x","og:image:width":1})";
    Client::UrlPreview p = Client::parse_url_preview(json);
    CHECK(p.failed == true);
    CHECK(p.image_mxc == "mxc://h/x");
}

TEST_CASE("parse_url_preview: missing dimensions default to zero", "[url_preview]")
{
    const std::string json = R"({"og:title":"T"})";
    Client::UrlPreview p = Client::parse_url_preview(json);
    CHECK(p.title == "T");
    CHECK(p.image_w == 0);
    CHECK(p.image_h == 0);
    CHECK(p.image_mxc.empty());
    CHECK(p.failed == false);
}

TEST_CASE("parse_url_preview: quoted-string dimensions are accepted", "[url_preview]")
{
    // Some OpenGraph servers emit dimensions as strings; the old digit scanner
    // accepted them and nlohmann path must too.
    const std::string json =
        R"({"og:title":"T","og:image:width":"800","og:image:height":"600"})";
    Client::UrlPreview p = Client::parse_url_preview(json);
    CHECK(p.image_w == 800);
    CHECK(p.image_h == 600);
}

TEST_CASE("parse_url_preview: surrogate-pair emoji decodes to UTF-8", "[url_preview]")
{
    // U+1F600 GRINNING FACE encoded as a UTF-16 surrogate pair. nlohmann must
    // combine the halves into valid 4-byte UTF-8.
    const std::string json = R"({"og:title":"😀"})";
    Client::UrlPreview p = Client::parse_url_preview(json);
    const std::string expected = "\xF0\x9F\x98\x80"; // U+1F600
    CHECK(p.title == expected);
    CHECK(p.failed == false);
}

TEST_CASE("parse_url_preview: malformed json degrades to failed", "[url_preview]")
{
    // Truncated/garbage JSON must not throw; it yields an all-default preview,
    // which is contentless -> failed.
    Client::UrlPreview p = Client::parse_url_preview("{not valid json");
    CHECK(p.failed == true);
    CHECK(p.title.empty());
}
