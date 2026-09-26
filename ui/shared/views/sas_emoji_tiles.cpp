#include "views/sas_emoji_tiles.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr int   kColumns     = 4;
constexpr float kTileH       = 72.0f;
constexpr float kTileGap     = 8.0f;
constexpr float kGlyphH      = 46.0f;
constexpr float kTileRadius  = 6.0f;
} // namespace

float sas_emoji_grid_height()
{
    return 2.0f * kTileH + kTileGap;
}

void paint_sas_emoji_grid(tk::PaintCtx& ctx, tk::Rect area,
                          const std::vector<VerificationEmoji>& emojis)
{
    const auto& pal = ctx.theme.palette;
    const float tile_w = (area.w - kTileGap * (kColumns - 1)) / kColumns;

    tk::TextStyle glyph_style;
    glyph_style.role = tk::FontRole::BigEmoji;

    tk::TextStyle caption_style;
    caption_style.role = tk::FontRole::Small;
    caption_style.trim = tk::TextTrim::Ellipsis;
    caption_style.max_width = tile_w - 8.0f;

    const int count = static_cast<int>(emojis.size());
    for (int i = 0; i < count; ++i)
    {
        const int row = i / kColumns;
        const int col = i % kColumns;
        // Centre a short last row under the full one.
        const int in_row = std::min(kColumns, count - row * kColumns);
        const float row_w = in_row * tile_w + (in_row - 1) * kTileGap;
        const float x0 = area.x + (area.w - row_w) * 0.5f;
        tk::Rect r{x0 + col * (tile_w + kTileGap), area.y + row * (kTileH + kTileGap),
                   tile_w, kTileH};

        ctx.canvas.fill_rounded_rect(r, kTileRadius, pal.bg);
        ctx.canvas.stroke_rounded_rect(r, kTileRadius, pal.border, 1.0f);

        if (auto lo = ctx.factory.build_text(emojis[i].symbol, glyph_style))
        {
            const tk::Size sz = lo->measure();
            ctx.canvas.draw_text(*lo, {r.x + (r.w - sz.w) * 0.5f,
                                       r.y + (kGlyphH - sz.h) * 0.5f + 2.0f},
                                 pal.text_primary);
        }
        if (auto lo = ctx.factory.build_text(emojis[i].description, caption_style))
        {
            const tk::Size sz = lo->measure();
            ctx.canvas.draw_text(*lo, {r.x + (r.w - sz.w) * 0.5f,
                                       r.y + kGlyphH + (r.h - kGlyphH - sz.h) * 0.5f},
                                 pal.text_secondary);
        }
    }
}

} // namespace tesseract::views
