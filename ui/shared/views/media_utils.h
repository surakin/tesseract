#pragma once
#include "tk/canvas.h"
#include <algorithm>

namespace tesseract::views
{

// Scale `(natural_w, natural_h)` to fit within `(max_w, max_h)` while
// preserving aspect ratio and never upscaling beyond the natural size.
// Returns a reasonable fallback size when intrinsic dimensions are absent.
inline tk::Size fit_media(float natural_w, float natural_h, float max_w,
                          float max_h)
{
    if (natural_w <= 0 || natural_h <= 0)
    {
        return {max_w, max_h * 0.5f};
    }
    float sx = max_w / natural_w;
    float sy = max_h / natural_h;
    float s = std::min({sx, sy, 1.0f});
    return {natural_w * s, natural_h * s};
}

inline bool rect_contains(const tk::Rect& r, tk::Point p)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

// Draw a circular avatar into the disc centred at `centre` with the given
// `diameter`. When `img` is non-null it is centre-fit and clipped to the
// circle (exactly as `Canvas::draw_circle_image`); otherwise an initials
// disc is drawn from `initials_name` with the supplied background/foreground
// colours (exactly as `Canvas::draw_initials_circle`).
//
// Image resolution stays at the call site because it diverges widely
// (lazy-fetch on miss, MSC4278 invite-avatar gating, presence lookups, the
// name source picked for the initials fallback). This helper centralises only
// the invariant image-or-initials draw that was copy-pasted across ~10 views.
inline void draw_avatar(tk::Canvas& canvas, const tk::Image* img,
                        tk::Point centre, float diameter,
                        std::string_view initials_name, tk::Color initials_bg,
                        tk::Color initials_fg)
{
    if (img)
    {
        canvas.draw_circle_image(*img, centre, diameter);
    }
    else
    {
        canvas.draw_initials_circle(initials_name, centre, diameter,
                                    initials_bg, initials_fg);
    }
}

} // namespace tesseract::views
