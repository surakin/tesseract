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
    // Set only via BetterTextSetStatic, which also forces read_only,
    // caret_visible=false, show_scrollbar=false, and single_line=true —
    // static_mode itself only drives CreateLayout()'s ellipsis trimming
    // (see there); nothing else needs to consult it directly, since those
    // other flags already fully suppress selection/caret/scrollbar
    // rendering and document mutation on their own.
    bool static_mode = false;
    // BetterText has no internal blink cycle — hosts that drive their own
    // blink timer (see BetterTextSetCaretVisible) toggle this each tick.
    // Paint() only draws the caret when this is true (and focused).
    bool caret_visible = true;
    // Selects which render-target backend this control uses. True (default)
    // is the original on-screen path: a DXGI swap chain via CreateSwapChain,
    // presented every Paint() — exactly what the library's own demo app
    // still relies on to render at all, since it never calls
    // BetterTextSetPresentEnabled or any capture API. False is for hosts
    // that never show this control's HWND on screen at all (e.g.
    // Tesseract's Win32 shell, which keeps it SetWindowRgn(empty) and reads
    // frames via BetterTextRequestCaptureBGRA/ReadCaptureBGRA instead): the
    // render target becomes a plain offscreen texture (see
    // ControlState::offscreen_target) with no swap chain and nothing ever
    // presented. This used to be just a "skip Present()" flag, but a
    // flip-model swap chain's Present() still reaches the display
    // compositor on real hardware even when the owning HWND's region is
    // empty (unlike WARP/RDP's software path, where it's a no-op) —
    // observed as visible flicker of the whole top-level window — so
    // off-screen hosts now avoid the swap chain entirely rather than merely
    // skipping the Present call on one.
    bool present_enabled = true;
    // Set by InvalidateBetterText (and anything else that changes what
    // Paint() would draw — see BetterTextSetCaretVisible) and cleared by
    // CaptureBGRA once it has actually repainted. Lets CaptureBGRA skip a
    // redundant Paint()+GPU-readback when nothing changed since the last
    // capture (e.g. a caller re-querying the same frame twice, or a host
    // polling on a timer with nothing new to show) instead of unconditionally
    // repainting every call. Starts true so the very first capture always
    // renders.
    bool content_dirty = true;
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
    // The render target when present_enabled is false — a plain
    // D3D11_BIND_RENDER_TARGET texture with no HWND/swap-chain association
    // (see CreateOffscreenTarget), sized identically to how CreateSwapChain
    // would size a swap chain buffer. Mutually exclusive with swap_chain:
    // exactly one of the two exists at a time, chosen by present_enabled.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> offscreen_target;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext4> device_context4;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_bitmap;
    // True once target_bitmap was successfully created with
    // D2D1_ALPHA_MODE_PREMULTIPLIED (see CreateTargetBitmap) instead of the
    // D2D1_ALPHA_MODE_IGNORE fallback — tells CaptureBGRA whether the alpha
    // byte it reads off the swap chain's back buffer is real per-pixel
    // coverage data (safe to pass through) or meaningless padding (must
    // stay forced to opaque, since IGNORE mode gives no guarantee about
    // what ends up in those bits).
    bool target_alpha_premultiplied = false;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory;
    Microsoft::WRL::ComPtr<IDWriteFactory4> dwrite_factory4;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> foreground_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selection_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> caret_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> placeholder_brush;
    // Overlay scroll-thumb drawn directly into the D2D content — see
    // DrawScrollbarThumb's doc comment for why this can't be the OS's own
    // SB_VERT scrollbar (show_scrollbar above).
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> scrollbar_brush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> emoji_font_collection;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory;

    // Bitmaps resolved via IBetterTextImageProvider, keyed by the URI passed
    // to BetterTextInsertImageUri / BetterTextNotifyImageResolved. Multiple
    // image runs sharing a URI (e.g. the same custom emoji inserted twice)
    // share one entry.
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> resolved_images;

    // GPU-readback pipeline for RequestCaptureBGRA/ReadCaptureBGRA — a
    // persistent CPU-readable staging texture, recreated only when the
    // source render target's size changes, instead of one allocated fresh
    // per call.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> readback_staging;
    UINT readback_staging_width = 0;
    UINT readback_staging_height = 0;
    // True from a successful RequestCaptureBGRA (CopyResource submitted)
    // until ReadCaptureBGRA either reads it or hits an unrecoverable Map()
    // failure. Guards against calling ReadCaptureBGRA without a prior
    // Request.
    bool readback_pending = false;

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
void ScrollBy(ControlState* state, float delta_dip);
bool GetCaretRect(ControlState* state, RECT* out);
bool RequestCaptureBGRA(ControlState* state, int* out_width, int* out_height);
bool ReadCaptureBGRA(ControlState* state, uint8_t* buffer, int buffer_size, int* out_width, int* out_height);
void StoreResolvedImage(ControlState* state, const wchar_t* uri, IWICBitmapSource* bitmap);

} // namespace bettertext
