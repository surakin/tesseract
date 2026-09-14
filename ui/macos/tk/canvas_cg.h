#pragma once

// CoreGraphics + CoreText implementation of tk::Canvas + tk::Image +
// tk::TextLayout + tk::CanvasFactory for the macOS AppKit app.
//
// CoreGraphics + CoreText are pure C APIs, so this lives in a .cpp file
// rather than .mm — no Objective-C required. NSView::drawRect: hands you
// a CGContextRef via [NSGraphicsContext currentContext].CGContext; wrap
// it in a Canvas for the duration of the paint pass.

#include "canvas.h"
#include "tk/anim_decode_session.h"

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
// `max_w`/`max_h` (0/0 = unbounded, the historical default) downscale each
// *animated* frame (independent axis limits — NOT a single square bound) to
// fit within that box via a second, offline CGContextDrawImage pass after
// the native-size thumbnail decode above — see scale_cgimage's doc comment
// in canvas_cg.cpp for why this isn't folded into the thumbnail-generator
// call itself. The still-image fallback is deliberately left undownscaled
// here (same native-size-thumbnail decode, no second pass) — same
// tradeoff the R/B-swap-avoidance design already makes for it elsewhere.
//
// `on_first_frame`/`on_extra_frame` (both null by default) stream frames out
// as they're decoded instead of collecting them into the returned
// DecodedFrames: when both are supplied, whichever frame is the first to
// actually decode (and downscale) successfully goes to `on_first_frame`
// regardless of its original position in the source, and every one after
// that to `on_extra_frame` (with a running delivery index) — and the
// returned DecodedFrames' `frames` stays empty; checking `delays_ms`/still
// emptiness is not a valid success signal in that mode, the boolean return
// callers build on top of this (see MacShell::decode_image_streamed_) is.
DecodedFrames decode_image_bytes(
    std::span<const std::uint8_t> bytes, int max_w = 0, int max_h = 0,
    const std::function<void(std::unique_ptr<Image>, int)>* on_first_frame =
        nullptr,
    const std::function<void(int, std::unique_ptr<Image>, int)>*
        on_extra_frame = nullptr);

// Windowed variant of decode_image_bytes' streaming mode: for animations
// with at most `window_threshold_frames` frames, behaves exactly like
// decode_image_bytes(..., &on_first_frame, &on_extra_frame) — decodes the
// whole thing via the callbacks, passing a null session. For longer
// animations, decodes only an initial window's worth of frames via the
// callbacks and passes `on_first_frame` a live tk::AnimDecodeSession, plus
// the animation's total frame count, so the caller can decode further
// frames on demand — see anim_decode_session.h — instead of the whole
// animation up front. The session owns its own copy of `bytes` so it stays
// valid for calls made long after this function returns and `bytes` may
// have been freed; unlike the GTK4/Qt6 windowed sessions (and Windows' WIC
// GIF compositor), it needs no other persistent decoder state — ImageIO's
// CGImageSourceRef is genuinely random-access (see decode_frame_at_index's
// doc comment in canvas_cg.cpp), so decoding any frame index at any time
// needs nothing but that retained byte copy.
//
// The session/total-frame-count are delivered as `on_first_frame`'s own
// arguments (not an out-param assigned after this function returns) so a
// caller that needs them for its own later-posted work can stash them
// synchronously, inside on_first_frame's own call — see
// ShellBase::decode_image_streamed_windowed_'s doc comment for the data
// race an out-param-based version of this contract had (confirmed real on
// the Windows backend).
//
// Returned DecodedFrames is always empty — success/failure is whether
// on_first_frame fired, exactly like decode_image_bytes' own streaming
// mode.
DecodedFrames decode_image_bytes_windowed(
    std::span<const std::uint8_t> bytes, int max_w, int max_h,
    int window_threshold_frames,
    const std::function<void(std::unique_ptr<Image>, int delay_ms,
                             std::shared_ptr<tk::AnimDecodeSession>,
                             std::size_t total_frames)>& on_first_frame,
    const std::function<void(int frame_index, std::unique_ptr<Image>,
                             int delay_ms)>& on_extra_frame);

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
