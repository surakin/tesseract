#pragma once

#include "tk/canvas.h"

#include <cmath>
#include <cstdint>

namespace tk
{

// Draw the standard 8-dot rotating "loading" spinner centred at `center`.
// `phase01` is the rotation phase in [0,1) (typically
// (elapsed_ms % 1000) / 1000). Dots fade around the ring from `base`'s colour;
// callers supply the base colour, ring radius and dot radius. Self-animation
// (scheduling the next repaint) stays with the caller. Single source of truth
// for the room-switch / pagination / media-load / verification spinners.
inline void draw_spinner_dots(Canvas& cv, Point center, float phase01,
                              float radius, float dot_r, Color base)
{
    constexpr int kN = 8;
    for (int i = 0; i < kN; ++i)
    {
        const float angle =
            (static_cast<float>(i) / kN + phase01) * 2.0f * 3.14159265f;
        const float dx    = std::cos(angle) * radius;
        const float dy    = std::sin(angle) * radius;
        const float t     = static_cast<float>(i) / kN;
        const auto  alpha = static_cast<std::uint8_t>(40.0f + 215.0f * t);
        cv.fill_rounded_rect({center.x + dx - dot_r, center.y + dy - dot_r,
                              dot_r * 2.0f, dot_r * 2.0f},
                             dot_r, base.with_alpha(alpha));
    }
}

} // namespace tk
