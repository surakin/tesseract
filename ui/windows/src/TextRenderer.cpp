#include "TextRenderer.h"

#include <d2d1_1.h>
#include <dwrite_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace win32::text {
namespace {

ComPtr<ID2D1Factory1>       g_d2d;
ComPtr<IDWriteFactory2>     g_dw;
ComPtr<IDWriteFontFallback> g_font_fallback;
ComPtr<ID2D1DCRenderTarget> g_rt;
UINT                        g_dpi = 96;

struct FormatKey {
    std::wstring family;
    int    size_x100;   // DIP * 100, integer for stable comparison
    Weight weight;
    Slant  slant;
    HAlign halign;
    VAlign valign;
    Trim   trim;
    Wrap   wrap;
    bool operator==(const FormatKey& o) const noexcept {
        return size_x100 == o.size_x100
            && weight == o.weight && slant == o.slant
            && halign == o.halign && valign == o.valign
            && trim   == o.trim   && wrap  == o.wrap
            && family == o.family;
    }
};

std::vector<std::pair<FormatKey, ComPtr<IDWriteTextFormat>>> g_format_cache;
std::vector<std::pair<DWORD, ComPtr<ID2D1SolidColorBrush>>> g_brush_cache;

float dip_from_style(const Style& s) {
    if (s.unit == SizeUnit::Pixel) {
        // GDI+ UnitPixel semantics: physical pixels, DPI-independent.
        // Convert to DIPs through the active DPI so the RT scales it back
        // to the same physical pixel count.
        return s.size * 96.0f / static_cast<float>(g_dpi);
    }
    // Point → DIP (RT then scales DIP → physical pixels via its DPI).
    return s.size * 96.0f / 72.0f;
}

DWRITE_FONT_WEIGHT to_dw_weight(Weight w) {
    switch (w) {
        case Weight::Semibold: return DWRITE_FONT_WEIGHT_SEMI_BOLD;
        case Weight::Bold:     return DWRITE_FONT_WEIGHT_BOLD;
        default:               return DWRITE_FONT_WEIGHT_NORMAL;
    }
}

DWRITE_FONT_STYLE to_dw_style(Slant s) {
    return s == Slant::Italic ? DWRITE_FONT_STYLE_ITALIC
                              : DWRITE_FONT_STYLE_NORMAL;
}

DWRITE_TEXT_ALIGNMENT to_dw_halign(HAlign a) {
    switch (a) {
        case HAlign::Center:   return DWRITE_TEXT_ALIGNMENT_CENTER;
        case HAlign::Trailing: return DWRITE_TEXT_ALIGNMENT_TRAILING;
        default:               return DWRITE_TEXT_ALIGNMENT_LEADING;
    }
}

DWRITE_PARAGRAPH_ALIGNMENT to_dw_valign(VAlign a) {
    switch (a) {
        case VAlign::Center: return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
        case VAlign::Bottom: return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
        default:             return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    }
}

IDWriteTextFormat* get_or_make_format(const Style& s) {
    FormatKey key{
        s.family,
        static_cast<int>(std::lround(dip_from_style(s) * 100.0f)),
        s.weight, s.slant, s.halign, s.valign, s.trim, s.wrap,
    };
    for (auto& [k, f] : g_format_cache) {
        if (k == key) return f.Get();
    }

    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = g_dw->CreateTextFormat(
        s.family, nullptr,
        to_dw_weight(s.weight),
        to_dw_style(s.slant),
        DWRITE_FONT_STRETCH_NORMAL,
        dip_from_style(s),
        L"",
        &fmt);
    if (FAILED(hr) || !fmt) return nullptr;

    fmt->SetTextAlignment(to_dw_halign(s.halign));
    fmt->SetParagraphAlignment(to_dw_valign(s.valign));
    fmt->SetWordWrapping(s.wrap == Wrap::Word
                         ? DWRITE_WORD_WRAPPING_WRAP
                         : DWRITE_WORD_WRAPPING_NO_WRAP);

    if (s.trim == Trim::EllipsisChar) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis))) {
            fmt->SetTrimming(&trim, ellipsis.Get());
        }
    }

    auto* raw = fmt.Get();
    g_format_cache.emplace_back(std::move(key), std::move(fmt));
    return raw;
}

DWORD pack_argb(COLORREF c, BYTE a) {
    return (static_cast<DWORD>(a) << 24)
         | (static_cast<DWORD>(GetRValue(c)) << 16)
         | (static_cast<DWORD>(GetGValue(c)) << 8)
         |  static_cast<DWORD>(GetBValue(c));
}

ID2D1SolidColorBrush* get_or_make_brush(COLORREF c, BYTE a) {
    DWORD key = pack_argb(c, a);
    for (auto& [k, b] : g_brush_cache) {
        if (k == key) return b.Get();
    }
    D2D1_COLOR_F col{
        GetRValue(c) / 255.0f,
        GetGValue(c) / 255.0f,
        GetBValue(c) / 255.0f,
        a / 255.0f,
    };
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(g_rt->CreateSolidColorBrush(col, &brush)) || !brush) {
        return nullptr;
    }
    auto* raw = brush.Get();
    g_brush_cache.emplace_back(key, std::move(brush));
    return raw;
}

bool create_render_target(UINT dpi) {
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi), static_cast<float>(dpi),
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE,
        D2D1_FEATURE_LEVEL_DEFAULT);
    ComPtr<ID2D1DCRenderTarget> rt;
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, &rt)) || !rt) {
        return false;
    }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    g_rt = std::move(rt);
    g_brush_cache.clear();   // brushes are tied to the old RT
    return true;
}

void recreate_rt_if_lost(HRESULT hr) {
    if (hr == D2DERR_RECREATE_TARGET) {
        g_rt.Reset();
        g_brush_cache.clear();
        create_render_target(g_dpi);
    }
}

// Build a fresh IDWriteTextLayout for one draw/measure call.
ComPtr<IDWriteTextLayout> make_layout(const wchar_t* text, UINT32 len,
                                      const Style& s,
                                      float max_w_dip, float max_h_dip) {
    if (len == 0xFFFFFFFFu) {
        size_t n = 0;
        while (text[n] != L'\0') ++n;
        len = static_cast<UINT32>(n);
    }
    IDWriteTextFormat* fmt = get_or_make_format(s);
    if (!fmt) return nullptr;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = g_dw->CreateTextLayout(text, len, fmt,
                                        max_w_dip, max_h_dip, &layout);
    if (FAILED(hr) || !layout) return nullptr;

    if (g_font_fallback) {
        ComPtr<IDWriteTextLayout2> layout2;
        if (SUCCEEDED(layout.As(&layout2)))
            layout2->SetFontFallback(g_font_fallback.Get());
    }
    return layout;
}

} // namespace

bool init() {
    HRESULT hr;

    {
        ComPtr<ID2D1Factory> base;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               __uuidof(ID2D1Factory), nullptr,
                               reinterpret_cast<void**>(base.GetAddressOf()));
        if (FAILED(hr) || !base) return false;
        if (FAILED(base.As(&g_d2d))) return false;
    }

    {
        ComPtr<IUnknown> unk;
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory2),
                                 unk.GetAddressOf());
        if (FAILED(hr) || !unk) return false;
        if (FAILED(unk.As(&g_dw))) return false;
    }

    if (!create_render_target(g_dpi)) return false;
    g_dw->GetSystemFontFallback(&g_font_fallback);
    return true;
}

void shutdown() {
    g_brush_cache.clear();
    g_format_cache.clear();
    g_rt.Reset();
    g_font_fallback.Reset();
    g_dw.Reset();
    g_d2d.Reset();
}

void on_dpi_changed(UINT dpi) {
    if (dpi == 0) dpi = 96;
    if (dpi == g_dpi) return;
    g_dpi = dpi;
    // Format DIP sizes depend on g_dpi for Pixel-unit styles.
    g_format_cache.clear();
    if (g_rt) g_rt->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
}

void set_font_fallback(IDWriteFontFallback* fallback) {
    g_font_fallback = fallback;
}

void draw(HDC hdc, const RECT& bounds,
          const wchar_t* text, int len,
          const Style& s)
{
    if (!g_rt || !g_dw) return;
    if (!text || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return;
    }

    float w_px = static_cast<float>(bounds.right  - bounds.left);
    float h_px = static_cast<float>(bounds.bottom - bounds.top);
    float dip_per_px = 96.0f / static_cast<float>(g_dpi);
    float w_dip = w_px * dip_per_px;
    float h_dip = h_px * dip_per_px;

    UINT32 ulen = (len < 0) ? 0xFFFFFFFFu : static_cast<UINT32>(len);
    ComPtr<IDWriteTextLayout> layout = make_layout(text, ulen, s, w_dip, h_dip);
    if (!layout) return;

    HRESULT hr = g_rt->BindDC(hdc, &bounds);
    if (FAILED(hr)) {
        recreate_rt_if_lost(hr);
        return;
    }

    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());

    ID2D1SolidColorBrush* brush = get_or_make_brush(s.color, s.alpha);
    if (brush) {
        g_rt->DrawTextLayout(
            D2D1::Point2F(0.0f, 0.0f),
            layout.Get(),
            brush,
            static_cast<D2D1_DRAW_TEXT_OPTIONS>(
                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
                | D2D1_DRAW_TEXT_OPTIONS_CLIP));
    }
    HRESULT end_hr = g_rt->EndDraw();
    recreate_rt_if_lost(end_hr);
}

Metrics measure(const wchar_t* text, int len,
                const Style& s, int max_width)
{
    Metrics out{0, 0, 0};
    if (!g_dw || !text) return out;

    bool wrap = (max_width > 0) && (s.wrap == Wrap::Word);
    float dip_per_px = 96.0f / static_cast<float>(g_dpi);
    float w_dip = (max_width > 0)
        ? static_cast<float>(max_width) * dip_per_px
        : 1.0e7f;   // effectively unbounded for no-wrap measure
    float h_dip = 1.0e7f;

    // Honor the caller's wrap intent regardless of the style's default.
    Style eff = s;
    if (!wrap) eff.wrap = Wrap::NoWrap;

    UINT32 ulen = (len < 0) ? 0xFFFFFFFFu : static_cast<UINT32>(len);
    ComPtr<IDWriteTextLayout> layout = make_layout(text, ulen, eff,
                                                   w_dip, h_dip);
    if (!layout) return out;

    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return out;

    float px_per_dip = static_cast<float>(g_dpi) / 96.0f;
    out.width  = static_cast<int>(std::ceil(
                    m.widthIncludingTrailingWhitespace * px_per_dip));
    out.height = static_cast<int>(std::ceil(m.height * px_per_dip));
    out.line_count = static_cast<int>(m.lineCount);
    return out;
}

} // namespace win32::text
