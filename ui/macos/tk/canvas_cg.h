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
std::unique_ptr<Canvas> make_canvas(CGContextRef ctx);

std::unique_ptr<CanvasFactory> make_factory();

// Wrap an already-decoded CGImage as a tk::Image. The wrapper takes an
// extra retain on the image; release the returned unique_ptr to drop it.
// Use this when integration code has decoded media bytes via ImageIO,
// CIImage, or NSImage and wants to hand the result to the shared views
// (RoomListView avatar provider, MessageListView image provider) without
// re-decoding.
std::unique_ptr<Image> make_image(CGImageRef img);

// The reverse of make_image() — extract the underlying native bitmap from
// a tk::Image so it can be embedded into a platform-native rich-text
// control (e.g. an NSTextAttachment for an inline composer emoticon
// pill). Every backend exposes the same name, tk::<backend>::to_native_image,
// returning its own concretely-typed NativeImageHandle — host_macos.mm is
// the only caller, and only ever compiles this one backend's header.
// Borrowed — caller does not own the returned image.
using NativeImageHandle = CGImageRef;
NativeImageHandle to_native_image(const Image& img);

} // namespace tk::cg
