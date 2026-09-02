#pragma once
#include <cstdint>
#include <span>

// Accessors for the handful of Lucide icon documents the *native* platform
// shells need to paint outside any tk::Canvas surface (status-bar glyphs and
// the like). The generated icons.h byte arrays are on tesseract_tk's private
// include path only, so the shells go through these instead of #including it.
// Rasterize the bytes with tk::rasterize_svg / tk::rasterize_svg_rgba.

namespace tk
{

// Lucide "battery-low" (currentColor line icon) — the low-power-mode
// status-bar indicator.
std::span<const std::uint8_t> low_power_icon_svg();

} // namespace tk
