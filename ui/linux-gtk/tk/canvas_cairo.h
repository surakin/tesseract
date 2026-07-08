#pragma once

// Cairo + Pango + GdkPixbuf implementation of tk::Canvas, tk::Image,
// tk::TextLayout, tk::CanvasFactory. Bind one Canvas to an externally
// owned cairo_t for each GtkDrawingArea::draw / GtkSnapshot frame; the
// factory is application-lifetime.

#include "canvas.h"

typedef struct _cairo cairo_t;
// Forward-declared at global scope (matching cairo_t above) so it binds to
// the same type as <cairo.h>'s own `typedef struct _cairo_surface
// cairo_surface_t;` once that header is included — an elaborated-type-
// specifier used only inside the tk::cairo_pango namespace below, before
// <cairo.h> has been seen, would otherwise implicitly declare a distinct,
// wrongly-scoped `tk::cairo_pango::_cairo_surface` tag instead.
typedef struct _cairo_surface cairo_surface_t;

namespace tk::cairo_pango
{

// Wrap a borrowed cairo_t for one paint pass. Caller owns the context.
std::unique_ptr<Canvas> make_canvas(cairo_t* cr);

// Build a factory using the default PangoCairoFontMap singleton. Image
// decode goes through GdkPixbufLoader.
std::unique_ptr<CanvasFactory> make_factory();

// Wrap an already-decoded ARGB32 image surface as a tk::Image. The
// wrapper takes ownership of the surface (a refcounted cairo_surface_t)
// and releases it on destruction. Use this when integration code has
// decoded image bytes once and wants to hand the result to the shared
// views without re-decoding on every paint.
std::unique_ptr<Image> make_image(struct _cairo_surface* surface);

// The reverse of make_image() — extract the underlying native bitmap from
// a tk::Image so it can be embedded into a platform-native rich-text
// control (e.g. a GtkPicture anchored via gtk_text_view_add_child_at_anchor
// for an inline composer emoticon pill). Every backend exposes the same
// name, tk::<backend>::to_native_image, returning its own
// concretely-typed NativeImageHandle — host_gtk.cpp is the only caller,
// and only ever compiles this one backend's header. Borrowed — caller
// does not own the returned surface.
using NativeImageHandle = cairo_surface_t*;
NativeImageHandle to_native_image(const Image& img);

} // namespace tk::cairo_pango
