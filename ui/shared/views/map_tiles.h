#pragma once
#include "tk/canvas.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tesseract::views
{

struct TileCoord
{
    int z = 0, x = 0, y = 0;
};
struct MapViewport
{
    double lat = 0.0, lon = 0.0;
    int zoom = 15;
};

/// Convert geographic coordinates to the slippy-map tile index at `zoom`.
// Web Mercator is undefined at the poles, and a latitude out of range makes
// log/tan/cos non-finite. Clamp to the square-map limit, the longitude to
// [-180, 180], and the zoom to the tile servers' range, so the conversions below
// always stay finite and in range. The coordinates come from remote events.
inline constexpr double kMaxMercatorLat = 85.0511287798;
inline constexpr int kMaxZoom = 19;

inline double clamp_mercator_lat(double lat)
{
    return std::max(-kMaxMercatorLat, std::min(lat, kMaxMercatorLat));
}

inline double clamp_lon(double lon)
{
    return std::max(-180.0, std::min(lon, 180.0));
}

inline TileCoord lat_lon_to_tile(double lat, double lon, int zoom)
{
    zoom = std::max(0, std::min(zoom, kMaxZoom));
    const int n = 1 << zoom;
    const double fn = static_cast<double>(n);
    lat = clamp_mercator_lat(lat);
    lon = clamp_lon(lon);
    const double lat_r = lat * std::numbers::pi / 180.0;
    int x = static_cast<int>(std::floor((lon + 180.0) / 360.0 * fn));
    int y = static_cast<int>(std::floor(
        (1.0 - std::log(std::tan(lat_r) + 1.0 / std::cos(lat_r)) / std::numbers::pi) / 2.0 * fn));
    x = std::max(0, std::min(x, n - 1));
    y = std::max(0, std::min(y, n - 1));
    return {zoom, x, y};
}

/// World-pixel position of a lat/lon at `zoom` (tile size = 256 px).
inline tk::Point lat_lon_to_world_px(double lat, double lon, int zoom)
{
    zoom = std::max(0, std::min(zoom, kMaxZoom));
    const double n = static_cast<double>(1 << zoom) * 256.0;
    lat = clamp_mercator_lat(lat);
    lon = clamp_lon(lon);
    const double lat_r = lat * std::numbers::pi / 180.0;
    double fx = (lon + 180.0) / 360.0 * n;
    double fy =
        (1.0 - std::log(std::tan(lat_r) + 1.0 / std::cos(lat_r)) / std::numbers::pi) / 2.0 * n;
    return {static_cast<float>(fx), static_cast<float>(fy)};
}

/// Inverse of lat_lon_to_world_px.
inline std::pair<double, double> world_px_to_lat_lon(float px, float py, int zoom)
{
    const double n = static_cast<double>(1 << zoom) * 256.0;
    double fx = static_cast<double>(px) / n;
    double fy = static_cast<double>(py) / n;
    double lon = fx * 360.0 - 180.0;
    double merc_y = std::numbers::pi * (1.0 - 2.0 * fy);
    double lat =
        (2.0 * std::atan(std::exp(merc_y)) - std::numbers::pi / 2.0) * (180.0 / std::numbers::pi);
    return {lat, lon};
}

/// Top-left pixel position of a tile within `map_rect`, given the viewport.
/// `viewport_px` is lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom).
inline tk::Point tile_pixel_origin(TileCoord t, tk::Point viewport_px, tk::Rect map_rect)
{
    float tile_world_x = static_cast<float>(t.x) * 256.0f;
    float tile_world_y = static_cast<float>(t.y) * 256.0f;
    float map_cx = map_rect.x + map_rect.w * 0.5f;
    float map_cy = map_rect.y + map_rect.h * 0.5f;
    return {map_cx + (tile_world_x - viewport_px.x), map_cy + (tile_world_y - viewport_px.y)};
}

/// All tile coordinates needed to fill `map_rect` at the viewport zoom.
inline std::vector<TileCoord> tiles_in_view(MapViewport vp, tk::Rect map_rect)
{
    const int max_idx = (1 << vp.zoom) - 1;
    tk::Point vp_px = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);
    float half_w = map_rect.w * 0.5f;
    float half_h = map_rect.h * 0.5f;
    int x0 = static_cast<int>(std::floor((vp_px.x - half_w) / 256.0f));
    int x1 = static_cast<int>(std::floor((vp_px.x + half_w) / 256.0f));
    int y0 = static_cast<int>(std::floor((vp_px.y - half_h) / 256.0f));
    int y1 = static_cast<int>(std::floor((vp_px.y + half_h) / 256.0f));
    x0 = std::max(0, x0);
    x1 = std::min(max_idx, x1);
    y0 = std::max(0, y0);
    y1 = std::min(max_idx, y1);
    std::vector<TileCoord> out;
    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            out.push_back({vp.zoom, x, y});
        }
    }
    return out;
}

/// Stable image-cache key for a tile: "tile:z/x/y".
// True when `bytes` start with the PNG signature. Tiles are cached on disk only
// when they do, so an error page from the tile server is never kept as a tile.
inline bool is_png_bytes(const std::vector<std::uint8_t>& bytes)
{
    static constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= sizeof kSignature &&
           std::equal(std::begin(kSignature), std::end(kSignature), bytes.begin());
}

inline std::string tile_cache_key(TileCoord t)
{
    return "tile:" + std::to_string(t.z) + "/" + std::to_string(t.x) + "/" + std::to_string(t.y);
}

/// OSM slippy-map tile URL.
inline std::string tile_url(TileCoord t)
{
    return "https://tile.openstreetmap.org/" + std::to_string(t.z) + "/" + std::to_string(t.x) +
           "/" + std::to_string(t.y) + ".png";
}

/// Locale-independent fixed-point text of `v` with 6 decimals. std::to_string
/// follows the process numeric locale, so a comma-decimal locale produced
/// "51,500800" and a broken URL.
inline std::string coord_text(double v)
{
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(6) << v;
    return os.str();
}

/// URL for the openstreetmap.org web viewer, centered on `lat`/`lon` with a
/// marker dropped at that point.
inline std::string osm_view_url(double lat, double lon, int zoom)
{
    return "https://www.openstreetmap.org/?mlat=" + coord_text(lat) + "&mlon=" + coord_text(lon) +
           "#map=" + std::to_string(zoom) + "/" + coord_text(lat) + "/" + coord_text(lon);
}

/// URL to open when a location card is clicked. A location whose body is a
/// maps URL opens that verbatim (the sender's exact link), except for an
/// openstreetmap.org link containing ',': messages sent from a comma-decimal
/// locale carry "mlat=51,5008" links that don't resolve, so those are rebuilt
/// from the card's coordinates.
// Map services a location card may open. The body is sender-supplied, so a
// link to any other host is replaced by the coordinates URL built here.
inline bool is_known_map_url(std::string_view url)
{
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
    {
        return false;
    }
    const auto after = url.substr(scheme_end + 3);
    const auto host_end = after.find_first_of("/?#");
    const auto host = after.substr(0, host_end);
    const auto path =
        host_end == std::string_view::npos ? std::string_view{} : after.substr(host_end);
    if (host == "openstreetmap.org" || host == "www.openstreetmap.org" || host == "osm.org" ||
        host == "maps.apple.com")
    {
        return true;
    }
    if (host == "maps.google.com" || host == "www.google.com" || host == "google.com")
    {
        return path.starts_with("/maps");
    }
    return false;
}

inline std::string location_click_url(std::string_view body, double lat, double lon, int zoom)
{
    const bool body_is_url = body.starts_with("http://") || body.starts_with("https://");
    if (!body_is_url || !is_known_map_url(body))
    {
        return osm_view_url(lat, lon, zoom);
    }
    const bool broken_osm = (body.starts_with("https://www.openstreetmap.org/") ||
                             body.starts_with("https://openstreetmap.org/")) &&
                            body.find(',') != std::string_view::npos;
    return broken_osm ? osm_view_url(lat, lon, zoom) : std::string(body);
}

} // namespace tesseract::views
