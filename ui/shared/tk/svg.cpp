#include "svg.h"

#include <algorithm>
#include <cstring>
#include <vector>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

namespace tk {

std::unique_ptr<Image> rasterize_svg(CanvasFactory& factory,
                                     std::span<const std::uint8_t> bytes,
                                     int target_px)
{
    if (bytes.empty() || target_px <= 0)
        return nullptr;

    // nsvgParse modifies the buffer in-place; we need a mutable null-terminated copy.
    std::vector<char> buf(bytes.size() + 1);
    std::memcpy(buf.data(), bytes.data(), bytes.size());
    buf.back() = '\0';

    NSVGimage* svg = nsvgParse(buf.data(), "px", 96.0f);
    if (!svg || svg->width <= 0.0f || svg->height <= 0.0f)
    {
        if (svg)
            nsvgDelete(svg);
        return nullptr;
    }

    float scale = static_cast<float>(target_px) /
                  std::max(svg->width, svg->height);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast)
    {
        nsvgDelete(svg);
        return nullptr;
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(target_px * target_px * 4));
    nsvgRasterize(rast, svg, 0.0f, 0.0f, scale,
                  pixels.data(), target_px, target_px, target_px * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    return factory.create_image_rgba(pixels.data(), target_px, target_px);
}

} // namespace tk
