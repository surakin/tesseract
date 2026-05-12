#include "tk_test_surface.h"
#include "tk/canvas_d2d.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace {

// Offscreen tk::Canvas backed by a WIC bitmap + ID2D1BitmapRenderTarget.
// Mirrors the Qt / GTK test surfaces: BeginDraw is implicit, EndDraw is
// flushed lazily on the first read_pixel() call.
class D2DTestSurface : public TestSurface {
public:
    D2DTestSurface(int w, int h) : w_(w), h_(h) {
        auto f = tk::d2d::factories(backend_);

        // 32bppPBGRA matches the default D2D pixel format and lets the
        // bitmap render target draw directly into the WIC bitmap memory.
        f.wic->CreateBitmap(
            static_cast<UINT>(w), static_cast<UINT>(h),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad,
            wic_bitmap_.GetAddressOf());

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);

        f.d2d->CreateWicBitmapRenderTarget(
            wic_bitmap_.Get(), props, rt_.GetAddressOf());

        factory_ = tk::d2d::make_factory(backend_);
        canvas_  = tk::d2d::make_canvas(backend_, rt_.Get());

        rt_->BeginDraw();
        drawing_ = true;
    }

    ~D2DTestSurface() override { flush(); }

    tk::Canvas&        canvas()  override { return *canvas_;  }
    tk::CanvasFactory& factory() override { return *factory_; }

    tk::Color read_pixel(int x, int y) override {
        flush();
        WICRect rect{ x, y, 1, 1 };
        BYTE pixel[4]{};
        wic_bitmap_->CopyPixels(&rect, 4, sizeof(pixel), pixel);

        // 32bppPBGRA layout: pixel[0]=B, [1]=G, [2]=R, [3]=A (premultiplied).
        std::uint8_t a = pixel[3];
        if (a == 0) return tk::Color::rgba(0, 0, 0, 0);
        auto unmul = [a](std::uint8_t v) -> std::uint8_t {
            int r = (int(v) * 255 + a / 2) / a;
            return static_cast<std::uint8_t>(std::min(r, 255));
        };
        return tk::Color::rgba(unmul(pixel[2]),
                               unmul(pixel[1]),
                               unmul(pixel[0]),
                               a);
    }

    int width()  const override { return w_; }
    int height() const override { return h_; }

private:
    void flush() {
        if (!drawing_) return;
        rt_->EndDraw();
        drawing_ = false;
    }

    int                                 w_;
    int                                 h_;
    tk::d2d::Backend                    backend_;
    ComPtr<IWICBitmap>                  wic_bitmap_;
    ComPtr<ID2D1RenderTarget>           rt_;
    std::unique_ptr<tk::CanvasFactory>  factory_;
    std::unique_ptr<tk::Canvas>         canvas_;
    bool                                drawing_ = false;
};

} // namespace

std::unique_ptr<TestSurface> TestSurface::create(int w, int h) {
    return std::make_unique<D2DTestSurface>(w, h);
}
