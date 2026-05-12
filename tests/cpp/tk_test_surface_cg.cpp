#include "tk_test_surface.h"
#include "tk/canvas_cg.h"

#include <CoreGraphics/CoreGraphics.h>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace {

class CgTestSurface : public TestSurface {
public:
    CgTestSurface(int w, int h) : w_(w), h_(h) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        // kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast = RGBA
        ctx_ = CGBitmapContextCreate(nullptr, w, h, 8,
                                     static_cast<size_t>(w) * 4, cs,
                                     kCGBitmapByteOrder32Big |
                                     kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(cs);
        if (!ctx_)
            throw std::runtime_error("CGBitmapContextCreate failed");

        // Flip so origin is top-left, matching the rest of canvas_cg.
        CGContextTranslateCTM(ctx_, 0, h);
        CGContextScaleCTM(ctx_, 1, -1);

        // White fill so tests have a predictable background.
        CGContextSetRGBFillColor(ctx_, 1, 1, 1, 1);
        CGContextFillRect(ctx_, CGRectMake(0, 0, w, h));

        factory_ = tk::cg::make_factory();
        canvas_  = tk::cg::make_canvas(ctx_);
    }

    ~CgTestSurface() override {
        canvas_.reset();
        CGContextRelease(ctx_);
    }

    tk::Canvas&        canvas()  override { return *canvas_;  }
    tk::CanvasFactory& factory() override { return *factory_; }

    tk::Color read_pixel(int x, int y) override {
        // Re-wrap the context after each draw so the CTM is still correct.
        const auto* data = static_cast<const std::uint8_t*>(
            CGBitmapContextGetData(ctx_));
        size_t stride = CGBitmapContextGetBytesPerRow(ctx_);
        // Bitmap is stored top-down (we flipped the CTM at construction).
        const std::uint8_t* px = data + static_cast<size_t>(y) * stride
                                      + static_cast<size_t>(x) * 4;
        std::uint8_t r = px[0];
        std::uint8_t g = px[1];
        std::uint8_t b = px[2];
        std::uint8_t a = px[3];
        if (a == 0) return tk::Color::rgba(0, 0, 0, 0);
        // Un-premultiply.
        auto undo = [&](std::uint8_t c) -> std::uint8_t {
            unsigned v = (static_cast<unsigned>(c) * 255u + a / 2u) / a;
            return static_cast<std::uint8_t>(v < 255u ? v : 255u);
        };
        return tk::Color::rgba(undo(r), undo(g), undo(b), a);
    }

    int width()  const override { return w_; }
    int height() const override { return h_; }

private:
    int                                w_, h_;
    CGContextRef                       ctx_;
    std::unique_ptr<tk::CanvasFactory> factory_;
    std::unique_ptr<tk::Canvas>        canvas_;
};

} // namespace

std::unique_ptr<TestSurface> TestSurface::create(int w, int h) {
    return std::make_unique<CgTestSurface>(w, h);
}
