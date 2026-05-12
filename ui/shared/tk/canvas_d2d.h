#pragma once

// Direct2D + DirectWrite + WIC implementation of tk::Canvas, tk::Image,
// tk::TextLayout, and tk::CanvasFactory.
//
// Why D2D/DWrite (not GDI+):
//   - DWrite reads the OpenType COLR/CPAL tables in Segoe UI Emoji, so emoji
//     render as their colored layered glyphs rather than monochrome outlines.
//   - D2D paints into the same HWND surface that the rest of the app uses,
//     gets HiDPI for free via SetDpi(), and supports rounded clips natively.

#include "canvas.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <memory>

namespace tk::d2d {

// One per application. Owns the D2D, DWrite, and WIC factory singletons.
class Backend {
public:
    Backend();
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    struct Impl;
    Impl& impl() { return *impl_; }
    const Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// One per HWND. Owns the ID2D1HwndRenderTarget. Lost-device retry is
// handled transparently: when EndDraw returns D2DERR_RECREATE_TARGET the
// next begin_paint() drops and rebuilds the target. The window should
// InvalidateRect() once after a recreate to repaint with the new target.
class Surface {
public:
    Surface(Backend&, HWND);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // Tell the surface the window was resized. Call from WM_SIZE.
    void resize(int width_px, int height_px);

    // Begin a paint frame inside WM_PAINT. Returns the Canvas to draw
    // into; valid only until the matching end_paint() call.
    Canvas& begin_paint();

    // Commit the frame. Returns true if the device was lost and the
    // window should InvalidateRect to repaint with the new target.
    bool end_paint();

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// CanvasFactory backed by a Backend. Use one for the application lifetime
// to build avatars + text layouts that outlive any single paint.
std::unique_ptr<CanvasFactory> make_factory(Backend&);

} // namespace tk::d2d
