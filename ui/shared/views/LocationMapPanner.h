#pragma once

// LocationMapPanner — interactive slippy-map pan/zoom subsystem for
// Kind::Location (MSC3488) rows, extracted from MessageListView. Owns:
//   - the pan FSM state (active row, drag-start anchor + viewport pixel),
//   - the wheel-zoom fractional accumulator,
//   - the map-area hover tooltip showing flag,
//   - the per-paint map hit-test geometry (event_id -> map rect).
//
// MessageListView holds one of these by value. It still owns `messages_`, so
// the dispatch handlers resolve the active row and hand the row's mutable
// MapViewport to the panner's pan/zoom math by reference. The painter
// (Adapter::paint_location_map) records its rect via record_geom() and the
// pointer hit-tests read it back via geom_at().
//
// The tile-fetch request callback (on_tile_needed) and the tooltip
// show/hide callbacks stay public on MessageListView (RoomView wires them);
// the panner invokes them through injected std::function wiring so the tile
// fetch + tooltip flow is preserved exactly.

#include "tk/canvas.h"
#include "views/map_tiles.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>

namespace tk
{
struct PaintCtx;
} // namespace tk

namespace tesseract::views
{

struct MessageRowData;

// Given an OSM-style tile cache key, return the decoded tile image (or
// nullptr on a cache miss, which the painter turns into a tile fetch).
using TileImageProvider =
    std::function<const tk::Image*(const std::string& cache_key)>;

class LocationMapPanner
{
public:
    static constexpr std::size_t kNoRow =
        std::numeric_limits<std::size_t>::max();

    // --- wiring (forwarded from MessageListView) ---
    // Image lookup for tiles (mirrors MessageListView::image_provider_).
    void set_tile_image_provider(TileImageProvider p)
    {
        tile_image_provider_ = std::move(p);
    }
    // Tile-fetch request: invoked for any tile not yet in the image cache.
    void set_tile_request(std::function<void(int z, int x, int y)> cb)
    {
        tile_request_ = std::move(cb);
    }
    // Tooltip show/hide for the location description on map hover.
    void set_tooltip_callbacks(
        std::function<void(std::string text, tk::Rect anchor)> show,
        std::function<void()> hide)
    {
        tooltip_show_ = std::move(show);
        tooltip_hide_ = std::move(hide);
    }

    // --- pan FSM (dispatched from on_pointer_down / move / up) ---
    // Begin a pan on `row_index`, anchoring at pointer `local` and the row's
    // current viewport. `vp` is the row's MapViewport (read only here).
    void begin_pan(std::size_t row_index, tk::Point local,
                   const MapViewport& vp)
    {
        active_row_ = row_index;
        drag_start_pt_ = local;
        drag_start_vp_px_ = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);
    }
    bool panning() const { return active_row_ != kNoRow; }
    std::size_t active_row() const { return active_row_; }
    // Apply a drag delta to `vp` (mutated in place). `local` is the current
    // pointer position. Returns the new lat/lon already written into `vp`.
    void drag_pan(tk::Point local, MapViewport& vp) const
    {
        float dx = local.x - drag_start_pt_.x;
        float dy = local.y - drag_start_pt_.y;
        float new_wp_x = drag_start_vp_px_.x - dx;
        float new_wp_y = drag_start_vp_px_.y - dy;
        auto [lat, lon] = world_px_to_lat_lon(new_wp_x, new_wp_y, vp.zoom);
        vp.lat = lat;
        vp.lon = lon;
    }
    // Ends the pan; `local` is the pointer-up position. Returns true if the
    // pointer never moved past the click threshold, i.e. this was a plain
    // click rather than a drag (mirrors the same-spot idiom used for
    // multi-click detection elsewhere in MessageListView).
    bool end_pan(tk::Point local)
    {
        active_row_ = kNoRow;
        float dx = local.x - drag_start_pt_.x;
        float dy = local.y - drag_start_pt_.y;
        return (dx * dx + dy * dy) < 64.0f;
    }

    // --- wheel zoom ---
    // Accumulate wheel delta `dy`; when it crosses the step threshold, adjust
    // `vp.zoom` (clamped to [1, 19]) and return true so the caller repaints.
    bool zoom(float dy, MapViewport& vp)
    {
        zoom_accum_ += dy;
        constexpr float kZoomThreshold = 60.0f;
        if (zoom_accum_ >= kZoomThreshold)
        {
            zoom_accum_ = 0.0f;
            vp.zoom = std::min(19, vp.zoom + 1);
            return true;
        }
        if (zoom_accum_ <= -kZoomThreshold)
        {
            zoom_accum_ = 0.0f;
            vp.zoom = std::max(1, vp.zoom - 1);
            return true;
        }
        return false;
    }

    // --- tooltip ---
    bool tooltip_showing() const { return tooltip_showing_; }
    // Show the tooltip if not already showing. No-op if already shown.
    void show_tooltip(const std::string& text, tk::Rect anchor)
    {
        if (tooltip_showing_)
            return;
        tooltip_showing_ = true;
        if (tooltip_show_)
            tooltip_show_(text, anchor);
    }
    // Hide the tooltip if currently showing. No-op otherwise.
    void hide_tooltip()
    {
        if (!tooltip_showing_)
            return;
        tooltip_showing_ = false;
        if (tooltip_hide_)
            tooltip_hide_();
    }

    // --- geometry: written by paint, read by the pointer hit-test ---
    // Cleared each paint pass from MessageListView::paint()'s geom-reset block
    // (same as the sibling hit-test maps). This is the fix for the previously
    // unbounded growth of the map rect geometry.
    void clear_geometry() { rect_geom_.clear(); }
    void record_geom(const std::string& event_id, tk::Rect rect)
    {
        rect_geom_[event_id] = rect;
    }
    const tk::Rect* geom_at(const std::string& event_id) const
    {
        auto it = rect_geom_.find(event_id);
        return it == rect_geom_.end() ? nullptr : &it->second;
    }

    // --- paint (Adapter delegates here) ---
    // Draws the slippy-map tile composite + pin + attribution for one
    // Kind::Location row into `map_rect` (pre-clipped). Records the rect for
    // hit-testing and requests any missing tiles via the tile-request cb.
    void paint(const MessageRowData& m, tk::PaintCtx& ctx,
               tk::Rect map_rect);

private:
    TileImageProvider tile_image_provider_;
    std::function<void(int z, int x, int y)> tile_request_;
    std::function<void(std::string text, tk::Rect anchor)> tooltip_show_;
    std::function<void()> tooltip_hide_;

    std::size_t active_row_ = kNoRow;
    tk::Point drag_start_pt_{};
    tk::Point drag_start_vp_px_{}; // world-pixel viewport at drag start
    float zoom_accum_ = 0.0f;      // fractional wheel accumulator
    bool tooltip_showing_ = false;

    mutable std::unordered_map<std::string, tk::Rect> rect_geom_;
};

} // namespace tesseract::views
