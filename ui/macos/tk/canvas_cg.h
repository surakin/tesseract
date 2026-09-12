#pragma once

// CoreGraphics + CoreText implementation of tk::Canvas + tk::Image +
// tk::TextLayout + tk::CanvasFactory for the macOS AppKit app.
//
// CoreGraphics + CoreText are pure C APIs, so this lives in a .cpp file
// rather than .mm — no Objective-C required. NSView::drawRect: hands you
// a CGContextRef via [NSGraphicsContext currentContext].CGContext; wrap
// it in a Canvas for the duration of the paint pass.

#include "canvas.h"

typedef struct CGContext* CGContextRef;
typedef struct CGImage* CGImageRef;

namespace tk::cg
{

// Wrap a borrowed CGContextRef for one paint pass. The caller's NSView
// should override `-isFlipped` to return YES so the origin is top-left
// (matching the rest of the toolkit). No extra flip is applied here.
//
// `scale_override`, when > 0, is what Canvas::scale_factor() reports instead
// of reading it back from the context's CTM. Needed for the live window
// surface: when an ancestor NSView is layer-backed (content.wantsLayer =
// YES, set for chrome elsewhere in MainWindowController.mm), every
// descendant view — including ours, even with its own wantsLayer left NO —
// is forced into inherited layer-backing too, and the CGContext handed to
// -drawRect: in that case has an unscaled, unflipped CTM (a=1, d=1)
// regardless of the screen's real backing scale. Vector/text drawing still
// rasterizes at full resolution regardless (CoreAnimation's backing store is
// the real 2x surface; the CTM reading is just a coordinate-space artifact
// of the inherited-layer path), so this was invisible everywhere *except*
// tk::render_pill_bitmap()'s offscreen bake, which explicitly sizes its own
// bitmap from scale_factor() — reading 1.0 here quietly baked mention pills
// at half resolution, then upscaled them onto the real 2x screen, which is
// what made them look pixelated next to the composer's pills (which size
// their own bake from window.backingScaleFactor directly, never through this
// path). Pass 0 (the default) for anything that isn't the live window
// surface — offscreen bake contexts (create_offscreen) already set up their
// own CTM scale deliberately and read it back correctly.
std::unique_ptr<Canvas> make_canvas(CGContextRef ctx, float scale_override = 0.0f);

std::unique_ptr<CanvasFactory> make_factory();

// Wrap an already-decoded CGImage as a tk::Image. The wrapper takes an
// extra retain on the image; release the returned unique_ptr to drop it.
// Use this when integration code has decoded media bytes via ImageIO,
// CIImage, or NSImage and wants to hand the result to the shared views
// (RoomListView avatar provider, MessageListView image provider) without
// re-decoding.
std::unique_ptr<Image> make_image(CGImageRef img);

struct DecodedFrames
{
    std::unique_ptr<Image> still;               // single-frame result
    std::vector<std::unique_ptr<Image>> frames; // animated result (>= 2)
    std::vector<int> delays_ms;                 // parallel to frames
};

// Decode raw image bytes via ImageIO/CGImageSource. Animated formats
// (WebP/GIF/APNG) get one entry per frame, each frame decoded from its own
// freshly-parsed CGImageSourceRef rather than reusing one CGImageSourceRef
// across the whole sequence: ImageIO's animated-format decoders keep
// internal state across sequential CGImageSourceCreateImageAtIndex calls on
// a shared source object, which has been observed to intermittently produce
// an R/B channel swap on alternating frames of animated WebP.
// Frames (and the still-image fallback) are decoded via
// CGImageSourceCreateThumbnailAtIndex rather than
// CGImageSourceCreateImageAtIndex, requesting the frame's own native pixel
// size — i.e. no actual downscaling happens here. CreateImageAtIndex's
// plain frame-extraction path hands back a CGImage that
// CGContextDrawImage's accelerated minification mishandles (an R/B swap)
// when later drawn into a small destination rect (e.g. the Room List
// last-message thumbnail, or a sticker-picker pack tab icon, as opposed to
// the Timeline or the sticker-picker grid tile); going through ImageIO's
// dedicated thumbnail generator instead avoids it.
// Safe to call from any thread (CGImageSource is thread-safe across
// independent source objects). This is the single implementation for both
// MacShell::decode_image_ (Room List, Timeline, avatars) and ComposeBar's
// attachment preview — do not reimplement this logic at a third call site.
DecodedFrames decode_image_bytes(std::span<const std::uint8_t> bytes);

// The reverse of make_image() — extract the underlying native bitmap from
// a tk::Image so it can be embedded into a platform-native rich-text
// control (e.g. an NSTextAttachment for an inline composer emoticon
// pill). Every backend exposes the same name, tk::<backend>::to_native_image,
// returning its own concretely-typed NativeImageHandle — host_macos.mm is
// the only caller, and only ever compiles this one backend's header.
// Borrowed — caller does not own the returned image.
using NativeImageHandle = CGImageRef;
NativeImageHandle to_native_image(const Image& img);

struct RealLineMetrics
{
    float ascent = 0.0f;
    float descent = 0.0f;
};

// The genuine ascent/descent split (points) for `role`'s font, via
// CTFontGetAscent/CTFontGetDescent directly. Unlike
// tk::role_line_metrics()/TextLayout::ascent() on this backend, which for
// a plain (non-elided) layout always reports the *full measured height* as
// ascent (see CTLayout::ascent()'s non-elided branch, kept so Apple Color
// Emoji — which fills the whole line box — centers correctly elsewhere),
// this gives the real per-font split, needed by anything that must know
// how much of a line's height sits below the baseline (e.g. reserving
// descent space for an inline attachment so it doesn't render flush with —
// or above — the baseline).
RealLineMetrics real_line_metrics(FontRole role);

} // namespace tk::cg
