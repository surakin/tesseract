#include <catch2/catch_test_macros.hpp>
#include "views/map_tiles.h"
#include <cmath>

using namespace tesseract::views;

// Tolerance for floating-point lat/lon round-trips.
static constexpr double kEps = 1e-4;

TEST_CASE("lat_lon_to_tile: London at zoom 10", "[map_tiles]") {
    // London: 51.5074, -0.1278 → tile (511, 340) at z=10
    auto t = lat_lon_to_tile(51.5074, -0.1278, 10);
    CHECK(t.z == 10);
    CHECK(t.x == 511);
    CHECK(t.y == 340);
}

TEST_CASE("lat_lon_to_tile: tiles clamped to valid range", "[map_tiles]") {
    auto t = lat_lon_to_tile(90.0, 180.0, 5);
    CHECK(t.x >= 0); CHECK(t.x < (1 << 5));
    CHECK(t.y >= 0); CHECK(t.y < (1 << 5));
}

TEST_CASE("lat_lon_to_world_px / world_px_to_lat_lon round-trip", "[map_tiles]") {
    const double lat = 51.5074, lon = -0.1278;
    auto px = lat_lon_to_world_px(lat, lon, 12);
    auto [rlat, rlon] = world_px_to_lat_lon(px.x, px.y, 12);
    CHECK(std::abs(rlat - lat) < kEps);
    CHECK(std::abs(rlon - lon) < kEps);
}

TEST_CASE("tiles_in_view: 256x240 map at zoom 15 returns tiles", "[map_tiles]") {
    MapViewport vp{ 51.5074, -0.1278, 15 };
    tk::Rect map_rect{ 0, 0, 256, 240 };
    auto tiles = tiles_in_view(vp, map_rect);
    // Should be a small grid — at least 1 tile, at most ~20
    CHECK(!tiles.empty());
    CHECK(tiles.size() <= 20);
    for (const auto& t : tiles) {
        CHECK(t.z == 15);
        CHECK(t.x >= 0); CHECK(t.x < (1 << 15));
        CHECK(t.y >= 0); CHECK(t.y < (1 << 15));
    }
}

TEST_CASE("tile_cache_key format", "[map_tiles]") {
    CHECK(tile_cache_key({15, 123, 456}) == "tile:15/123/456");
}

TEST_CASE("tile_url format", "[map_tiles]") {
    CHECK(tile_url({15, 123, 456}) ==
          "https://tile.openstreetmap.org/15/123/456.png");
}

TEST_CASE("tile_pixel_origin places viewport-centre tile near map centre",
          "[map_tiles]") {
    MapViewport vp{ 51.5074, -0.1278, 15 };
    tk::Rect map_rect{ 0, 0, 256, 256 };
    auto vp_px = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);
    // The tile that contains the viewport centre
    auto centre_tile = lat_lon_to_tile(vp.lat, vp.lon, vp.zoom);
    auto origin = tile_pixel_origin(centre_tile, vp_px, map_rect);
    // The tile origin should be within one tile-width of the map centre
    float map_cx = map_rect.x + map_rect.w / 2.0f;
    float map_cy = map_rect.y + map_rect.h / 2.0f;
    CHECK(std::abs(origin.x - map_cx) <= 256.0f);
    CHECK(std::abs(origin.y - map_cy) <= 256.0f);
}
