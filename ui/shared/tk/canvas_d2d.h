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
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <span>
#include <vector>
#include <cstdint>

struct ID2D1Device;
struct ID2D1Factory1;
struct ID2D1RenderTarget;
struct ID3D11Device;
struct IDWriteFactory2;
struct IDWriteFontFallback;
struct IDWriteFontFace;
struct IWICImagingFactory;

namespace tk::d2d
{

// One per application. Owns the D2D, DWrite, and WIC factory singletons.
class Backend
{
public:
    Backend();
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    struct Impl;
    Impl& impl()
    {
        return *impl_;
    }
    const Impl& impl() const
    {
        return *impl_;
    }

private:
    std::unique_ptr<Impl> impl_;
};

// One per HWND. Owns a DXGI flip-model swap chain + ID2D1DeviceContext.
// Presents via DWM's compositor for smooth, tear-free rendering. Lost-device
// retry is handled transparently: when EndDraw or Present signals a lost
// device the next begin_paint() drops and rebuilds the target. The window
// should InvalidateRect() once after a recreate to repaint.
//
// When transparent=true the swap chain uses DXGI_ALPHA_MODE_PREMULTIPLIED so
// DWM composites the window's per-pixel alpha channel against the content
// behind it. The HWND must have WS_EX_NOREDIRECTIONBITMAP. The caller is
// responsible for clearing each frame to {0,0,0,0} rather than an opaque bg.
class Surface
{
public:
    Surface(Backend&, HWND, bool transparent = false);
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
    Impl& impl()
    {
        return *impl_;
    }

private:
    std::unique_ptr<Impl> impl_;
};

// CanvasFactory backed by a Backend. Use one for the application lifetime
// to build avatars + text layouts that outlive any single paint.
std::unique_ptr<CanvasFactory> make_factory(Backend&);

// Wrap a borrowed ID2D1RenderTarget as a tk::Canvas. Caller owns the
// render target and is responsible for BeginDraw/EndDraw. The Canvas
// holds the raw pointer, so the render target must outlive it.
std::unique_ptr<Canvas> make_canvas(Backend&, ID2D1RenderTarget*);

// Direct access to the underlying D2D / DWrite / WIC factories owned by
// the Backend. Intended for callers that need to construct their own
// render target (e.g. tests using CreateWicBitmapRenderTarget).
struct Factories
{
    ID2D1Factory1*    d2d;
    IDWriteFactory2*  dwrite;
    IWICImagingFactory* wic;
    IDWriteFontFallback* font_fallback;
    // Noto Color Emoji IDWriteFontFace for IProvideFontInfo injection.
    // Null when the embedded font resource is absent or loading failed.
    IDWriteFontFace*  noto_emoji_face;
    // D2D/D3D devices for creating ID2D1DeviceContext-backed surfaces.
    // Null until the first window is created (device is lazy-initialised).
    ID2D1Device*      d2d_device;
    ID3D11Device*     d3d_device;
};
Factories factories(Backend&);

// One frame of a decoded multi-frame image. `delay_ms` is the trailing
// delay (how long to show this frame before advancing to the next),
// clamped to >= 20 ms so old GIFs encoded with 0 ms don't burn CPU.
struct AnimatedFrame
{
    std::unique_ptr<Image> image;
    int delay_ms;
};

// Decode a single-frame encoded image (PNG/JPEG/WebP/…) into a tk::Image.
// Pure WIC (free-threaded): callable off the UI thread; the resulting
// D2DImage holds a device-independent IWICBitmap and uploads at paint.
// Returns nullptr on failure / on a multi-frame image.
std::unique_ptr<Image> decode_image(Backend& backend,
                                    std::span<const std::uint8_t> bytes);

// Create a tk::Image from a raw BGRA pixel buffer (4 bytes/pixel, row-major,
// no padding required). Pixels are copied into an `IWICBitmap` so the
// resulting Image outlives `pixels`. Returns nullptr on failure.
std::unique_ptr<Image> make_image_from_bgra(Backend& backend,
                                            const std::uint8_t* pixels, int w,
                                            int h);

// Decode a multi-frame image (GIF natively, APNG on Win10 1809+,
// animated WebP if the Microsoft Store WebP Image Extension is
// installed) into a list of frames + per-frame delays. Returns an
// empty vector when the bytes contain a single-frame image, the codec
// is not present, or decoding fails. Frames are fully decoded into
// in-memory IWICBitmaps so the result survives the input span.
std::vector<AnimatedFrame>
decode_animation(Backend&, std::span<const std::uint8_t> bytes);

} // namespace tk::d2d
