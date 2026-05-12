#include "tk_test_surface.h"
#include "tk/canvas_cairo.h"

#include <cairo.h>

#include <cstdint>
#include <memory>

namespace {

class GtkTestSurface : public TestSurface {
public:
    GtkTestSurface(int w, int h)
        : surface_(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h)),
          cr_(cairo_create(surface_)),
          factory_(tk::cairo_pango::make_factory()),
          canvas_(tk::cairo_pango::make_canvas(cr_)) {
        // Default-fill the offscreen surface white so tests have a
        // predictable background. Use SOURCE so we don't blend with the
        // transparent initial state.
        cairo_save(cr_);
        cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgb(cr_, 1.0, 1.0, 1.0);
        cairo_paint(cr_);
        cairo_restore(cr_);
    }
    ~GtkTestSurface() override {
        cairo_destroy(cr_);
        cairo_surface_destroy(surface_);
    }

    tk::Canvas&        canvas()  override { return *canvas_;  }
    tk::CanvasFactory& factory() override { return *factory_; }

    tk::Color read_pixel(int x, int y) override {
        cairo_surface_flush(surface_);
        const unsigned char* data =
            cairo_image_surface_get_data(surface_);
        int stride = cairo_image_surface_get_stride(surface_);
        const unsigned char* px = data + y * stride + x * 4;
        // CAIRO_FORMAT_ARGB32 is premultiplied alpha, native byte order
        // (BGRA on little-endian, the only platform we ship on for GTK).
        std::uint8_t b = px[0];
        std::uint8_t g = px[1];
        std::uint8_t r = px[2];
        std::uint8_t a = px[3];
        if (a == 0) return tk::Color::rgba(0, 0, 0, 0);
        // Un-premultiply so the caller sees straight RGB.
        auto undo = [&](std::uint8_t c) -> std::uint8_t {
            unsigned v = (static_cast<unsigned>(c) * 255 + a / 2) / a;
            return static_cast<std::uint8_t>(std::min(v, 255u));
        };
        return tk::Color::rgba(undo(r), undo(g), undo(b), a);
    }

    int width()  const override {
        return cairo_image_surface_get_width(surface_);
    }
    int height() const override {
        return cairo_image_surface_get_height(surface_);
    }

private:
    cairo_surface_t*                   surface_;
    cairo_t*                            cr_;
    std::unique_ptr<tk::CanvasFactory>  factory_;
    std::unique_ptr<tk::Canvas>         canvas_;
};

} // namespace

std::unique_ptr<TestSurface> TestSurface::create(int w, int h) {
    return std::make_unique<GtkTestSurface>(w, h);
}
