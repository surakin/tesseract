#pragma once

#include "BetterText/BetterText.h"
#include "BetterTextDocument.h"

#include <d2d1_3.h>
#include <d3d11_1.h>
#include <dwrite_3.h>
#include <dxgi1_2.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace bettertext {

struct ControlState {
    HWND hwnd = nullptr;
    Document document;
    BetterTextSelection selection{ 0, 0 };
    bool read_only = false;
    bool dragging = false;
    float scroll_y = 0.0f;
    float scroll_x = 0.0f;
    bool vertical_caret_x_valid = false;
    float vertical_caret_x = 0.0f;
    uint64_t next_image_request = 1;

    bool single_line = false;
    bool submit_on_enter = false;
    bool password_mode = false;
    // Off by default — SetScrollInfo would otherwise auto-install a
    // scrollbar gutter regardless of whether the caller wants one. Mouse
    // wheel scrolling works independently of this flag.
    bool show_scrollbar = false;
    std::wstring placeholder;

    // Inset (DIPs) between the control's edges and its text/caret/selection
    // content. Split per-axis so a host can shrink vertical padding for a
    // compact single-line row without also tightening the horizontal inset
    // (see BetterTextSetPadding).
    float padding_x_dip = 8.0f;
    float padding_y_dip = 8.0f;

    BetterTextNotifyProc notify_callback = nullptr;
    void* notify_user_data = nullptr;

    // Inline IME composition (WM_IME_COMPOSITION / GCS_COMPSTR) — the
    // in-progress string is spliced into the rendered layout at the caret
    // but never touches `document` until GCS_RESULTSTR commits it.
    bool ime_composing = false;
    std::wstring ime_composition;
    int32_t ime_composition_cursor = 0;

    BetterTextTheme theme{
        0xffffffff,
        0x111111ff,
        0x0067c0aa,
        0x111111ff,
        0x777777ff,
    };

    TextStyle default_style;

    IBetterTextImageProvider* image_provider = nullptr;
    IBetterTextClipboardAdapter* clipboard_adapter = nullptr;
    IBetterTextFontProvider* font_provider = nullptr;

    std::vector<Document> undo_stack;
    std::vector<Document> redo_stack;

    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext4> device_context4;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_bitmap;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory;
    Microsoft::WRL::ComPtr<IDWriteFactory4> dwrite_factory4;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> foreground_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selection_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> caret_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> placeholder_brush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> emoji_font_collection;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory;

    // Bitmaps resolved via IBetterTextImageProvider, keyed by the URI passed
    // to BetterTextInsertImageUri / BetterTextNotifyImageResolved. Multiple
    // image runs sharing a URI (e.g. the same custom emoji inserted twice)
    // share one entry.
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> resolved_images;

    void PushUndo();
    void ClearRedo();
    void ClampSelection();
    void ResetVerticalCaretX();
};

ControlState* GetState(HWND hwnd);
BOOL RegisterBetterTextControl(HINSTANCE instance);
void InvalidateBetterText(ControlState* state);
void ResetRenderResources(ControlState* state);
bool CopyStringToBuffer(const std::wstring& value, wchar_t* buffer, int buffer_length, int* copied);
TextStyle ToInternalStyle(const BetterTextTextStyle* style, const TextStyle& fallback);
void ToPublicStyle(const TextStyle& style, BetterTextTextStyle* public_style);
void NotifyChanged(ControlState* state);
void NotifySubmit(ControlState* state);
float ComputeContentHeight(ControlState* state);
bool GetCaretRect(ControlState* state, RECT* out);
void StoreResolvedImage(ControlState* state, const wchar_t* uri, IWICBitmapSource* bitmap);

} // namespace bettertext
