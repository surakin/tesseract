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

} // namespace tk
