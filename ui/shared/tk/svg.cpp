#include "svg.h"

#include <algorithm>
#include <cmath>
#include <vector>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

namespace tk {

namespace {

// Rasterize `bytes` into a freshly allocated target_px² RGBA8888 buffer.
// Returns an empty vector on malformed input / allocation failure.
std::vector<std::uint8_t>
rasterize_to_rgba(std::span<const std::uint8_t> bytes, int target_px)
{
    if (bytes.empty() || target_px <= 0)
        return {};

    // nsvgParse modifies the buffer in-place; we need a mutable null-terminated copy.
    std::vector<char> buf(bytes.begin(), bytes.end());
    buf.push_back('\0');

    NSVGimage* svg = nsvgParse(buf.data(), "px", 96.0f);
    if (!svg || svg->width <= 0.0f || svg->height <= 0.0f)
    {
        if (svg)
            nsvgDelete(svg);
        return {};
    }

    float scale = static_cast<float>(target_px) /
                  std::max(svg->width, svg->height);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast)
    {
        nsvgDelete(svg);
        return {};
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(target_px) * target_px * 4);
    nsvgRasterize(rast, svg, 0.0f, 0.0f, scale,
                  pixels.data(), target_px, target_px, target_px * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);
    return pixels;
}

} // namespace

std::unique_ptr<Image> rasterize_svg(CanvasFactory& factory,
                                     std::span<const std::uint8_t> bytes,
                                     int target_px)
{
    std::vector<std::uint8_t> pixels = rasterize_to_rgba(bytes, target_px);
    if (pixels.empty())
        return nullptr;
    return factory.create_image_rgba(pixels.data(), target_px, target_px);
}

std::unique_ptr<Image> rasterize_svg(CanvasFactory& factory,
                                     std::span<const std::uint8_t> bytes,
                                     int target_px, Color tint)
{
    std::vector<std::uint8_t> pixels = rasterize_to_rgba(bytes, target_px);
    if (pixels.empty())
        return nullptr;

    // Recolor: keep the rasterized coverage (alpha) but force RGB to the tint
    // and scale alpha by the tint's own alpha. nanosvg output is
    // non-premultiplied RGBA8888, so a straight channel swap is correct.
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
    {
        pixels[i + 0] = tint.r;
        pixels[i + 1] = tint.g;
        pixels[i + 2] = tint.b;
        pixels[i + 3] = static_cast<std::uint8_t>(
            (static_cast<int>(pixels[i + 3]) * tint.a) / 255);
    }

    return factory.create_image_rgba(pixels.data(), target_px, target_px);
}

void IconCache::draw(Canvas& canvas, CanvasFactory& factory,
                     std::span<const std::uint8_t> svg, Rect box,
                     float logical_px, Color tint)
{
    const float scale = canvas.scale_factor();
    const bool same_tint = tint.r == tint_.r && tint.g == tint_.g &&
                           tint.b == tint_.b && tint.a == tint_.a;
    if (!img_ || scale != scale_ || logical_px != px_ || !same_tint)
    {
        scale_ = scale;
        px_    = logical_px;
        tint_  = tint;
        img_   = rasterize_svg(
            factory, svg,
            std::max(1, static_cast<int>(std::lround(logical_px * scale))),
            tint);
    }
    if (img_)
        canvas.draw_image(*img_, {box.x + (box.w - logical_px) * 0.5f,
                                  box.y + (box.h - logical_px) * 0.5f,
                                  logical_px, logical_px});
}

} // namespace tk
