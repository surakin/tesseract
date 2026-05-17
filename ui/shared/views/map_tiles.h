#pragma once
#include "tk/canvas.h"
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace tesseract::views {

struct TileCoord  { int z = 0, x = 0, y = 0; };
struct MapViewport { double lat = 0.0, lon = 0.0; int zoom = 15; };

/// Convert geographic coordinates to the slippy-map tile index at `zoom`.
inline TileCoord lat_lon_to_tile(double lat, double lon, int zoom) {
    const int n     = 1 << zoom;
    const double fn = static_cast<double>(n);
    const double lat_r = lat * M_PI / 180.0;
    int x = static_cast<int>(std::floor((lon + 180.0) / 360.0 * fn));
    int y = static_cast<int>(std::floor(
        (1.0 - std::log(std::tan(lat_r) + 1.0 / std::cos(lat_r)) / M_PI)
        / 2.0 * fn));
    x = std::max(0, std::min(x, n - 1));
    y = std::max(0, std::min(y, n - 1));
    return { zoom, x, y };
}

/// World-pixel position of a lat/lon at `zoom` (tile size = 256 px).
inline tk::Point lat_lon_to_world_px(double lat, double lon, int zoom) {
    const double n = static_cast<double>(1 << zoom) * 256.0;
    const double lat_r = lat * M_PI / 180.0;
    double fx = (lon + 180.0) / 360.0 * n;
    double fy = (1.0 - std::log(std::tan(lat_r) + 1.0 / std::cos(lat_r)) / M_PI)
                / 2.0 * n;
    return { static_cast<float>(fx), static_cast<float>(fy) };
}

/// Inverse of lat_lon_to_world_px.
inline std::pair<double, double> world_px_to_lat_lon(float px, float py, int zoom) {
    const double n = static_cast<double>(1 << zoom) * 256.0;
    double fx = static_cast<double>(px) / n;
    double fy = static_cast<double>(py) / n;
    double lon = fx * 360.0 - 180.0;
    double merc_y = M_PI * (1.0 - 2.0 * fy);
    double lat = (2.0 * std::atan(std::exp(merc_y)) - M_PI / 2.0) * (180.0 / M_PI);
    return { lat, lon };
}

/// Top-left pixel position of a tile within `map_rect`, given the viewport.
/// `viewport_px` is lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom).
inline tk::Point tile_pixel_origin(TileCoord t, tk::Point viewport_px,
                                    tk::Rect map_rect) {
    float tile_world_x = static_cast<float>(t.x) * 256.0f;
    float tile_world_y = static_cast<float>(t.y) * 256.0f;
    float map_cx = map_rect.x + map_rect.w * 0.5f;
    float map_cy = map_rect.y + map_rect.h * 0.5f;
    return { map_cx + (tile_world_x - viewport_px.x),
             map_cy + (tile_world_y - viewport_px.y) };
}

/// All tile coordinates needed to fill `map_rect` at the viewport zoom.
inline std::vector<TileCoord> tiles_in_view(MapViewport vp, tk::Rect map_rect) {
    const int max_idx = (1 << vp.zoom) - 1;
    tk::Point vp_px = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);
    float half_w = map_rect.w * 0.5f;
    float half_h = map_rect.h * 0.5f;
    int x0 = static_cast<int>(std::floor((vp_px.x - half_w) / 256.0f));
    int x1 = static_cast<int>(std::floor((vp_px.x + half_w) / 256.0f));
    int y0 = static_cast<int>(std::floor((vp_px.y - half_h) / 256.0f));
    int y1 = static_cast<int>(std::floor((vp_px.y + half_h) / 256.0f));
    x0 = std::max(0, x0); x1 = std::min(max_idx, x1);
    y0 = std::max(0, y0); y1 = std::min(max_idx, y1);
    std::vector<TileCoord> out;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            out.push_back({ vp.zoom, x, y });
    return out;
}

/// Stable image-cache key for a tile: "tile:z/x/y".
inline std::string tile_cache_key(TileCoord t) {
    return "tile:" + std::to_string(t.z) + "/" +
           std::to_string(t.x) + "/" + std::to_string(t.y);
}

/// OSM slippy-map tile URL.
inline std::string tile_url(TileCoord t) {
    return "https://tile.openstreetmap.org/" +
           std::to_string(t.z) + "/" +
           std::to_string(t.x) + "/" +
           std::to_string(t.y) + ".png";
}

} // namespace tesseract::views
