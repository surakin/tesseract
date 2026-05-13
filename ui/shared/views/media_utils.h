#pragma once
#include "tk/canvas.h"
#include <algorithm>

namespace tesseract::views {

// Scale `(natural_w, natural_h)` to fit within `(max_w, max_h)` while
// preserving aspect ratio and never upscaling beyond the natural size.
// Returns a reasonable fallback size when intrinsic dimensions are absent.
inline tk::Size fit_media(float natural_w, float natural_h,
                           float max_w,    float max_h) {
    if (natural_w <= 0 || natural_h <= 0) return { max_w, max_h * 0.5f };
    float sx = max_w / natural_w;
    float sy = max_h / natural_h;
    float s  = std::min({ sx, sy, 1.0f });
    return { natural_w * s, natural_h * s };
}

inline bool rect_contains(const tk::Rect& r, tk::Point p) {
    return p.x >= r.x && p.y >= r.y &&
           p.x <  r.x + r.w && p.y <  r.y + r.h;
}

} // namespace tesseract::views
