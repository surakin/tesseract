#include "BetterTextInternal.h"

#include <algorithm>
#include <imm.h>
#include <strsafe.h>
#include <vector>
#include <windowsx.h>

namespace bettertext {
namespace {

D2D1_COLOR_F Color(uint32_t rgba) {
    return D2D1::ColorF(
        static_cast<float>((rgba >> 24) & 0xff) / 255.0f,
        static_cast<float>((rgba >> 16) & 0xff) / 255.0f,
        static_cast<float>((rgba >> 8) & 0xff) / 255.0f,
        static_cast<float>(rgba & 0xff) / 255.0f);
}

int64_t SelectionStart(const ControlState& state) {
    return std::min(state.selection.anchor, state.selection.caret);
}

int64_t SelectionEnd(const ControlState& state) {
    return std::max(state.selection.anchor, state.selection.caret);
}

bool HasSelection(const ControlState& state) {
    return state.selection.anchor != state.selection.caret;
}

RECT ClientRect(HWND hwnd) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    return rect;
}

TextStyle SystemDefaultTextStyle(HWND hwnd) {
    TextStyle style;

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0)) {
        return style;
    }

    const LOGFONTW& font = metrics.lfMessageFont;
    if (font.lfFaceName[0]) {
        style.font_family = font.lfFaceName;
    }
    if (font.lfWeight > 0) {
        style.font_weight = font.lfWeight;
    }
    style.italic = font.lfItalic != FALSE;

    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : USER_DEFAULT_SCREEN_DPI;
    const LONG height = font.lfHeight < 0 ? -font.lfHeight : font.lfHeight;
    if (height > 0 && dpi > 0) {
        style.font_size = static_cast<float>(height) * 96.0f / static_cast<float>(dpi);
    }

    return style;
}

HRESULT EnsureFactories(ControlState* state) {
    if (!state->d2d_factory) {
        D2D1_FACTORY_OPTIONS options{};
        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &options,
            reinterpret_cast<void**>(state->d2d_factory.GetAddressOf()));
        if (FAILED(hr)) {
            return hr;
        }
    }
    if (!state->dwrite_factory) {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(state->dwrite_factory.GetAddressOf()));
        if (FAILED(hr)) {
            return hr;
        }
        state->dwrite_factory.As(&state->dwrite_factory4);
    }
    if (!state->d3d_device) {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL actual_level{};
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            state->d3d_device.GetAddressOf(),
            &actual_level,
            nullptr);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                ARRAYSIZE(levels),
                D3D11_SDK_VERSION,
                state->d3d_device.GetAddressOf(),
                &actual_level,
                nullptr);
        }
        if (FAILED(hr)) {
            return hr;
        }
        hr = state->d3d_device.As(&state->dxgi_device);
        if (FAILED(hr)) {
            return hr;
        }
    }
    if (!state->d2d_device) {
        HRESULT hr = state->d2d_factory->CreateDevice(state->dxgi_device.Get(), state->d2d_device.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
    }
    if (!state->device_context) {
        HRESULT hr = state->d2d_device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            state->device_context.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
        state->device_context.As(&state->device_context4);
    }
    return S_OK;
}

HRESULT CreateSwapChain(ControlState* state) {
    RECT rect = ClientRect(state->hwnd);
    const UINT width = static_cast<UINT>(std::max<LONG>(1, rect.right - rect.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, rect.bottom - rect.top));

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = state->dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        return hr;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(
        state->d3d_device.Get(),
        state->hwnd,
        &desc,
        nullptr,
        nullptr,
        state->swap_chain.GetAddressOf());
    if (SUCCEEDED(hr)) {
        factory->MakeWindowAssociation(state->hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    return hr;
}

HRESULT CreateTargetBitmap(ControlState* state) {
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    HRESULT hr = state->swap_chain->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf()));
    if (FAILED(hr)) {
        return hr;
    }

    const float dpi = static_cast<float>(GetDpiForWindow(state->hwnd));
    state->device_context->SetDpi(dpi, dpi);

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        dpi,
        dpi);
    hr = state->device_context->CreateBitmapFromDxgiSurface(
        surface.Get(),
        &properties,
        state->target_bitmap.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }
    state->device_context->SetTarget(state->target_bitmap.Get());
    return S_OK;
}

HRESULT ResizeRenderTarget(ControlState* state, UINT width, UINT height) {
    if (!state || !state->swap_chain) {
        return S_OK;
    }

    state->device_context->SetTarget(nullptr);
    state->target_bitmap.Reset();

    width = std::max<UINT>(1, width);
    height = std::max<UINT>(1, height);
    HRESULT hr = state->swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        ResetRenderResources(state);
        return hr;
    }
    return CreateTargetBitmap(state);
}

HRESULT EnsureRenderTarget(ControlState* state) {
    HRESULT hr = EnsureFactories(state);
    if (FAILED(hr)) {
        return hr;
    }
    if (!state->swap_chain) {
        hr = CreateSwapChain(state);
        if (FAILED(hr)) {
            return hr;
        }
    }
    if (!state->target_bitmap) {
        hr = CreateTargetBitmap(state);
        if (FAILED(hr)) {
            return hr;
        }
    }

    if (!state->foreground_brush) {
        state->device_context->CreateSolidColorBrush(Color(state->theme.foreground_rgba), state->foreground_brush.GetAddressOf());
    }
    if (!state->selection_brush) {
        state->device_context->CreateSolidColorBrush(Color(state->theme.selection_rgba), state->selection_brush.GetAddressOf());
    }
    if (!state->caret_brush) {
        state->device_context->CreateSolidColorBrush(Color(state->theme.caret_rgba), state->caret_brush.GetAddressOf());
    }
    if (!state->placeholder_brush) {
        state->device_context->CreateSolidColorBrush(Color(state->theme.placeholder_rgba), state->placeholder_brush.GetAddressOf());
    }
    return S_OK;
}

HRESULT EnsureTextFormat(ControlState* state) {
    HRESULT hr = EnsureFactories(state);
    if (FAILED(hr)) {
        return hr;
    }
    if (state->text_format) {
        return S_OK;
    }

    hr = state->dwrite_factory->CreateTextFormat(
        state->default_style.font_family.c_str(),
        nullptr,
        static_cast<DWRITE_FONT_WEIGHT>(state->default_style.font_weight),
        state->default_style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        state->default_style.font_size,
        L"",
        state->text_format.GetAddressOf());
    if (SUCCEEDED(hr)) {
        state->text_format->SetWordWrapping(
            state->single_line ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
        state->text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    return hr;
}

bool IsHighSurrogate(wchar_t ch) {
    return ch >= 0xd800 && ch <= 0xdbff;
}

bool IsLowSurrogate(wchar_t ch) {
    return ch >= 0xdc00 && ch <= 0xdfff;
}

uint32_t DecodeCodePoint(const std::wstring& text, size_t index, size_t* length) {
    *length = 1;
    const wchar_t lead = text[index];
    if (IsHighSurrogate(lead) && index + 1 < text.size() && IsLowSurrogate(text[index + 1])) {
        *length = 2;
        return 0x10000u +
            ((static_cast<uint32_t>(lead) - 0xd800u) << 10) +
            (static_cast<uint32_t>(text[index + 1]) - 0xdc00u);
    }
    return static_cast<uint32_t>(lead);
}

bool IsEmojiBase(uint32_t codepoint) {
    return codepoint == 0x00a9 ||
        codepoint == 0x00ae ||
        codepoint == 0x203c ||
        codepoint == 0x2049 ||
        codepoint == 0x2122 ||
        codepoint == 0x2139 ||
        (codepoint >= 0x2194 && codepoint <= 0x21aa) ||
        (codepoint >= 0x231a && codepoint <= 0x231b) ||
        codepoint == 0x2328 ||
        codepoint == 0x23cf ||
        (codepoint >= 0x23e9 && codepoint <= 0x23f3) ||
        (codepoint >= 0x23f8 && codepoint <= 0x23fa) ||
        codepoint == 0x24c2 ||
        (codepoint >= 0x25aa && codepoint <= 0x25ab) ||
        codepoint == 0x25b6 ||
        codepoint == 0x25c0 ||
        (codepoint >= 0x25fb && codepoint <= 0x25fe) ||
        (codepoint >= 0x2600 && codepoint <= 0x27bf) ||
        (codepoint >= 0x2934 && codepoint <= 0x2935) ||
        (codepoint >= 0x2b05 && codepoint <= 0x2b55) ||
        codepoint == 0x3030 ||
        codepoint == 0x303d ||
        codepoint == 0x3297 ||
        codepoint == 0x3299 ||
        (codepoint >= 0x1f000 && codepoint <= 0x1faff);
}

bool IsRegionalIndicator(uint32_t codepoint) {
    return codepoint >= 0x1f1e6 && codepoint <= 0x1f1ff;
}

bool IsEmojiModifier(uint32_t codepoint) {
    return codepoint == 0xfe0f ||
        (codepoint >= 0x1f3fb && codepoint <= 0x1f3ff);
}

bool IsKeycapStarter(uint32_t codepoint) {
    return codepoint == L'#' || codepoint == L'*' || (codepoint >= L'0' && codepoint <= L'9');
}

size_t ConsumeEmojiTail(const std::wstring& text, size_t index) {
    size_t current = index;
    while (current < text.size()) {
        size_t length = 0;
        const uint32_t codepoint = DecodeCodePoint(text, current, &length);
        if (!IsEmojiModifier(codepoint) && codepoint != 0x20e3) {
            break;
        }
        current += length;
    }
    return current;
}

size_t EmojiRangeLength(const std::wstring& text, size_t index) {
    size_t length = 0;
    const uint32_t codepoint = DecodeCodePoint(text, index, &length);

    if (IsKeycapStarter(codepoint)) {
        size_t current = index + length;
        if (current < text.size()) {
            size_t next_length = 0;
            const uint32_t next = DecodeCodePoint(text, current, &next_length);
            if (next == 0xfe0f) {
                current += next_length;
            }
        }
        if (current < text.size()) {
            size_t next_length = 0;
            if (DecodeCodePoint(text, current, &next_length) == 0x20e3) {
                return current + next_length - index;
            }
        }
        return 0;
    }

    if (!IsEmojiBase(codepoint)) {
        return 0;
    }

    size_t current = ConsumeEmojiTail(text, index + length);
    if (IsRegionalIndicator(codepoint) && current < text.size()) {
        size_t next_length = 0;
        const uint32_t next = DecodeCodePoint(text, current, &next_length);
        if (IsRegionalIndicator(next)) {
            current = ConsumeEmojiTail(text, current + next_length);
        }
    }

    while (current < text.size()) {
        size_t joiner_length = 0;
        if (DecodeCodePoint(text, current, &joiner_length) != 0x200d) {
            break;
        }
        const size_t joined = current + joiner_length;
        if (joined >= text.size()) {
            break;
        }
        size_t joined_length = 0;
        const uint32_t joined_codepoint = DecodeCodePoint(text, joined, &joined_length);
        if (!IsEmojiBase(joined_codepoint) && !IsKeycapStarter(joined_codepoint)) {
            break;
        }
        current = ConsumeEmojiTail(text, joined + joined_length);
    }

    return current - index;
}

HRESULT EnsureEmojiFontCollection(ControlState* state) {
    if (!state->font_provider || state->emoji_font_collection) {
        return S_OK;
    }

    IDWriteFontCollection* collection = nullptr;
    HRESULT hr = state->font_provider->CreateFontCollection(state->dwrite_factory.Get(), &collection);
    if (SUCCEEDED(hr) && collection) {
        state->emoji_font_collection.Attach(collection);
    }
    return hr;
}

void ApplyEmojiFallback(ControlState* state, IDWriteTextLayout* layout, const std::wstring& text) {
    if (!layout || text.empty()) {
        return;
    }

    Microsoft::WRL::ComPtr<IDWriteFontCollection> emoji_collection;
    std::wstring emoji_family = L"Segoe UI Emoji";
    if (state->font_provider && SUCCEEDED(EnsureEmojiFontCollection(state)) && state->emoji_font_collection) {
        emoji_collection = state->emoji_font_collection;
        const wchar_t* provider_family = state->font_provider->EmojiFallbackFamily();
        if (provider_family && provider_family[0]) {
            emoji_family = provider_family;
        }
    }

    for (size_t i = 0; i < text.size();) {
        const size_t range_length = EmojiRangeLength(text, i);
        if (range_length == 0) {
            size_t codepoint_length = 0;
            DecodeCodePoint(text, i, &codepoint_length);
            i += codepoint_length;
            continue;
        }

        const DWRITE_TEXT_RANGE range{
            static_cast<UINT32>(i),
            static_cast<UINT32>(range_length),
        };
        if (emoji_collection) {
            layout->SetFontCollection(emoji_collection.Get(), range);
        }
        layout->SetFontFamilyName(emoji_family.c_str(), range);
        i += range_length;
    }
}

// Text actually laid out on screen: the real document text with password
// masking applied and the live IME composition string (if any) spliced in at
// the caret. `document`/PlainText() itself is never touched by either —
// masking is display-only and the composition only lands in the document
// once GCS_RESULTSTR commits it (see HandleImeComposition).
std::wstring ComposedDisplayText(const ControlState* state, size_t* out_composition_start = nullptr,
                                  size_t* out_composition_len = nullptr) {
    if (out_composition_start) *out_composition_start = 0;
    if (out_composition_len) *out_composition_len = 0;

    std::wstring text = state->document.PlainText();
    if (state->password_mode) {
        for (wchar_t& ch : text) {
            if (ch != L'\n') ch = L'•';
        }
    }

    if (state->ime_composing && !state->ime_composition.empty()) {
        const size_t caret = static_cast<size_t>(
            std::clamp<int64_t>(state->selection.caret, 0, static_cast<int64_t>(text.size())));
        std::wstring piece = state->ime_composition;
        if (state->password_mode) {
            for (wchar_t& ch : piece) ch = L'•';
        }
        text.insert(caret, piece);
        if (out_composition_start) *out_composition_start = caret;
        if (out_composition_len) *out_composition_len = piece.size();
    }

    return text;
}

// Draws a resolved image bitmap inline at the position DirectWrite assigns
// its host atom's U+FFFC placeholder. One instance per image-run-per-layout
// build (layouts are rebuilt often; construction is a cheap heap alloc for
// what's typically a handful of custom-emoji atoms). Mirrors the real
// COM refcounting SingleFileFontEnumerator already uses elsewhere in this
// library — DirectWrite AddRefs on SetInlineObject and Releases when the
// layout is done with it, so this must actually free itself at refcount 0.
class BetterTextInlineImage final : public IDWriteInlineObject {
public:
    BetterTextInlineImage(ID2D1DeviceContext* context, ID2D1Bitmap* bitmap, float width, float height)
        : context_(context), bitmap_(bitmap), width_(width), height_(height) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualGUID(iid, __uuidof(IUnknown)) || IsEqualGUID(iid, __uuidof(IDWriteInlineObject))) {
            *object = static_cast<IDWriteInlineObject*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT origin_x, FLOAT origin_y, BOOL, BOOL,
                                    IUnknown*) override {
        // Match the color-emoji glyph path (DrawBitmapColorGlyphRun below) so
        // custom-emoji images and Unicode color emoji scale identically.
        context_->DrawBitmap(
            bitmap_.Get(), D2D1::RectF(origin_x, origin_y, origin_x + width_, origin_y + height_), 1.0f,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) override {
        if (!metrics) {
            return E_POINTER;
        }
        metrics->width = width_;
        metrics->height = height_;
        metrics->baseline = height_;
        metrics->supportsSideways = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) override {
        if (!overhangs) {
            return E_POINTER;
        }
        *overhangs = DWRITE_OVERHANG_METRICS{ 0.0f, 0.0f, 0.0f, 0.0f };
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before,
                                                  DWRITE_BREAK_CONDITION* after) override {
        if (!before || !after) {
            return E_POINTER;
        }
        *before = DWRITE_BREAK_CONDITION_NEUTRAL;
        *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

private:
    LONG ref_count_ = 1;
    ID2D1DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    float width_ = 0.0f;
    float height_ = 0.0f;
};

HRESULT CreateLayout(ControlState* state, IDWriteTextLayout** layout) {
    *layout = nullptr;
    HRESULT hr = EnsureTextFormat(state);
    if (FAILED(hr)) {
        return hr;
    }

    RECT rect = ClientRect(state->hwnd);
    const float width = std::max(1.0f, static_cast<float>(rect.right - rect.left) - (state->padding_x_dip * 2.0f));
    size_t composition_start = 0;
    size_t composition_len = 0;
    const std::wstring text = ComposedDisplayText(state, &composition_start, &composition_len);
    hr = state->dwrite_factory->CreateTextLayout(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        state->text_format.Get(),
        width,
        100000.0f,
        layout);
    if (SUCCEEDED(hr) && state->default_style.underline && !text.empty()) {
        (*layout)->SetUnderline(TRUE, DWRITE_TEXT_RANGE{ 0, static_cast<UINT32>(text.size()) });
    }
    if (SUCCEEDED(hr) && composition_len > 0) {
        (*layout)->SetUnderline(
            TRUE, DWRITE_TEXT_RANGE{ static_cast<UINT32>(composition_start), static_cast<UINT32>(composition_len) });
    }
    if (SUCCEEDED(hr)) {
        ApplyEmojiFallback(state, *layout, text);
    }
    if (SUCCEEDED(hr) && !state->resolved_images.empty()) {
        for (const auto& info : state->document.ImageAtoms()) {
            auto found = state->resolved_images.find(info.uri);
            if (found == state->resolved_images.end() || !found->second) {
                continue;  // not resolved yet — keeps rendering as U+FFFC tofu
            }
            // An in-progress IME composition splices text in ahead of this
            // atom's index — shift accordingly so the image object lands on
            // the right character of the *displayed* layout, not the
            // document's own (unspliced) index space.
            size_t index = info.atom_index;
            if (composition_len > 0 && index >= composition_start) {
                index += composition_len;
            }
            auto* inline_object = new BetterTextInlineImage(
                state->device_context.Get(), found->second.Get(), info.display_width, info.display_height);
            (*layout)->SetInlineObject(inline_object, DWRITE_TEXT_RANGE{ static_cast<UINT32>(index), 1 });
            inline_object->Release();  // SetInlineObject took its own reference
        }
    }
    return hr;
}

float LayoutHeight(ControlState* state) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(CreateLayout(state, layout.GetAddressOf())) || !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.height + state->padding_y_dip * 2.0f;
}

void UpdateScrollInfo(ControlState* state) {
    RECT rect = ClientRect(state->hwnd);
    const float content_height = LayoutHeight(state);
    const int page = std::max<LONG>(1, rect.bottom - rect.top);
    const int max_pos = std::max(0, static_cast<int>(content_height) - page);
    state->scroll_y = std::clamp(state->scroll_y, 0.0f, static_cast<float>(max_pos));

    // SetScrollInfo auto-installs a vertical scrollbar (reserving its gutter
    // width) even though the window was never created with WS_VSCROLL — it
    // isn't gated on whether scrolling is actually wanted. Mouse-wheel
    // scrolling (WM_MOUSEWHEEL) works independently of this, so skipping the
    // call entirely when the caller hasn't opted in costs nothing.
    if (!state->show_scrollbar) {
        return;
    }

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
    info.nMin = 0;
    info.nMax = std::max(page, static_cast<int>(content_height));
    info.nPage = static_cast<UINT>(page);
    info.nPos = static_cast<int>(state->scroll_y);
    SetScrollInfo(state->hwnd, SB_VERT, &info, TRUE);
}

void PaintGdiFallback(ControlState* state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(state->hwnd, &ps);
    RECT rect = ClientRect(state->hwnd);
    HBRUSH background = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &rect, background);
    DeleteObject(background);
    rect.left += static_cast<LONG>(state->padding_x_dip);
    rect.top += static_cast<LONG>(state->padding_y_dip - state->scroll_y);
    DrawTextW(dc, state->document.PlainText().c_str(), -1, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    EndPaint(state->hwnd, &ps);
}

class TextRenderer final : public IDWriteTextRenderer {
public:
    explicit TextRenderer(ControlState* state)
        : state_(state) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualGUID(iid, __uuidof(IUnknown)) ||
            IsEqualGUID(iid, __uuidof(IDWritePixelSnapping)) ||
            IsEqualGUID(iid, __uuidof(IDWriteTextRenderer))) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* is_disabled) override {
        if (!is_disabled) {
            return E_POINTER;
        }
        *is_disabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        if (!transform) {
            return E_POINTER;
        }
        D2D1_MATRIX_3X2_F d2d_transform{};
        state_->device_context->GetTransform(&d2d_transform);
        transform->m11 = d2d_transform._11;
        transform->m12 = d2d_transform._12;
        transform->m21 = d2d_transform._21;
        transform->m22 = d2d_transform._22;
        transform->dx = d2d_transform._31;
        transform->dy = d2d_transform._32;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels_per_dip) override {
        if (!pixels_per_dip) {
            return E_POINTER;
        }
        FLOAT dpi_x = 96.0f;
        FLOAT dpi_y = 96.0f;
        state_->device_context->GetDpi(&dpi_x, &dpi_y);
        *pixels_per_dip = dpi_x / 96.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*,
        FLOAT baseline_origin_x,
        FLOAT baseline_origin_y,
        DWRITE_MEASURING_MODE measuring_mode,
        const DWRITE_GLYPH_RUN* glyph_run,
        const DWRITE_GLYPH_RUN_DESCRIPTION* glyph_run_description,
        IUnknown*) override {
        if (DrawColorGlyphRun(baseline_origin_x, baseline_origin_y, measuring_mode, glyph_run, glyph_run_description)) {
            return S_OK;
        }
        state_->device_context->DrawGlyphRun(
            D2D1::Point2F(baseline_origin_x, baseline_origin_y),
            glyph_run,
            state_->foreground_brush.Get(),
            measuring_mode);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void*,
        FLOAT baseline_origin_x,
        FLOAT baseline_origin_y,
        const DWRITE_UNDERLINE* underline,
        IUnknown*) override {
        if (!underline) {
            return E_INVALIDARG;
        }
        const float top = baseline_origin_y + underline->offset;
        state_->device_context->FillRectangle(
            D2D1::RectF(
                baseline_origin_x,
                top,
                baseline_origin_x + underline->width,
                top + underline->thickness),
            state_->foreground_brush.Get());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void*,
        FLOAT baseline_origin_x,
        FLOAT baseline_origin_y,
        const DWRITE_STRIKETHROUGH* strikethrough,
        IUnknown*) override {
        if (!strikethrough) {
            return E_INVALIDARG;
        }
        const float top = baseline_origin_y + strikethrough->offset;
        state_->device_context->FillRectangle(
            D2D1::RectF(
                baseline_origin_x,
                top,
                baseline_origin_x + strikethrough->width,
                top + strikethrough->thickness),
            state_->foreground_brush.Get());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void* client_drawing_context,
        FLOAT origin_x,
        FLOAT origin_y,
        IDWriteInlineObject* inline_object,
        BOOL is_sideways,
        BOOL is_right_to_left,
        IUnknown* client_drawing_effect) override {
        if (!inline_object) {
            return E_INVALIDARG;
        }
        return inline_object->Draw(
            client_drawing_context,
            this,
            origin_x,
            origin_y,
            is_sideways,
            is_right_to_left,
            client_drawing_effect);
    }

private:
    bool DrawColorGlyphRun(
        FLOAT baseline_origin_x,
        FLOAT baseline_origin_y,
        DWRITE_MEASURING_MODE measuring_mode,
        const DWRITE_GLYPH_RUN* glyph_run,
        const DWRITE_GLYPH_RUN_DESCRIPTION* glyph_run_description) {
        if (!state_->dwrite_factory4 || !state_->device_context4 || !glyph_run) {
            return false;
        }

        constexpr DWRITE_GLYPH_IMAGE_FORMATS kBitmapFormats =
            DWRITE_GLYPH_IMAGE_FORMATS_PNG |
            DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
            DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
            DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8;
        constexpr DWRITE_GLYPH_IMAGE_FORMATS kDesiredFormats =
            DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE |
            DWRITE_GLYPH_IMAGE_FORMATS_CFF |
            DWRITE_GLYPH_IMAGE_FORMATS_COLR |
            DWRITE_GLYPH_IMAGE_FORMATS_SVG |
            kBitmapFormats;

        DWRITE_MATRIX transform{};
        GetCurrentTransform(nullptr, &transform);

        Microsoft::WRL::ComPtr<IDWriteColorGlyphRunEnumerator1> color_layers;
        const HRESULT hr = state_->dwrite_factory4->TranslateColorGlyphRun(
            D2D1::Point2F(baseline_origin_x, baseline_origin_y),
            glyph_run,
            glyph_run_description,
            kDesiredFormats,
            measuring_mode,
            &transform,
            0,
            color_layers.GetAddressOf());
        if (hr == DWRITE_E_NOCOLOR || !color_layers) {
            return false;
        }
        if (FAILED(hr)) {
            return false;
        }

        BOOL has_run = FALSE;
        while (SUCCEEDED(color_layers->MoveNext(&has_run)) && has_run) {
            const DWRITE_COLOR_GLYPH_RUN1* color_run = nullptr;
            if (FAILED(color_layers->GetCurrentRun(&color_run)) || !color_run) {
                continue;
            }

            if ((color_run->glyphImageFormat & kBitmapFormats) != 0 &&
                DrawBitmapColorGlyphRun(color_run, measuring_mode)) {
                continue;
            }

            if (color_run->glyphImageFormat == DWRITE_GLYPH_IMAGE_FORMATS_SVG) {
                state_->device_context4->DrawSvgGlyphRun(
                    D2D1::Point2F(color_run->baselineOriginX, color_run->baselineOriginY),
                    &color_run->glyphRun,
                    state_->foreground_brush.Get(),
                    nullptr,
                    0,
                    measuring_mode);
                continue;
            }

            Microsoft::WRL::ComPtr<ID2D1Brush> brush = BrushForColorRun(color_run);
            state_->device_context->DrawGlyphRun(
                D2D1::Point2F(color_run->baselineOriginX, color_run->baselineOriginY),
                &color_run->glyphRun,
                brush.Get(),
                measuring_mode);
        }
        return true;
    }

    bool DrawBitmapColorGlyphRun(const DWRITE_COLOR_GLYPH_RUN1* color_run, DWRITE_MEASURING_MODE measuring_mode) {
        const DWRITE_GLYPH_RUN& glyph_run = color_run->glyphRun;
        if (!glyph_run.fontFace || glyph_run.glyphCount == 0 || !glyph_run.glyphIndices) {
            return false;
        }

        std::vector<D2D1_POINT_2F> origins(glyph_run.glyphCount);
        DWRITE_MATRIX dwrite_transform{};
        GetCurrentTransform(nullptr, &dwrite_transform);
        HRESULT hr = state_->dwrite_factory4->ComputeGlyphOrigins(
            &glyph_run,
            measuring_mode,
            D2D1::Point2F(color_run->baselineOriginX, color_run->baselineOriginY),
            &dwrite_transform,
            origins.data());
        if (FAILED(hr)) {
            return false;
        }

        D2D1_MATRIX_3X2_F original_transform{};
        state_->device_context->GetTransform(&original_transform);
        FLOAT dpi_x = 96.0f;
        FLOAT dpi_y = 96.0f;
        state_->device_context->GetDpi(&dpi_x, &dpi_y);

        bool drew_any = false;
        for (UINT32 i = 0; i < glyph_run.glyphCount; ++i) {
            D2D1_MATRIX_3X2_F glyph_transform{};
            Microsoft::WRL::ComPtr<ID2D1Image> glyph_image;
            hr = state_->device_context4->GetColorBitmapGlyphImage(
                color_run->glyphImageFormat,
                origins[i],
                glyph_run.fontFace,
                glyph_run.fontEmSize,
                glyph_run.glyphIndices[i],
                glyph_run.isSideways,
                &original_transform,
                dpi_x,
                dpi_y,
                &glyph_transform,
                glyph_image.GetAddressOf());
            if (FAILED(hr) || !glyph_image) {
                continue;
            }

            state_->device_context->SetTransform(glyph_transform);
            state_->device_context->DrawImage(
                glyph_image.Get(),
                nullptr,
                nullptr,
                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                D2D1_COMPOSITE_MODE_SOURCE_OVER);
            drew_any = true;
        }
        state_->device_context->SetTransform(original_transform);
        return drew_any;
    }

    Microsoft::WRL::ComPtr<ID2D1Brush> BrushForColorRun(const DWRITE_COLOR_GLYPH_RUN* color_run) {
        if (color_run->paletteIndex == DWRITE_NO_PALETTE_INDEX) {
            Microsoft::WRL::ComPtr<ID2D1Brush> brush;
            state_->foreground_brush.As(&brush);
            return brush;
        }

        D2D1_COLOR_F color{};
        color.r = color_run->runColor.r;
        color.g = color_run->runColor.g;
        color.b = color_run->runColor.b;
        color.a = color_run->runColor.a;

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(state_->device_context->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
            Microsoft::WRL::ComPtr<ID2D1Brush> fallback;
            state_->foreground_brush.As(&fallback);
            return fallback;
        }
        Microsoft::WRL::ComPtr<ID2D1Brush> result;
        brush.As(&result);
        return result;
    }

    ControlState* state_ = nullptr;
};

void Paint(ControlState* state) {
    if (FAILED(EnsureRenderTarget(state))) {
        PaintGdiFallback(state);
        return;
    }

    PAINTSTRUCT ps{};
    BeginPaint(state->hwnd, &ps);

    state->device_context->BeginDraw();
    state->device_context->Clear(Color(state->theme.background_rgba));

    if (state->document.Empty() && !state->ime_composing && !state->placeholder.empty() &&
        SUCCEEDED(EnsureTextFormat(state))) {
        RECT rect = ClientRect(state->hwnd);
        const float width = std::max(1.0f, static_cast<float>(rect.right - rect.left) - (state->padding_x_dip * 2.0f));
        Microsoft::WRL::ComPtr<IDWriteTextLayout> placeholder_layout;
        if (SUCCEEDED(state->dwrite_factory->CreateTextLayout(
                state->placeholder.c_str(), static_cast<UINT32>(state->placeholder.size()),
                state->text_format.Get(), width, 100000.0f, placeholder_layout.GetAddressOf()))) {
            state->device_context->DrawTextLayout(
                D2D1::Point2F(state->padding_x_dip, state->padding_y_dip), placeholder_layout.Get(),
                state->placeholder_brush.Get());
        }
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(CreateLayout(state, layout.GetAddressOf())) && layout) {
        const D2D1_POINT_2F origin =
            D2D1::Point2F(state->padding_x_dip - state->scroll_x, state->padding_y_dip - state->scroll_y);

        if (HasSelection(*state)) {
            const UINT32 start = static_cast<UINT32>(SelectionStart(*state));
            const UINT32 length = static_cast<UINT32>(SelectionEnd(*state) - SelectionStart(*state));
            UINT32 actual = 0;
            HRESULT hr = layout->HitTestTextRange(start, length, origin.x, origin.y, nullptr, 0, &actual);
            if (hr == E_NOT_SUFFICIENT_BUFFER && actual > 0) {
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(actual);
                if (SUCCEEDED(layout->HitTestTextRange(start, length, origin.x, origin.y, metrics.data(), actual, &actual))) {
                    for (UINT32 i = 0; i < actual; ++i) {
                        const auto& m = metrics[i];
                        state->device_context->FillRectangle(
                            D2D1::RectF(m.left, m.top, m.left + m.width, m.top + m.height),
                            state->selection_brush.Get());
                    }
                }
            }
        }

        TextRenderer renderer(state);
        layout->Draw(nullptr, &renderer, origin.x, origin.y);

        if (GetFocus() == state->hwnd) {
            UINT32 caret = 0;
            if (state->ime_composing) {
                size_t composition_start = 0;
                size_t composition_len = 0;
                ComposedDisplayText(state, &composition_start, &composition_len);
                caret = static_cast<UINT32>(
                    composition_start +
                    static_cast<size_t>(std::clamp<int32_t>(state->ime_composition_cursor, 0,
                                                             static_cast<int32_t>(composition_len))));
            } else {
                caret = static_cast<UINT32>(std::clamp<int64_t>(
                    state->selection.caret,
                    0,
                    static_cast<int64_t>(state->document.Length())));
            }
            FLOAT x = 0.0f;
            FLOAT y = 0.0f;
            DWRITE_HIT_TEST_METRICS metrics{};
            if (SUCCEEDED(layout->HitTestTextPosition(caret, FALSE, &x, &y, &metrics))) {
                const float left = origin.x + x;
                const float top = origin.y + y;
                state->device_context->DrawLine(
                    D2D1::Point2F(left, top),
                    D2D1::Point2F(left, top + metrics.height),
                    state->caret_brush.Get(),
                    1.0f);
            }
        }
    }

    HRESULT hr = state->device_context->EndDraw();
    if (SUCCEEDED(hr) && state->swap_chain) {
        hr = state->swap_chain->Present(1, 0);
    }
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        ResetRenderResources(state);
    }
    EndPaint(state->hwnd, &ps);
}

int64_t HitTest(ControlState* state, float x, float y) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(CreateLayout(state, layout.GetAddressOf())) || !layout) {
        return state->selection.caret;
    }
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    const HRESULT hr = layout->HitTestPoint(
        x - state->padding_x_dip + state->scroll_x,
        y - state->padding_y_dip + state->scroll_y,
        &trailing,
        &inside,
        &metrics);
    if (FAILED(hr)) {
        return state->selection.caret;
    }
    int64_t position = static_cast<int64_t>(metrics.textPosition) + (trailing ? 1 : 0);
    return std::clamp<int64_t>(position, 0, static_cast<int64_t>(state->document.Length()));
}

// Single-line fields don't wrap — instead they scroll horizontally, mirroring
// EDIT's ES_AUTOHSCROLL. Called after every caret-affecting edit/navigation
// while single_line is set.
void EnsureCaretVisibleHorizontally(ControlState* state) {
    if (!state->single_line) {
        return;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(CreateLayout(state, layout.GetAddressOf())) || !layout) {
        return;
    }
    RECT rect = ClientRect(state->hwnd);
    const float visible_width =
        std::max(1.0f, static_cast<float>(rect.right - rect.left) - state->padding_x_dip * 2.0f);
    const UINT32 caret = static_cast<UINT32>(
        std::clamp<int64_t>(state->selection.caret, 0, static_cast<int64_t>(state->document.Length())));
    FLOAT x = 0.0f;
    FLOAT y = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestTextPosition(caret, FALSE, &x, &y, &metrics))) {
        return;
    }
    if (x - state->scroll_x < 0.0f) {
        state->scroll_x = x;
    } else if (x - state->scroll_x > visible_width) {
        state->scroll_x = x - visible_width;
    }
    state->scroll_x = std::max(0.0f, state->scroll_x);
}

std::wstring SelectedText(ControlState* state) {
    const std::wstring text = state->document.PlainText();
    const int64_t start = SelectionStart(*state);
    const int64_t end = SelectionEnd(*state);
    if (start < 0 || end < start || static_cast<size_t>(end) > text.size()) {
        return {};
    }
    return text.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
}

void CopySelectionToClipboard(ControlState* state) {
    const std::wstring selected = SelectedText(state);
    if (selected.empty() || !OpenClipboard(state->hwnd)) {
        return;
    }
    EmptyClipboard();
    const SIZE_T bytes = (selected.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            memcpy(data, selected.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
            memory = nullptr;
        }
    }
    if (memory) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

void PasteClipboardText(ControlState* state) {
    if (state->read_only || !OpenClipboard(state->hwnd)) {
        return;
    }
    HGLOBAL memory = GetClipboardData(CF_UNICODETEXT);
    if (memory) {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(memory));
        if (text) {
            BetterTextInsertText(state->hwnd, text);
            GlobalUnlock(memory);
        }
    }
    CloseClipboard();
}

void DeleteSelectionOrRange(ControlState* state, bool backward) {
    if (state->read_only) {
        return;
    }
    int64_t start = SelectionStart(*state);
    int64_t end = SelectionEnd(*state);
    if (start == end) {
        if (backward) {
            if (start == 0) {
                return;
            }
            --start;
        } else {
            if (end >= static_cast<int64_t>(state->document.Length())) {
                return;
            }
            ++end;
        }
    }
    state->PushUndo();
    state->ClearRedo();
    state->document.DeleteRange(static_cast<size_t>(start), static_cast<size_t>(end - start));
    state->selection = { start, start };
    state->ResetVerticalCaretX();
    EnsureCaretVisibleHorizontally(state);
    NotifyChanged(state);
    InvalidateBetterText(state);
}

void MoveCaret(ControlState* state, int64_t caret, bool extend, bool keep_vertical_x = false) {
    caret = std::clamp<int64_t>(caret, 0, static_cast<int64_t>(state->document.Length()));
    if (extend) {
        state->selection.caret = caret;
    } else {
        state->selection = { caret, caret };
    }
    if (!keep_vertical_x) {
        state->ResetVerticalCaretX();
    }
    EnsureCaretVisibleHorizontally(state);
    InvalidateBetterText(state);
}

int64_t PlainTextLineMove(ControlState* state, int direction) {
    const std::wstring text = state->document.PlainText();
    const int64_t length = static_cast<int64_t>(text.size());
    const int64_t caret = std::clamp<int64_t>(state->selection.caret, 0, length);

    auto line_start = [&](int64_t position) {
        if (position <= 0) {
            return int64_t{ 0 };
        }
        const size_t search_from = static_cast<size_t>(position - 1);
        const size_t newline = text.rfind(L'\n', search_from);
        return newline == std::wstring::npos ? int64_t{ 0 } : static_cast<int64_t>(newline + 1);
    };

    auto line_end = [&](int64_t start) {
        const size_t newline = text.find(L'\n', static_cast<size_t>(start));
        return newline == std::wstring::npos ? length : static_cast<int64_t>(newline);
    };

    const int64_t current_start = line_start(caret);
    const int64_t current_end = line_end(current_start);
    const int64_t column = caret - current_start;

    if (direction < 0) {
        if (current_start == 0) {
            return 0;
        }
        const int64_t target_start = line_start(current_start - 1);
        const int64_t target_end = line_end(target_start);
        return std::min(target_start + column, target_end);
    }

    if (current_end >= length) {
        return length;
    }
    const int64_t target_start = current_end + 1;
    const int64_t target_end = line_end(target_start);
    return std::min(target_start + column, target_end);
}

void MoveCaretVertically(ControlState* state, int direction, bool extend) {
    const int64_t length = static_cast<int64_t>(state->document.Length());
    if (length == 0) {
        MoveCaret(state, 0, extend, true);
        return;
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(CreateLayout(state, layout.GetAddressOf())) || !layout) {
        MoveCaret(state, PlainTextLineMove(state, direction), extend);
        return;
    }

    const UINT32 caret = static_cast<UINT32>(std::clamp<int64_t>(state->selection.caret, 0, length));
    FLOAT caret_x = 0.0f;
    FLOAT caret_y = 0.0f;
    DWRITE_HIT_TEST_METRICS caret_metrics{};
    if (FAILED(layout->HitTestTextPosition(caret, FALSE, &caret_x, &caret_y, &caret_metrics)) ||
        caret_metrics.height <= 0.0f) {
        MoveCaret(state, PlainTextLineMove(state, direction), extend);
        return;
    }

    if (!state->vertical_caret_x_valid) {
        state->vertical_caret_x = caret_x;
        state->vertical_caret_x_valid = true;
    }

    const float target_y = direction < 0
        ? caret_metrics.top - 0.5f
        : caret_metrics.top + caret_metrics.height + 0.5f;

    DWRITE_TEXT_METRICS layout_metrics{};
    if (SUCCEEDED(layout->GetMetrics(&layout_metrics))) {
        if (target_y < 0.0f) {
            MoveCaret(state, 0, extend, true);
            return;
        }
        if (target_y >= layout_metrics.height) {
            MoveCaret(state, length, extend, true);
            return;
        }
    }

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS target_metrics{};
    if (FAILED(layout->HitTestPoint(state->vertical_caret_x, target_y, &trailing, &inside, &target_metrics))) {
        MoveCaret(state, PlainTextLineMove(state, direction), extend);
        return;
    }

    const int64_t target = static_cast<int64_t>(target_metrics.textPosition) + (trailing ? 1 : 0);
    MoveCaret(state, target, extend, true);
}

bool CtrlDown() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

bool ShiftDown() {
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}

LRESULT HandleKeyDown(ControlState* state, WPARAM key) {
    const bool ctrl = CtrlDown();
    const bool shift = ShiftDown();
    if (ctrl) {
        switch (key) {
        case L'A':
            state->selection = { 0, static_cast<int64_t>(state->document.Length()) };
            state->ResetVerticalCaretX();
            InvalidateBetterText(state);
            return 0;
        case L'C':
            CopySelectionToClipboard(state);
            return 0;
        case L'V':
            PasteClipboardText(state);
            return 0;
        case L'X':
            CopySelectionToClipboard(state);
            DeleteSelectionOrRange(state, false);
            return 0;
        case L'Z':
            BetterTextUndo(state->hwnd);
            return 0;
        case L'Y':
            BetterTextRedo(state->hwnd);
            return 0;
        default:
            break;
        }
    }

    switch (key) {
    case VK_LEFT:
        MoveCaret(state, state->selection.caret - 1, shift);
        return 0;
    case VK_RIGHT:
        MoveCaret(state, state->selection.caret + 1, shift);
        return 0;
    case VK_UP:
        MoveCaretVertically(state, -1, shift);
        return 0;
    case VK_DOWN:
        MoveCaretVertically(state, 1, shift);
        return 0;
    case VK_HOME:
        MoveCaret(state, 0, shift);
        return 0;
    case VK_END:
        MoveCaret(state, static_cast<int64_t>(state->document.Length()), shift);
        return 0;
    case VK_BACK:
        DeleteSelectionOrRange(state, true);
        return 0;
    case VK_DELETE:
        DeleteSelectionOrRange(state, false);
        return 0;
    default:
        return DefWindowProcW(state->hwnd, WM_KEYDOWN, key, 0);
    }
}

LRESULT HandleChar(ControlState* state, WPARAM ch) {
    if (state->read_only || CtrlDown()) {
        return 0;
    }
    if (ch == L'\b' || ch == 0x1b) {
        return 0;
    }

    if (ch == L'\r' && (state->single_line || (state->submit_on_enter && !ShiftDown()))) {
        NotifySubmit(state);
        return 0;
    }

    wchar_t buffer[3] = {};
    if (ch == L'\r') {
        buffer[0] = L'\n';
    } else if (ch == L'\t' || ch >= 0x20) {
        buffer[0] = static_cast<wchar_t>(ch);
    } else {
        return 0;
    }
    BetterTextInsertText(state->hwnd, buffer);
    UpdateScrollInfo(state);
    EnsureCaretVisibleHorizontally(state);
    return 0;
}

// Positions the (legacy, non-TSF) IME composition/candidate window at the
// caret so it doesn't default to the top-left of the window. Modern TSF IMEs
// mostly ignore this in favor of drawing their candidate UI near the caret
// they infer from our inline rendering, but this is still the documented
// mechanism and is respected by IMEs that fall back to the classic API.
void UpdateImeCompositionWindowPos(ControlState* state) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(CreateLayout(state, layout.GetAddressOf())) || !layout) {
        return;
    }
    size_t composition_start = 0;
    size_t composition_len = 0;
    ComposedDisplayText(state, &composition_start, &composition_len);
    const UINT32 caret = static_cast<UINT32>(
        composition_start +
        static_cast<size_t>(std::clamp<int32_t>(state->ime_composition_cursor, 0,
                                                 static_cast<int32_t>(composition_len))));
    FLOAT x = 0.0f;
    FLOAT y = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestTextPosition(caret, FALSE, &x, &y, &metrics))) {
        return;
    }

    HIMC context = ImmGetContext(state->hwnd);
    if (!context) {
        return;
    }
    COMPOSITIONFORM form{};
    form.dwStyle = CFS_POINT;
    form.ptCurrentPos.x = static_cast<LONG>(state->padding_x_dip - state->scroll_x + x);
    form.ptCurrentPos.y = static_cast<LONG>(state->padding_y_dip - state->scroll_y + y);
    ImmSetCompositionWindow(context, &form);
    ImmReleaseContext(state->hwnd, context);
}

LRESULT HandleImeComposition(ControlState* state, LPARAM lparam) {
    HIMC context = ImmGetContext(state->hwnd);
    if (!context) {
        return 0;
    }

    if (lparam & GCS_COMPSTR) {
        const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
        std::wstring composition(static_cast<size_t>(std::max<LONG>(0, bytes)) / sizeof(wchar_t), L'\0');
        if (bytes > 0) {
            ImmGetCompositionStringW(context, GCS_COMPSTR, composition.data(), bytes);
        }
        state->ime_composing = true;
        state->ime_composition = std::move(composition);
        // Per ImmGetCompositionString docs, GCS_CURSORPOS returns the cursor
        // index directly (in WCHARs) rather than writing through a buffer.
        state->ime_composition_cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
        InvalidateBetterText(state);
    }

    if (!state->read_only && (lparam & GCS_RESULTSTR)) {
        const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
        if (bytes > 0) {
            std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes);
            BetterTextInsertText(state->hwnd, text.c_str());
        }
        state->ime_composition.clear();
        state->ime_composition_cursor = 0;
    }

    ImmReleaseContext(state->hwnd, context);
    if (lparam & GCS_COMPSTR) {
        UpdateImeCompositionWindowPos(state);
    }
    return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    ControlState* state = GetState(hwnd);

    switch (message) {
    case WM_NCCREATE: {
        auto* created = new ControlState();
        created->hwnd = hwnd;
        created->default_style = SystemDefaultTextStyle(hwnd);
        created->document.SetDefaultStyle(created->default_style);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        return TRUE;
    }
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateBetterText(state);
        return 0;
    case WM_SIZE:
        if (state && state->swap_chain) {
            const UINT width = LOWORD(lparam);
            const UINT height = HIWORD(lparam);
            ResizeRenderTarget(state, width, height);
        }
        if (state) {
            state->ResetVerticalCaretX();
            UpdateScrollInfo(state);
        }
        return 0;
    case WM_PAINT:
        if (state) {
            Paint(state);
            UpdateScrollInfo(state);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        return state ? HandleKeyDown(state, wparam) : 0;
    case WM_CHAR:
        return state ? HandleChar(state, wparam) : 0;
    case WM_IME_STARTCOMPOSITION:
        if (state) {
            state->ime_composing = true;
            state->ime_composition.clear();
            state->ime_composition_cursor = 0;
            UpdateImeCompositionWindowPos(state);
            InvalidateBetterText(state);
        }
        // Handled entirely ourselves (composition string is rendered inline
        // via D2D in Paint()) — do not let Windows draw its own default
        // composition overlay on top.
        return 0;
    case WM_IME_ENDCOMPOSITION:
        if (state) {
            state->ime_composing = false;
            state->ime_composition.clear();
            state->ime_composition_cursor = 0;
            InvalidateBetterText(state);
        }
        return 0;
    case WM_IME_COMPOSITION:
        return state ? HandleImeComposition(state, lparam) : 0;
    case WM_LBUTTONDOWN:
        if (state) {
            SetFocus(hwnd);
            SetCapture(hwnd);
            state->dragging = true;
            const int64_t pos = HitTest(state, static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            state->selection = { pos, pos };
            state->ResetVerticalCaretX();
            EnsureCaretVisibleHorizontally(state);
            InvalidateBetterText(state);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (state && state->dragging && (wparam & MK_LBUTTON)) {
            const int64_t pos = HitTest(state, static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            state->selection.caret = pos;
            state->ResetVerticalCaretX();
            EnsureCaretVisibleHorizontally(state);
            InvalidateBetterText(state);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state) {
            state->dragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (state) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            state->scroll_y -= static_cast<float>(delta) / WHEEL_DELTA * 48.0f;
            UpdateScrollInfo(state);
            InvalidateBetterText(state);
        }
        return 0;
    case WM_VSCROLL:
        if (state) {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &info);
            int pos = info.nPos;
            switch (LOWORD(wparam)) {
            case SB_LINEUP: pos -= 24; break;
            case SB_LINEDOWN: pos += 24; break;
            case SB_PAGEUP: pos -= static_cast<int>(info.nPage); break;
            case SB_PAGEDOWN: pos += static_cast<int>(info.nPage); break;
            case SB_THUMBTRACK: pos = info.nTrackPos; break;
            default: break;
            }
            state->scroll_y = static_cast<float>(std::clamp(pos, info.nMin, info.nMax));
            UpdateScrollInfo(state);
            InvalidateBetterText(state);
        }
        return 0;
    case WM_GETTEXT: {
        if (!state) {
            return 0;
        }
        int copied = 0;
        CopyStringToBuffer(state->document.PlainText(), reinterpret_cast<wchar_t*>(lparam), static_cast<int>(wparam), &copied);
        return copied;
    }
    case WM_GETTEXTLENGTH:
        return state ? state->document.PlainText().size() : 0;
    case WM_SETTEXT:
        return BetterTextSetText(hwnd, reinterpret_cast<const wchar_t*>(lparam));
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

} // namespace

ControlState* GetState(HWND hwnd) {
    return hwnd ? reinterpret_cast<ControlState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)) : nullptr;
}

BOOL RegisterBetterTextControl(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.cbWndExtra = 0;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = BETTERTEXT_CLASS_NAME;

    ATOM atom = RegisterClassExW(&wc);
    if (atom) {
        return TRUE;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? TRUE : FALSE;
}

void ResetRenderResources(ControlState* state) {
    if (!state) {
        return;
    }
    if (state->device_context) {
        state->device_context->SetTarget(nullptr);
    }
    state->target_bitmap.Reset();
    state->caret_brush.Reset();
    state->selection_brush.Reset();
    state->foreground_brush.Reset();
    state->placeholder_brush.Reset();
    state->device_context4.Reset();
    state->device_context.Reset();
    state->d2d_device.Reset();
    state->swap_chain.Reset();
    state->dxgi_device.Reset();
    state->d3d_device.Reset();
}

void NotifyChanged(ControlState* state) {
    if (state && state->notify_callback) {
        state->notify_callback(state->hwnd, BetterTextEvent_Changed, state->notify_user_data);
    }
}

void NotifySubmit(ControlState* state) {
    if (state && state->notify_callback) {
        state->notify_callback(state->hwnd, BetterTextEvent_Submit, state->notify_user_data);
    }
}

float ComputeContentHeight(ControlState* state) {
    return state ? LayoutHeight(state) : 0.0f;
}

bool GetCaretRect(ControlState* state, RECT* out) {
    if (!state || !out) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(CreateLayout(state, layout.GetAddressOf())) || !layout) {
        return false;
    }
    const UINT32 caret = static_cast<UINT32>(
        std::clamp<int64_t>(state->selection.caret, 0, static_cast<int64_t>(state->document.Length())));
    FLOAT x = 0.0f;
    FLOAT y = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestTextPosition(caret, FALSE, &x, &y, &metrics))) {
        return false;
    }
    const float left = state->padding_x_dip - state->scroll_x + x;
    const float top = state->padding_y_dip - state->scroll_y + y;
    out->left = static_cast<LONG>(left);
    out->top = static_cast<LONG>(top);
    out->right = static_cast<LONG>(left + 1.0f);
    out->bottom = static_cast<LONG>(top + metrics.height);
    return true;
}

void StoreResolvedImage(ControlState* state, const wchar_t* uri, IWICBitmapSource* bitmap) {
    if (!state || !uri || !bitmap || FAILED(EnsureFactories(state))) {
        return;
    }
    if (!state->wic_factory) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(state->wic_factory.GetAddressOf()));
    }
    if (!state->wic_factory) {
        return;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(state->wic_factory->CreateFormatConverter(converter.GetAddressOf()))) {
        return;
    }
    if (FAILED(converter->Initialize(bitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                      nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2d_bitmap;
    if (FAILED(state->device_context->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                                                                  d2d_bitmap.GetAddressOf()))) {
        return;
    }
    state->resolved_images[uri] = std::move(d2d_bitmap);
}

} // namespace bettertext
