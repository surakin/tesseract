#pragma once
#include "canvas.h"
#include <cstdint>
#include <memory>
#include <span>

namespace tk {

// Rasterize an SVG document into a square image of target_px × target_px
// device pixels. Returns nullptr on malformed input or allocation failure.
std::unique_ptr<Image> rasterize_svg(CanvasFactory& factory,
                                     std::span<const std::uint8_t> bytes,
                                     int target_px);

// As above, but recolor every pixel to `tint` after rasterizing:
// RGB = tint.rgb, alpha = src_alpha * tint.a / 255. Use for monochrome line
// icons (e.g. Lucide `currentColor`) so they adopt the surrounding theme /
// context color instead of nanosvg's flat default gray.
std::unique_ptr<Image> rasterize_svg(CanvasFactory& factory,
                                     std::span<const std::uint8_t> bytes,
                                     int target_px, Color tint);

// A lazily-rasterized, tinted SVG icon, cached until the DPI scale, tint, or
// logical size changes — so it stays crisp on HiDPI and recolors correctly when
// the theme switches. Hold one per icon-and-context as a view member and call
// draw() during paint(); it rasterizes at physical-pixel resolution and draws
// the icon centred within `box`.
class IconCache
{
public:
    void draw(Canvas& canvas, CanvasFactory& factory,
              std::span<const std::uint8_t> svg, Rect box, float logical_px,
              Color tint);

private:
    std::unique_ptr<Image> img_;
    float scale_ = -1.0f;
    float px_    = -1.0f;
    Color tint_{0, 0, 0, 0};
};

} // namespace tk
