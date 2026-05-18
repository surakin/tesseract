#pragma once

// Cairo + Pango + GdkPixbuf implementation of tk::Canvas, tk::Image,
// tk::TextLayout, tk::CanvasFactory. Bind one Canvas to an externally
// owned cairo_t for each GtkDrawingArea::draw / GtkSnapshot frame; the
// factory is application-lifetime.

#include "canvas.h"

typedef struct _cairo cairo_t;

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

} // namespace tk::cairo_pango
