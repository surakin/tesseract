#include "tesseract/maps_link.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("classify_maps_link matches Google Maps @lat,lon form", "[maps_link]")
{
    auto r = tesseract::classify_maps_link(
        "https://www.google.com/maps/@41.403380,-2.174040,15z");
    REQUIRE(r.matched);
    REQUIRE_FALSE(r.needs_resolve);
    REQUIRE(r.lat == Catch::Approx(41.403380));
    REQUIRE(r.lon == Catch::Approx(-2.174040));
}

TEST_CASE("classify_maps_link matches Google Maps query form", "[maps_link]")
{
    auto r = tesseract::classify_maps_link(
        "https://www.google.com/maps?q=41.40338,-2.17404");
    REQUIRE(r.matched);
    REQUIRE_FALSE(r.needs_resolve);
    REQUIRE(r.lat == Catch::Approx(41.40338));
    REQUIRE(r.lon == Catch::Approx(-2.17404));
}

TEST_CASE("classify_maps_link matches OpenStreetMap query form", "[maps_link]")
{
    auto r = tesseract::classify_maps_link(
        "https://www.openstreetmap.org/?mlat=41.40338&mlon=-2.17404");
    REQUIRE(r.matched);
    REQUIRE_FALSE(r.needs_resolve);
    REQUIRE(r.lat == Catch::Approx(41.40338));
    REQUIRE(r.lon == Catch::Approx(-2.17404));
}

TEST_CASE("classify_maps_link marks goo.gl/maps shortlinks as needing resolve",
          "[maps_link]")
{
    auto r = tesseract::classify_maps_link("https://goo.gl/maps/abcd1234");
    REQUIRE(r.matched);
    REQUIRE(r.needs_resolve);
    REQUIRE(r.shortlink_url == "https://goo.gl/maps/abcd1234");
}

TEST_CASE("classify_maps_link marks maps.app.goo.gl shortlinks as needing resolve",
          "[maps_link]")
{
    auto r = tesseract::classify_maps_link("https://maps.app.goo.gl/xyz789");
    REQUIRE(r.matched);
    REQUIRE(r.needs_resolve);
    REQUIRE(r.shortlink_url == "https://maps.app.goo.gl/xyz789");
}

TEST_CASE("classify_maps_link marks osm.org/go shortlinks as needing resolve",
          "[maps_link]")
{
    auto r = tesseract::classify_maps_link("https://osm.org/go/0EEQjE-");
    REQUIRE(r.matched);
    REQUIRE(r.needs_resolve);
    REQUIRE(r.shortlink_url == "https://osm.org/go/0EEQjE-");
}

TEST_CASE("classify_maps_link rejects plain text", "[maps_link]")
{
    auto r = tesseract::classify_maps_link("check out this restaurant");
    REQUIRE_FALSE(r.matched);
}

TEST_CASE("classify_maps_link rejects a link with surrounding text",
          "[maps_link]")
{
    auto r = tesseract::classify_maps_link(
        "meet me here: https://www.google.com/maps/@41.4,-2.1,15z");
    REQUIRE_FALSE(r.matched);
}

TEST_CASE("classify_maps_link rejects an unrelated URL", "[maps_link]")
{
    auto r = tesseract::classify_maps_link("https://example.com/@41.4,-2.1,15z");
    REQUIRE_FALSE(r.matched);
}

TEST_CASE("classify_maps_link rejects an empty string", "[maps_link]")
{
    auto r = tesseract::classify_maps_link("");
    REQUIRE_FALSE(r.matched);
}
