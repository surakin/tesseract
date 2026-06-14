#pragma once

#include "canvas.h"
#include "loading_spinner.h"

#include <algorithm>
#include <cstdint>

namespace tk
{

// Returns the rotation speed in revolutions-per-millisecond for a given
// combined in-flight request count. Returns 0 when n <= 1 (green/idle state:
// no ring animation). Linear ramp from 0.5 Hz at n=2 to 3 Hz at n>=18.
inline float inflight_revs_per_ms(uint32_t n)
{
    if (n <= 1u)
        return 0.0f;
    const float t =
        std::min(static_cast<float>(n) - 2.0f, 16.0f) / 16.0f;
    return 0.0005f + t * (0.003f - 0.0005f);
}

// Canonical geometry constants (unscaled logical pixels). Use these in every
// platform shell so the indicator is visually identical everywhere.
static constexpr float kInflightDotR     = 3.5f;  // center dot radius
static constexpr float kInflightOrbitR   = 6.0f;  // ring orbit radius
static constexpr float kInflightRingDotR = 1.0f;  // per-dot ring radius
// Widget/view size required to fit the indicator without clipping:
//   2 * (kInflightOrbitR + kInflightRingDotR) ≈ 14 px → use 16 px
static constexpr float kInflightViewSize = 16.0f;

// Draw the inflight status indicator: a filled center dot with an optional
// 8-dot orbiting ring. When show_ring is false (idle/green state) only the
// center dot is drawn and phase01 is ignored.
//
// All radii are in unscaled logical pixels; the canvas backend applies HiDPI
// scaling via its scale_factor().
inline void draw_inflight_indicator(Canvas& cv, Point center,
                                    float dot_r, float orbit_r,
                                    float ring_dot_r,
                                    Color dot_color, Color ring_color,
                                    float phase01, bool show_ring)
{
    if (show_ring)
        draw_spinner_dots(cv, center, phase01, orbit_r, ring_dot_r, ring_color);

    // Draw the center dot last so ring dots behind it are naturally occluded.
    cv.fill_rounded_rect({center.x - dot_r, center.y - dot_r,
                          dot_r * 2.0f, dot_r * 2.0f},
                         dot_r, dot_color);
}

} // namespace tk
