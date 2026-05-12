#include "canvas_d2d.h"

#include <d2d1_1.h>
#include <dwrite_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tk::d2d {

namespace {

// ── Small helpers ─────────────────────────────────────────────────────────

inline D2D1_COLOR_F to_d2d(Color c) {
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                         c.a / 255.0f);
}

inline D2D1_RECT_F to_d2d(Rect r) {
    return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
}

inline D2D1_POINT_2F to_d2d(Point p) {
    return D2D1::Point2F(p.x, p.y);
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string("d2d: ") + what + " failed");
    }
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

// Extract the first 1–2 grapheme starts for an initials disc. UTF-8 aware
// at the byte level: a new character begins on any byte that is not a
// continuation byte (0b10xxxxxx). Two-letter initials are picked from
// the first word + the first letter of the next word; otherwise one
// letter from the first word.
std::wstring initials_of(std::string_view name) {
    std::wstring wide = utf8_to_wide(name);
    std::wstring out;
    bool at_word = true;
    for (wchar_t ch : wide) {
        if (ch == L' ' || ch == L'\t' || ch == L'\n') {
            at_word = true;
            continue;
        }
        if (at_word) {
            out.push_back(static_cast<wchar_t>(towupper(ch)));
            at_word = false;
            if (out.size() == 2) break;
        }
    }
    if (out.empty()) out = L"?";
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Font roles → DWrite text format
// ─────────────────────────────────────────────────────────────────────────
//
// Sizes follow client/include/tesseract/visual.h. Weight follows the
// "semibold for emphasis" rule in docs/UI-PARITY.md. The family is the
// modern Segoe UI Variable face on Win11; older systems fall back to
// "Segoe UI" via the system font fallback chain that DWrite already
// applies when the requested family is unavailable.

struct FontDesc {
    const wchar_t*       family;
    float                size_pt;
    DWRITE_FONT_WEIGHT   weight;
};

static FontDesc desc_for(FontRole r) {
    switch (r) {
        case FontRole::Small:           return { L"Segoe UI Variable Text",     9.0f, DWRITE_FONT_WEIGHT_REGULAR  };
        case FontRole::Body:            return { L"Segoe UI Variable Text",    13.0f, DWRITE_FONT_WEIGHT_REGULAR  };
        case FontRole::SenderName:      return { L"Segoe UI Variable Text",    12.0f, DWRITE_FONT_WEIGHT_SEMIBOLD };
        case FontRole::Timestamp:       return { L"Segoe UI Variable Text",    10.0f, DWRITE_FONT_WEIGHT_REGULAR  };
        case FontRole::SidebarName:     return { L"Segoe UI Variable Text",    13.0f, DWRITE_FONT_WEIGHT_SEMIBOLD };
        case FontRole::SidebarPreview:  return { L"Segoe UI Variable Text",    11.0f, DWRITE_FONT_WEIGHT_REGULAR  };
        case FontRole::UnreadBadge:     return { L"Segoe UI Variable Text",    11.0f, DWRITE_FONT_WEIGHT_SEMIBOLD };
        case FontRole::Title:           return { L"Segoe UI Variable Display", 15.0f, DWRITE_FONT_WEIGHT_SEMIBOLD };
        case FontRole::UiSemibold:      return { L"Segoe UI Variable Text",    11.0f, DWRITE_FONT_WEIGHT_SEMIBOLD };
    }
    return { L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_REGULAR };
}

// ─────────────────────────────────────────────────────────────────────────
//  Backend::Impl — D2D + DWrite + WIC singletons
// ─────────────────────────────────────────────────────────────────────────

struct Backend::Impl {
    ComPtr<ID2D1Factory1>     d2d;
    ComPtr<IDWriteFactory2>   dwrite;
    ComPtr<IWICImagingFactory> wic;
    bool                      com_initialised_here = false;

    // IDWriteTextFormat is cheap to keep around; one per FontRole.
    std::unordered_map<int, ComPtr<IDWriteTextFormat>> text_formats;

    IDWriteTextFormat* text_format_for(FontRole role) {
        auto it = text_formats.find(static_cast<int>(role));
        if (it != text_formats.end()) return it->second.Get();

        FontDesc fd = desc_for(role);
        ComPtr<IDWriteTextFormat> tf;
        // 1 pt = 1/72 inch; DWrite measures size in DIPs (1 DIP = 1/96 inch),
        // so multiply by 96/72 = 4/3 to convert.
        float size_dip = fd.size_pt * (96.0f / 72.0f);
        HRESULT hr = dwrite->CreateTextFormat(
            fd.family, nullptr,
            fd.weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size_dip, L"",
            tf.GetAddressOf());
        check(hr, "CreateTextFormat");
        text_formats.emplace(static_cast<int>(role), tf);
        return tf.Get();
    }
};

Backend::Backend() : impl_(std::make_unique<Impl>()) {
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr_co)) {
        impl_->com_initialised_here = true;
    } else if (hr_co != RPC_E_CHANGED_MODE && hr_co != S_FALSE) {
        check(hr_co, "CoInitializeEx");
    }

    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    __uuidof(ID2D1Factory1),
                                    &opts,
                                    reinterpret_cast<void**>(impl_->d2d.GetAddressOf()));
    check(hr, "D2D1CreateFactory");

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                              __uuidof(IDWriteFactory2),
                              reinterpret_cast<IUnknown**>(impl_->dwrite.GetAddressOf()));
    check(hr, "DWriteCreateFactory");

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(impl_->wic.GetAddressOf()));
    check(hr, "WICImagingFactory");
}

Backend::~Backend() {
    impl_->text_formats.clear();
    impl_->wic.Reset();
    impl_->dwrite.Reset();
    impl_->d2d.Reset();
    if (impl_->com_initialised_here) {
        CoUninitialize();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  D2DImage — tk::Image
// ─────────────────────────────────────────────────────────────────────────

class D2DImage : public Image {
public:
    D2DImage(ComPtr<IWICFormatConverter> source, int w, int h)
        : source_(std::move(source)), width_(w), height_(h) {}

    int width()  const override { return width_; }
    int height() const override { return height_; }

    // ID2D1Bitmap is bound to a particular ID2D1RenderTarget. We keep the
    // WIC-decoded frame as the source of truth and lazily construct the
    // D2D bitmap for the render target that draws us. The cache is
    // invalidated when the render target is recreated (lost device).
    ID2D1Bitmap* bitmap_for(ID2D1RenderTarget* rt) {
        if (bitmap_ && rt_cached_ == rt) return bitmap_.Get();
        bitmap_.Reset();
        rt_cached_ = nullptr;
        HRESULT hr = rt->CreateBitmapFromWicBitmap(source_.Get(), nullptr,
                                                    bitmap_.GetAddressOf());
        if (FAILED(hr)) return nullptr;
        rt_cached_ = rt;
        return bitmap_.Get();
    }

    void release_device_resources() {
        bitmap_.Reset();
        rt_cached_ = nullptr;
    }

private:
    ComPtr<IWICFormatConverter> source_;
    int                         width_;
    int                         height_;
    ComPtr<ID2D1Bitmap>         bitmap_;
    ID2D1RenderTarget*          rt_cached_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────
//  DWriteLayout — tk::TextLayout
// ─────────────────────────────────────────────────────────────────────────

class DWriteLayout : public TextLayout {
public:
    DWriteLayout(ComPtr<IDWriteTextLayout> layout)
        : layout_(std::move(layout)) {
        DWRITE_TEXT_METRICS m{};
        layout_->GetMetrics(&m);
        size_       = Size{ m.widthIncludingTrailingWhitespace, m.height };
        line_count_ = static_cast<int>(m.lineCount);
    }

    Size measure()    const override { return size_; }
    int  line_count() const override { return line_count_; }

    IDWriteTextLayout* raw() const { return layout_.Get(); }

private:
    ComPtr<IDWriteTextLayout> layout_;
    Size                      size_{};
    int                       line_count_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  D2DCanvas — tk::Canvas implementation
// ─────────────────────────────────────────────────────────────────────────

class D2DCanvas : public Canvas {
public:
    D2DCanvas(Backend::Impl& backend, ID2D1HwndRenderTarget* rt)
        : backend_(backend), rt_(rt) {}

    void rebind(ID2D1HwndRenderTarget* rt) {
        rt_ = rt;
        brush_cache_.clear();
    }

    void clear(Color c) override { rt_->Clear(to_d2d(c)); }

    void fill_rect(Rect r, Color c) override {
        rt_->FillRectangle(to_d2d(r), brush(c));
    }

    void fill_rounded_rect(Rect r, float radius, Color c) override {
        D2D1_ROUNDED_RECT rr{ to_d2d(r), radius, radius };
        rt_->FillRoundedRectangle(rr, brush(c));
    }

    void stroke_rect(Rect r, Color c, float width) override {
        // D2D strokes are centred on the path: inset by half the width so
        // a 1 px stroke at (0,0,w,h) lands inside the rect.
        D2D1_RECT_F dr = to_d2d(r);
        float half = width * 0.5f;
        dr.left   += half; dr.top    += half;
        dr.right  -= half; dr.bottom -= half;
        rt_->DrawRectangle(dr, brush(c), width);
    }

    void stroke_rounded_rect(Rect r, float radius, Color c,
                              float width) override {
        D2D1_RECT_F dr = to_d2d(r);
        float half = width * 0.5f;
        dr.left   += half; dr.top    += half;
        dr.right  -= half; dr.bottom -= half;
        D2D1_ROUNDED_RECT rr{ dr, radius, radius };
        rt_->DrawRoundedRectangle(rr, brush(c), width);
    }

    void draw_image(const Image& image, Rect dst) override {
        ID2D1Bitmap* bmp =
            const_cast<D2DImage&>(static_cast<const D2DImage&>(image))
                .bitmap_for(rt_);
        if (!bmp) return;
        rt_->DrawBitmap(bmp, to_d2d(dst), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    void draw_image_subregion(const Image& image, Rect src,
                               Rect dst) override {
        ID2D1Bitmap* bmp =
            const_cast<D2DImage&>(static_cast<const D2DImage&>(image))
                .bitmap_for(rt_);
        if (!bmp) return;
        D2D1_RECT_F srcr = to_d2d(src);
        rt_->DrawBitmap(bmp, to_d2d(dst), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcr);
    }

    void draw_circle_image(const Image& image, Point centre,
                            float diameter) override {
        ID2D1Bitmap* bmp =
            const_cast<D2DImage&>(static_cast<const D2DImage&>(image))
                .bitmap_for(rt_);
        if (!bmp) return;

        ComPtr<ID2D1EllipseGeometry> ellipse;
        D2D1_ELLIPSE e = D2D1::Ellipse(to_d2d(centre),
                                        diameter * 0.5f,
                                        diameter * 0.5f);
        backend_.d2d->CreateEllipseGeometry(e, ellipse.GetAddressOf());
        if (!ellipse) return;

        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
            D2D1::InfiniteRect(),
            ellipse.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        rt_->PushLayer(lp, nullptr);
        Rect dst{ centre.x - diameter * 0.5f,
                  centre.y - diameter * 0.5f,
                  diameter, diameter };
        rt_->DrawBitmap(bmp, to_d2d(dst), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        rt_->PopLayer();
    }

    void draw_initials_circle(std::string_view name, Point centre,
                               float diameter, Color bg,
                               Color fg) override {
        D2D1_ELLIPSE e = D2D1::Ellipse(to_d2d(centre),
                                        diameter * 0.5f,
                                        diameter * 0.5f);
        rt_->FillEllipse(e, brush(bg));

        std::wstring initials = initials_of(name);
        // Pick a font size proportional to the diameter — matches the
        // GDI+ initials-disc code that used to live in MainWindow.cpp.
        float font_dip = diameter * 0.42f;
        ComPtr<IDWriteTextFormat> tf;
        HRESULT hr = backend_.dwrite->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMIBOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, font_dip, L"",
            tf.GetAddressOf());
        if (FAILED(hr)) return;
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        D2D1_RECT_F r = D2D1::RectF(centre.x - diameter * 0.5f,
                                     centre.y - diameter * 0.5f,
                                     centre.x + diameter * 0.5f,
                                     centre.y + diameter * 0.5f);
        rt_->DrawText(initials.c_str(),
                      static_cast<UINT32>(initials.size()),
                      tf.Get(), r, brush(fg),
                      D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    void draw_text(const TextLayout& layout, Point origin,
                    Color c) override {
        auto& dl = static_cast<const DWriteLayout&>(layout);
        // D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT is the reason we use
        // D2D + DWrite instead of GDI/GDI+. Without this flag, emoji
        // render as monochrome outlines from the COLR base layer.
        rt_->DrawTextLayout(to_d2d(origin), dl.raw(), brush(c),
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    void push_clip_rect(Rect r) override {
        rt_->PushAxisAlignedClip(to_d2d(r),
                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        clip_stack_.push_back(ClipKind::AxisAligned);
    }

    void push_clip_rounded_rect(Rect r, float radius) override {
        ComPtr<ID2D1RoundedRectangleGeometry> geom;
        D2D1_ROUNDED_RECT rr{ to_d2d(r), radius, radius };
        backend_.d2d->CreateRoundedRectangleGeometry(rr,
                                                      geom.GetAddressOf());
        if (!geom) {
            // Degrade to axis-aligned clip so the stack stays balanced.
            rt_->PushAxisAlignedClip(to_d2d(r),
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            clip_stack_.push_back(ClipKind::AxisAligned);
            return;
        }
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
            D2D1::InfiniteRect(), geom.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt_->PushLayer(lp, nullptr);
        clip_stack_.push_back(ClipKind::Layer);
    }

    void pop_clip() override {
        if (clip_stack_.empty()) return;
        ClipKind k = clip_stack_.back();
        clip_stack_.pop_back();
        if (k == ClipKind::AxisAligned) rt_->PopAxisAlignedClip();
        else                            rt_->PopLayer();
    }

    float scale_factor() const override {
        float dpi_x = 96.0f, dpi_y = 96.0f;
        rt_->GetDpi(&dpi_x, &dpi_y);
        return dpi_x / 96.0f;
    }

private:
    enum class ClipKind { AxisAligned, Layer };

    ID2D1SolidColorBrush* brush(Color c) {
        std::uint32_t key = (std::uint32_t(c.r) << 24) |
                            (std::uint32_t(c.g) << 16) |
                            (std::uint32_t(c.b) <<  8) |
                             std::uint32_t(c.a);
        auto it = brush_cache_.find(key);
        if (it != brush_cache_.end()) return it->second.Get();
        ComPtr<ID2D1SolidColorBrush> b;
        rt_->CreateSolidColorBrush(to_d2d(c), b.GetAddressOf());
        ID2D1SolidColorBrush* raw = b.Get();
        brush_cache_.emplace(key, std::move(b));
        return raw;
    }

    Backend::Impl&         backend_;
    ID2D1HwndRenderTarget* rt_;
    std::unordered_map<std::uint32_t, ComPtr<ID2D1SolidColorBrush>>
                           brush_cache_;
    std::vector<ClipKind>  clip_stack_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Surface::Impl
// ─────────────────────────────────────────────────────────────────────────

struct Surface::Impl {
    Backend::Impl&                  backend;
    HWND                            hwnd;
    ComPtr<ID2D1HwndRenderTarget>   rt;
    std::unique_ptr<D2DCanvas>      canvas;
    bool                            painting = false;

    Impl(Backend::Impl& b, HWND h) : backend(b), hwnd(h) {}

    void ensure_target() {
        if (rt) return;
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(rc.right  - rc.left, 1)),
            static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top,  1)));
        D2D1_RENDER_TARGET_PROPERTIES rt_props =
            D2D1::RenderTargetProperties();
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
            D2D1::HwndRenderTargetProperties(hwnd, size,
                                              D2D1_PRESENT_OPTIONS_NONE);
        HRESULT hr = backend.d2d->CreateHwndRenderTarget(
            rt_props, hwnd_props, rt.GetAddressOf());
        check(hr, "CreateHwndRenderTarget");

        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0) dpi = 96;
        rt->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));

        canvas = std::make_unique<D2DCanvas>(backend, rt.Get());
    }

    void resize(int w, int h) {
        if (!rt) return;
        D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(std::max(w, 1)),
            static_cast<UINT32>(std::max(h, 1)));
        rt->Resize(size);
    }

    void drop_target() {
        canvas.reset();
        rt.Reset();
    }
};

Surface::Surface(Backend& b, HWND h)
    : impl_(std::make_unique<Impl>(b.impl(), h)) {}

Surface::~Surface() = default;

void Surface::resize(int w, int h) { impl_->resize(w, h); }

Canvas& Surface::begin_paint() {
    impl_->ensure_target();
    impl_->rt->BeginDraw();
    impl_->painting = true;
    impl_->canvas->rebind(impl_->rt.Get());
    return *impl_->canvas;
}

bool Surface::end_paint() {
    if (!impl_->painting) return false;
    impl_->painting = false;
    HRESULT hr = impl_->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        impl_->drop_target();
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────
//  Factory
// ─────────────────────────────────────────────────────────────────────────

class D2DFactory : public CanvasFactory {
public:
    explicit D2DFactory(Backend& b) : backend_(b.impl()) {}

    std::unique_ptr<Image>
    decode_image(std::span<const std::uint8_t> bytes) override {
        if (bytes.empty()) return nullptr;

        ComPtr<IWICStream> stream;
        if (FAILED(backend_.wic->CreateStream(stream.GetAddressOf())))
            return nullptr;
        if (FAILED(stream->InitializeFromMemory(
                const_cast<BYTE*>(bytes.data()),
                static_cast<DWORD>(bytes.size()))))
            return nullptr;

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(backend_.wic->CreateDecoderFromStream(
                stream.Get(), nullptr,
                WICDecodeMetadataCacheOnLoad,
                decoder.GetAddressOf())))
            return nullptr;

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
            return nullptr;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(backend_.wic->CreateFormatConverter(
                converter.GetAddressOf())))
            return nullptr;
        if (FAILED(converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0f,
                WICBitmapPaletteTypeMedianCut)))
            return nullptr;

        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        return std::make_unique<D2DImage>(std::move(converter),
                                          static_cast<int>(w),
                                          static_cast<int>(h));
    }

    std::unique_ptr<TextLayout>
    build_text(std::string_view utf8, const TextStyle& s) override {
        std::wstring wide = utf8_to_wide(utf8);
        IDWriteTextFormat* tf = backend_.text_format_for(s.role);
        if (!tf) return nullptr;

        float max_w = s.max_width  >= 0 ? s.max_width  : 8192.0f;
        float max_h = s.max_height >= 0 ? s.max_height : 8192.0f;

        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = backend_.dwrite->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()),
            tf, max_w, max_h, layout.GetAddressOf());
        if (FAILED(hr) || !layout) return nullptr;

        DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING;
        switch (s.halign) {
            case TextHAlign::Leading:  a = DWRITE_TEXT_ALIGNMENT_LEADING;  break;
            case TextHAlign::Center:   a = DWRITE_TEXT_ALIGNMENT_CENTER;   break;
            case TextHAlign::Trailing: a = DWRITE_TEXT_ALIGNMENT_TRAILING; break;
        }
        layout->SetTextAlignment(a);

        DWRITE_PARAGRAPH_ALIGNMENT pa = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        switch (s.valign) {
            case TextVAlign::Top:    pa = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;   break;
            case TextVAlign::Center: pa = DWRITE_PARAGRAPH_ALIGNMENT_CENTER; break;
            case TextVAlign::Bottom: pa = DWRITE_PARAGRAPH_ALIGNMENT_FAR;    break;
        }
        layout->SetParagraphAlignment(pa);

        layout->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP
                                       : DWRITE_WORD_WRAPPING_NO_WRAP);

        if (s.trim == TextTrim::Ellipsis) {
            DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER,
                                0, 0 };
            ComPtr<IDWriteInlineObject> ellipsis_sign;
            backend_.dwrite->CreateEllipsisTrimmingSign(
                tf, ellipsis_sign.GetAddressOf());
            layout->SetTrimming(&tr, ellipsis_sign.Get());
        }

        return std::make_unique<DWriteLayout>(std::move(layout));
    }

private:
    Backend::Impl& backend_;
};

std::unique_ptr<CanvasFactory> make_factory(Backend& b) {
    return std::make_unique<D2DFactory>(b);
}

} // namespace tk::d2d
