#include "LocationMapPanner.h"

#include "MessageListView.h" // MessageRowData (full definition)

#include "tk/i18n.h"
#include "tk/theme.h"
#include "tk/widget.h" // tk::PaintCtx

#include <algorithm>
#include <string>
#include <vector>

namespace tesseract::views
{

// Paint a slippy-map tile composite for a Kind::Location row.
// `map_rect` is the bounding box for the map canvas area (pre-clipped).
void LocationMapPanner::paint(const MessageRowData& m, tk::PaintCtx& ctx,
                              tk::Rect map_rect)
{
    record_geom(m.event_id, map_rect);

    // Round corners on the map card.
    ctx.canvas.push_clip_rounded_rect(map_rect, 8.0f);

    const auto& vp = m.map_viewport;
    tk::Point vp_px = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);
    auto tiles = tiles_in_view(vp, map_rect);

    // Draw tiles (or a placeholder if not yet loaded).
    bool any_tile_drawn = false;
    for (const auto& t : tiles)
    {
        tk::Point origin = tile_pixel_origin(t, vp_px, map_rect);
        tk::Rect tdst{origin.x, origin.y, 256.0f, 256.0f};
        const std::string key = tile_cache_key(t);
        const tk::Image* img =
            tile_image_provider_ ? tile_image_provider_(key) : nullptr;
        if (img)
        {
            ctx.canvas.draw_image(*img, tdst);
            any_tile_drawn = true;
        }
        else
        {
            // Placeholder: grey base, then overlay the nearest cached
            // approximation so the map doesn't flash blank on zoom.
            ctx.canvas.fill_rect(tdst, ctx.theme.palette.chrome_bg);
            if (tile_image_provider_)
            {
                bool used_parent = false;
                if (t.z > 0)
                {
                    // Parent tile (zoom-1): this child occupies a 128×128
                    // quadrant of the parent's 256×256 image; stretch it.
                    TileCoord parent{t.z - 1, t.x / 2, t.y / 2};
                    if (const tk::Image* pimg =
                            tile_image_provider_(tile_cache_key(parent)))
                    {
                        int qx = t.x & 1, qy = t.y & 1;
                        tk::Rect src{qx * 128.0f, qy * 128.0f, 128.0f,
                                     128.0f};
                        ctx.canvas.draw_image_subregion(*pimg, src, tdst);
                        any_tile_drawn = true;
                        used_parent = true;
                    }
                }
                if (!used_parent && t.z < 19)
                {
                    // Child tiles (zoom+1): each covers a 128×128 quadrant
                    // of this tile's area; draw whichever are cached.
                    for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        TileCoord child{t.z + 1, t.x * 2 + dx,
                                        t.y * 2 + dy};
                        if (const tk::Image* cimg =
                                tile_image_provider_(tile_cache_key(child)))
                        {
                            tk::Rect qdst{tdst.x + dx * 128.0f,
                                          tdst.y + dy * 128.0f, 128.0f,
                                          128.0f};
                            ctx.canvas.draw_image(*cimg, qdst);
                            any_tile_drawn = true;
                        }
                    }
                }
            }
            if (tile_request_)
            {
                tile_request_(t.z, t.x, t.y);
            }
        }
    }

    // Show a hint text when no tiles have loaded yet so the map area
    // doesn't look like a rendering gap.
    if (!any_tile_drawn && !tiles.empty())
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.max_width = map_rect.w;
        auto lo = ctx.factory.build_text(tk::tr("Loading map\xe2\x80\xa6"), st);
        if (lo)
        {
            tk::Size sz = lo->measure();
            ctx.canvas.draw_text(
                *lo,
                {map_rect.x + (map_rect.w - sz.w) * 0.5f,
                 map_rect.y + (map_rect.h - lo->ascent()) * 0.5f},
                ctx.theme.palette.text_muted);
        }
    }

    // Location pin — white outer disc + red inner disc, centred on the
    // pin coordinate. Use fill_rounded_rect with radius = diameter/2.
    {
        tk::Point pin_px =
            lat_lon_to_world_px(m.location_lat, m.location_lon, vp.zoom);
        float map_cx = map_rect.x + map_rect.w * 0.5f;
        float map_cy = map_rect.y + map_rect.h * 0.5f;
        float pin_sx = map_cx + (pin_px.x - vp_px.x);
        float pin_sy = map_cy + (pin_px.y - vp_px.y);

        constexpr float kPinOuter = 18.0f;
        constexpr float kPinInner = 12.0f;
        tk::Rect outer{pin_sx - kPinOuter * 0.5f, pin_sy - kPinOuter * 0.5f,
                       kPinOuter, kPinOuter};
        tk::Rect inner{pin_sx - kPinInner * 0.5f, pin_sy - kPinInner * 0.5f,
                       kPinInner, kPinInner};
        ctx.canvas.fill_rounded_rect(outer, kPinOuter * 0.5f,
                                     tk::Color{255, 255, 255, 230});
        ctx.canvas.fill_rounded_rect(inner, kPinInner * 0.5f,
                                     tk::Color::rgb(0xE53935));
    }

    // Attribution badge — "© OpenStreetMap contributors" at bottom-right.
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto lo = ctx.factory.build_text(
            "\xC2\xA9 OpenStreetMap contributors", st);
        if (lo)
        {
            tk::Size sz = lo->measure();
            constexpr float kBadgePadX = 5.0f;
            constexpr float kBadgePadY = 3.0f;
            float bx =
                map_rect.x + map_rect.w - sz.w - kBadgePadX * 2 - 4.0f;
            float by =
                map_rect.y + map_rect.h - sz.h - kBadgePadY * 2 - 4.0f;
            tk::Rect badge{bx, by, sz.w + kBadgePadX * 2,
                           sz.h + kBadgePadY * 2};
            ctx.canvas.fill_rounded_rect(badge, 3.0f,
                                         tk::Color{255, 255, 255, 180});
            ctx.canvas.draw_text(*lo, {bx + kBadgePadX, by + kBadgePadY},
                                 tk::Color{0, 0, 0, 200});
        }
    }

    ctx.canvas.pop_clip();
}

} // namespace tesseract::views
