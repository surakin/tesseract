#include "host_win32.h"
#include "anim_image_cache.h"
#include "canvas_d2d.h"
#include "controls.h"

#include <tesseract/settings.h>

#include <commctrl.h>
#include <d2d1.h>           // ID2D1HwndRenderTarget, D2D1_RENDER_TARGET_PROPERTIES
#include <d2d1_1.h>         // ID2D1Factory1, ID2D1DeviceContext, ID2D1Bitmap1
#include <d2d1_1helper.h>   // D2D1::BitmapProperties1
#include <d3d11.h>          // ID3D11Device (for swap chain creation)
#include <dxgi1_2.h>        // IDXGISwapChain1, IDXGIFactory2
#include <dwrite.h>         // IDWriteTextFormat, DWRITE_FONT_WEIGHT_* enums
#include <dwrite_2.h>       // IDWriteFactory2::CreateTextFormat
#include <richedit.h>  // MSFTEDIT_CLASS, RichEdit ES_* flags
#include <textserv.h>  // ITextHost2, ITextServices2, TXTBIT_*, TXTNS_*
#include "win32_textserv2_compat.h"  // ITextHost2/ITextServices2 shim for mingw-w64
#include <imm.h>       // ImmGetContext / ImmReleaseContext
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM

// Typography flags added in later Windows 10 SDKs; define if absent.
#ifndef TO_DEFAULTCOLOREMOJI
#define TO_DEFAULTCOLOREMOJI 0x1000
#endif
#ifndef TO_DISPLAYFONTCOLOR
#define TO_DISPLAYFONTCOLOR 0x2000
#endif
#include <wincodec.h>
#include <objidl.h>
#include <ole2.h>     // RegisterDragDrop / IDropTarget
#include <shellapi.h> // CF_HDROP / DragQueryFileW
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tk::win32
{

// ─────────────────────────────────────────────────────────────────────────
//  Process-wide D2D backend + post-to-UI message
// ─────────────────────────────────────────────────────────────────────────

// External linkage (declared in host_win32.h): the WIC factory it owns is
// free-threaded, so worker threads can call decode_image / decode_animation
// against this backend without marshalling to the UI thread.
d2d::Backend& backend_singleton()
{
    static d2d::Backend instance;
    return instance;
}

namespace
{

// One registered window message per process for the post_to_ui channel.
// The lParam is a heap-allocated std::function<void()>* that the
// receiving WndProc invokes and frees.
UINT post_to_ui_message()
{
    static UINT msg = RegisterWindowMessageW(L"tk_post_to_ui");
    return msg;
}

// Process-wide font for native EDIT overlays — matches FontRole::Body
// ("Segoe UI Variable Text", Settings::font_body pt, regular weight) so
// text input fields render in the same face and size as message body text.
// On systems without "Segoe UI Variable Text" (pre-Win11) GDI silently
// substitutes "Segoe UI" at the same size, which is visually identical.
HFONT body_font()
{
    static HFONT cached = []() -> HFONT
    {
        const int pt = tesseract::Settings::instance().font_body;
        HDC hdc = GetDC(nullptr);
        int h = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        ReleaseDC(nullptr, hdc);
        LOGFONTW lf{};
        lf.lfHeight = h;
        lf.lfWeight = FW_REGULAR;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
        HFONT font = CreateFontIndirectW(&lf);
        return font ? font
                    : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }();
    return cached;
}

// Converters.
inline std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

inline std::string wide_to_utf8(const std::wstring& s)
{
    if (s.empty())
    {
        return {};
    }
    int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// ── Clipboard image extraction ────────────────────────────────────────────
//
// Read CF_DIBV5 or CF_DIB from the Windows clipboard, decode through WIC,
// and re-encode as PNG so the shared layer doesn't need to understand DIB
// memory layouts. The output mime is always "image/png" because we lose
// the source identity when transcoding from a DIB.
inline bool clipboard_image_to_png(IWICImagingFactory* wic, HWND owner,
                                   std::vector<std::uint8_t>& out)
{
    if (!OpenClipboard(owner))
    {
        return false;
    }
    struct CloseGuard
    {
        ~CloseGuard()
        {
            CloseClipboard();
        }
    } guard;

    UINT fmt = 0;
    if (IsClipboardFormatAvailable(CF_DIBV5))
    {
        fmt = CF_DIBV5;
    }
    else if (IsClipboardFormatAvailable(CF_DIB))
    {
        fmt = CF_DIB;
    }
    else
    {
        return false;
    }

    HGLOBAL hg = GetClipboardData(fmt);
    if (!hg)
    {
        return false;
    }
    SIZE_T sz = GlobalSize(hg);
    void* data = GlobalLock(hg);
    if (!data || sz == 0)
    {
        if (data)
        {
            GlobalUnlock(hg);
        }
        return false;
    }

    // A CF_DIB/CF_DIBV5 payload starts with a BITMAPINFOHEADER (or V5
    // header) followed by colour table + pixel data. WIC's
    // CreateDecoderFromStream needs a full BMP file (with file header).
    // Synthesize a 14-byte BITMAPFILEHEADER in front of the DIB.
    std::vector<std::uint8_t> bmp;
    bmp.resize(sizeof(BITMAPFILEHEADER) + sz);
    BITMAPFILEHEADER* bfh = reinterpret_cast<BITMAPFILEHEADER*>(bmp.data());
    bfh->bfType = 0x4D42; // 'BM'
    bfh->bfSize = static_cast<DWORD>(bmp.size());
    bfh->bfReserved1 = 0;
    bfh->bfReserved2 = 0;

    const BITMAPINFOHEADER* bih =
        reinterpret_cast<const BITMAPINFOHEADER*>(data);
    DWORD header_size = bih->biSize;
    // Colour table for paletted / bitfields formats.
    DWORD palette_bytes = 0;
    if (bih->biBitCount <= 8)
    {
        DWORD entries =
            bih->biClrUsed ? bih->biClrUsed : (1u << bih->biBitCount);
        palette_bytes = entries * sizeof(RGBQUAD);
    }
    else if (bih->biCompression == BI_BITFIELDS)
    {
        palette_bytes = 3 * sizeof(DWORD);
    }
    bfh->bfOffBits = sizeof(BITMAPFILEHEADER) + header_size + palette_bytes;
    std::memcpy(bmp.data() + sizeof(BITMAPFILEHEADER), data, sz);
    GlobalUnlock(hg);

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.GetAddressOf())))
    {
        return false;
    }
    if (FAILED(stream->InitializeFromMemory(bmp.data(),
                                            static_cast<DWORD>(bmp.size()))))
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                            WICDecodeMetadataCacheOnLoad,
                                            decoder.GetAddressOf())))
    {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
    {
        return false;
    }

    // Encode to PNG into an in-memory IStream.
    ComPtr<IStream> mem_stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, mem_stream.GetAddressOf())))
    {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                  encoder.GetAddressOf())))
    {
        return false;
    }
    if (FAILED(encoder->Initialize(mem_stream.Get(), WICBitmapEncoderNoCache)))
    {
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> out_frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(out_frame.GetAddressOf(),
                                       props.GetAddressOf())))
    {
        return false;
    }
    if (FAILED(out_frame->Initialize(nullptr)))
    {
        return false;
    }
    if (FAILED(out_frame->WriteSource(frame.Get(), nullptr)))
    {
        return false;
    }
    if (FAILED(out_frame->Commit()))
    {
        return false;
    }
    if (FAILED(encoder->Commit()))
    {
        return false;
    }

    // Read the encoded bytes back from the stream.
    HGLOBAL h_out = nullptr;
    if (FAILED(GetHGlobalFromStream(mem_stream.Get(), &h_out)) || !h_out)
    {
        return false;
    }
    SIZE_T n = GlobalSize(h_out);
    void* p = GlobalLock(h_out);
    if (!p || n == 0)
    {
        if (p)
        {
            GlobalUnlock(h_out);
        }
        return false;
    }
    out.assign(static_cast<const std::uint8_t*>(p),
               static_cast<const std::uint8_t*>(p) + n);
    GlobalUnlock(h_out);
    return true;
}

// ── UTF-8 byte offset to UTF-16 code unit count ──────────────────────────
// Used by both Win32NativeTextArea and Win32RichEditArea for replace_range().
// Returns the number of UTF-16 code units in the first `byte_offset` bytes
// of `utf8`; clamped to [0, utf8.size()].
static int utf8_byte_to_utf16_len(const std::string& utf8, int byte_offset)
{
    byte_offset = std::clamp(byte_offset, 0, (int)utf8.size());
    int wlen =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), byte_offset, nullptr, 0);
    return wlen < 0 ? 0 : wlen;
}

// ITextHost / ITextHost2 / ITextServices2 IIDs extracted from MSFTEDIT.DLL
// exports (no import lib ships with the Windows SDK; textserv.h declares them
// as EXTERN_C const IID without providing a definition).
//
// Verified against MSFTEDIT.DLL 10.0.26100 via dumpbin:
//   ITextHost      RVA 0x2B34D8 → {13e670f4-1a5a-11cf-abeb-00aa00b65ea1}
//   ITextHost2     RVA 0x2AE2E8 → {13e670f5-1a5a-11cf-abeb-00aa00b65ea1}
//   ITextServices  RVA 0x2AE210 → {8d33f740-cf58-11ce-a89d-00aa006cadc5}
//   ITextServices2 RVA 0x2AE1C0 → {8d33f741-cf58-11ce-a89d-00aa006cadc5}
static constexpr GUID kIID_ITextHost = {
    0x13e670f4,
    0x1a5a,
    0x11cf,
    {0xab, 0xeb, 0x00, 0xaa, 0x00, 0xb6, 0x5e, 0xa1}};
static constexpr GUID kIID_ITextHost2 = {
    0x13e670f5,
    0x1a5a,
    0x11cf,
    {0xab, 0xeb, 0x00, 0xaa, 0x00, 0xb6, 0x5e, 0xa1}};
static constexpr GUID kIID_ITextServices2 = {
    0x8d33f741,
    0xcf58,
    0x11ce,
    {0xa8, 0x9d, 0x00, 0xaa, 0x00, 0x6c, 0xad, 0xc5}};

// IProvideFontInfo — undocumented MSFTEDIT host interface for injecting custom
// IDWriteFontFace pointers per text run.  Documented in:
// https://learn.microsoft.com/en-us/archive/blogs/murrays/richedit-8-feature-additions
// GUID confirmed by QI logging against MSFTEDIT.DLL 10.0.26100.
static constexpr GUID kIID_IProvideFontInfo = {
    0x7502135b,
    0x17c1,
    0x4a25,
    {0xbd, 0xc9, 0x55, 0xe6, 0xbc, 0xb8, 0x59, 0x8a}};

// IProvideFontInfo COM interface — vtable layout must match MSFTEDIT exactly.
// Inherits IUnknown; 4 additional methods in the order the blog documents.
struct __declspec(novtable) IProvideFontInfo : IUnknown
{
    // Return the default font family name for the control.
    virtual BSTR STDMETHODCALLTYPE GetDefaultFont() = 0;

    // Return a font face ID for the text run starting at pText[0..cch-1].
    // On entry, fontFaceIdCurrent is the face ID already in use (0 = none).
    // On exit, set runCount = number of UTF-16 code units in the homogeneous
    // run (all using the same returned face ID).
    virtual DWORD STDMETHODCALLTYPE GetRunFontFaceId(
        const wchar_t*      pCurrentFontFamilyName,
        DWRITE_FONT_WEIGHT  weight,
        DWRITE_FONT_STRETCH stretch,
        DWRITE_FONT_STYLE   style,
        LCID                lcid,
        const wchar_t*      pText,
        UINT                cch,
        DWORD               fontFaceIdCurrent,
        UINT&               runCount) = 0;

    // Return the IDWriteFontFace* for the given ID (caller should Release).
    // Return nullptr to use the control's default font.
    virtual IDWriteFontFace* STDMETHODCALLTYPE GetFontFace(DWORD fontFaceId) = 0;

    // Return a font family name suitable for RTF serialisation.
    virtual BSTR STDMETHODCALLTYPE GetSerializableFontName(DWORD fontFaceId) = 0;
};

// ── Emoji codepoint classifier ─────────────────────────────────────────────
// Used by Win32RichEditArea::GetRunFontFaceId to detect emoji text runs and
// route them to the Noto Color Emoji IDWriteFontFace via IProvideFontInfo.
static bool is_emoji_codepoint(char32_t cp)
{
    // All supplementary-plane emoji blocks (U+1F000 and above).
    if (cp >= 0x1F000u) return true;
    // Miscellaneous Symbols (☀ ☁ ☂ ♠ ♣ …)
    if (cp >= 0x2600u && cp <= 0x26FFu) return true;
    // Dingbats (✂ ✈ ✉ …)
    if (cp >= 0x2700u && cp <= 0x27BFu) return true;
    // Miscellaneous Symbols and Arrows (⬅ ⬆ ⬇ ⬛ ⬜ ⭐ …)
    if (cp >= 0x2B00u && cp <= 0x2BFFu) return true;
    // Enclosed Alphanumerics (🅰 🅱 🅾 🅿 …)
    if (cp >= 0x24C2u && cp <= 0x24C2u) return true;
    // © and ®
    if (cp == 0x00A9u || cp == 0x00AEu) return true;
    // ™ and ℹ
    if (cp == 0x2122u || cp == 0x2139u) return true;
    return false;
}

// Face ID constants used by Win32RichEditArea::IProvideFontInfo implementation.
// 0 = let RichEdit choose the face (return fontFaceIdCurrent unchanged).
// 1 = Noto Color Emoji (GetFontFace returns noto_face_).
static constexpr DWORD kNotoFaceId = 1u;

// Blink timer ID used by Win32RichEditArea for the D2D caret.
// Must not clash with MSFTEDIT's own timer IDs (typically 1–4).
static constexpr UINT kRichEditCaretTimerId = 0xCAFEu;

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Win32TextAreaBase — internal base stored in Host::areas_by_id_
// ─────────────────────────────────────────────────────────────────────────
//
// Both Win32NativeTextArea (RICHEDIT50W child HWND) and Win32RichEditArea
// (windowless ITextServices2 host HWND) inherit from this so the Host can
// store either in areas_by_id_ and call notify_changed() uniformly.

class Win32TextAreaBase
{
public:
    virtual ~Win32TextAreaBase() = default;
    virtual void notify_changed() = 0;
    virtual int  ctrl_id() const = 0;
    virtual HWND hwnd()    const = 0;
    virtual void on_theme_changed(const Theme& /*t*/) {}
};

// ─────────────────────────────────────────────────────────────────────────
//  Win32NativeTextField — EDIT-control NativeTextField
// ─────────────────────────────────────────────────────────────────────────
//
// One EDIT child window per make_text_field(). The EDIT is subclassed so
// VK_RETURN raises on_submit_ rather than getting eaten silently.
// EN_CHANGE notifications arrive at the parent surface as WM_COMMAND;
// the surface forwards them to the right NativeTextField by control ID.

class Win32NativeTextField : public NativeTextField
{
public:
    Win32NativeTextField(HWND parent, int ctrl_id)
        : parent_(parent), id_(ctrl_id)
    {
        hwnd_ = CreateWindowExW(
            0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 100, 24, parent_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_)),
            reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
            nullptr);
        if (!hwnd_)
        {
            return;
        }
        SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(body_font()),
                     FALSE);
        SetWindowSubclass(hwnd_, &Win32NativeTextField::subclass_proc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(hwnd_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELONG(8, 8));
        // Measure the natural one-line height for vertical centering in set_rect.
        HDC hdc = GetDC(hwnd_);
        HGDIOBJ old = SelectObject(hdc, body_font());
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        line_h_ = tm.tmHeight + 4; // 2 px internal top+bottom margin
    }

    ~Win32NativeTextField() override
    {
        if (hwnd_)
        {
            RemoveWindowSubclass(hwnd_, &Win32NativeTextField::subclass_proc,
                                 1);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void set_rect(Rect r) override
    {
        if (!hwnd_)
        {
            return;
        }
        if (r.x == last_rect_.x && r.y == last_rect_.y && r.w == last_rect_.w &&
            r.h == last_rect_.h)
        {
            return;
        }
        last_rect_ = r;
        const float s = dip_scale();
        int x  = static_cast<int>(std::floor(r.x * s));
        int w  = static_cast<int>(std::round(r.w * s));
        int rh = static_cast<int>(std::round(r.h * s));
        // Single-line EDIT controls draw text top-aligned within their HWND.
        // Size the HWND to the measured line height (physical px) and centre
        // it vertically within the physical rect so text appears centred.
        int h = (line_h_ > 0) ? line_h_ : rh;
        int y = static_cast<int>(std::floor(r.y * s)) + (rh - h) / 2;
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void set_text(std::string text) override
    {
        if (!hwnd_)
        {
            return;
        }
        suppress_changed_ = true;
        std::wstring w = utf8_to_wide(text);
        SetWindowTextW(hwnd_, w.c_str());
        suppress_changed_ = false;
    }
    std::string text() const override
    {
        if (!hwnd_)
        {
            return {};
        }
        int len = GetWindowTextLengthW(hwnd_);
        if (len <= 0)
        {
            return {};
        }
        std::wstring w(len, L'\0');
        GetWindowTextW(hwnd_, w.data(), len + 1);
        return wide_to_utf8(w);
    }
    void set_placeholder(std::string text) override
    {
        if (!hwnd_)
        {
            return;
        }
        std::wstring w = utf8_to_wide(text);
        // EM_SETCUEBANNER lives in commctrl.h; available on XP SP1+.
        SendMessageW(hwnd_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(w.c_str()));
    }
    void set_focused(bool focused) override
    {
        if (hwnd_ && focused)
        {
            SetFocus(hwnd_);
        }
    }
    void set_visible(bool visible) override
    {
        if (hwnd_)
        {
            ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
        }
    }
    void set_enabled(bool enabled) override
    {
        if (hwnd_)
        {
            EnableWindow(hwnd_, enabled ? TRUE : FALSE);
        }
    }
    void set_password(bool password) override
    {
        if (!hwnd_)
        {
            return;
        }
        // EM_SETPASSWORDCHAR is the universal toggle: setting '*' masks
        // the buffer, 0 clears the mask and shows plaintext.
        SendMessageW(hwnd_, EM_SETPASSWORDCHAR,
                     static_cast<WPARAM>(password ? L'•' : 0), 0);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }
    void set_on_popup_nav(std::function<bool(NavKey)> cb) override
    {
        popup_nav_ = std::move(cb);
    }

    // Called by Surface's WndProc on WM_COMMAND with EN_CHANGE.
    void notify_changed()
    {
        if (suppress_changed_ || !on_changed_)
        {
            return;
        }
        on_changed_(text());
    }
    int ctrl_id() const
    {
        return id_;
    }
    HWND hwnd() const
    {
        return hwnd_;
    }

private:
    static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR /*id*/,
                                          DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<Win32NativeTextField*>(ref);
        if (msg == WM_NCPAINT)
        {
            return 0;
        }
        // Up / Down / Escape navigation forwarded to a popup the field drives
        // (the Ctrl+K quick switcher), mirroring the multi-line variant.
        if (msg == WM_KEYDOWN && self->popup_nav_)
        {
            NavKey nk{};
            bool is_nav = true;
            if (wParam == VK_UP)
            {
                nk = NavKey::Up;
            }
            else if (wParam == VK_DOWN)
            {
                nk = NavKey::Down;
            }
            else if (wParam == VK_ESCAPE)
            {
                nk = NavKey::Escape;
            }
            else
            {
                is_nav = false;
            }
            // Copy to keep the closure alive across re-entrant mutation.
            auto nav = self->popup_nav_;
            if (is_nav && nav && nav(nk))
            {
                return 0;
            }
        }
        if (msg == WM_KEYDOWN && wParam == VK_RETURN)
        {
            if (self->on_submit_)
            {
                self->on_submit_();
            }
            return 0;
        }
        // Tell the parent we want all char input forwarded for Enter, so
        // the default beep on VK_RETURN doesn't fire.
        if (msg == WM_GETDLGCODE)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            return r | DLGC_WANTALLKEYS;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    float dip_scale() const
    {
        const float dpi = static_cast<float>(GetDpiForWindow(parent_));
        return dpi > 0.f ? dpi / 96.f : 1.f;
    }

    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    int id_ = 0;
    int line_h_ = 0;
    bool suppress_changed_ = false;
    Rect last_rect_ = {-1.f, -1.f, -1.f, -1.f};
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<bool(NavKey)> popup_nav_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Win32NativeTextArea — multi-line EDIT control overlay
// ─────────────────────────────────────────────────────────────────────────
//
// Multi-line EDIT (ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL). The
// subclass swallows Enter (without Shift) and raises on_submit; Shift+
// Enter falls through and inserts a newline. Natural height comes from a
// CalcTextSize-ish measurement: line count × DT_HEIGHT_OF_FIRST_LINE.

class Win32NativeTextArea : public NativeTextArea, public Win32TextAreaBase
{
public:
    Win32NativeTextArea(HWND parent, int ctrl_id,
                        IWICImagingFactory* wic = nullptr)
        : parent_(parent), id_(ctrl_id), wic_(wic)
    {
        // MSFTEDIT.DLL must be loaded before RICHEDIT50W windows are created.
        // Load it once per process; RICHEDIT50W uses DirectWrite for rendering
        // (unlike the legacy EDIT control which uses GDI) so colour emoji and
        // other OpenType colour glyphs render correctly in the compose bar.
        static const HMODULE s_msftedit = LoadLibraryW(L"MSFTEDIT.DLL");
        (void)s_msftedit;

        hwnd_ =
            CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                            WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                ES_AUTOVSCROLL | ES_LEFT | ES_WANTRETURN,
                            0, 0, 200, 40, parent_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_)),
                            reinterpret_cast<HINSTANCE>(
                                GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
                            nullptr);
        if (!hwnd_)
        {
            return;
        }
        SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(body_font()),
                     FALSE);
        SetWindowSubclass(hwnd_, &Win32NativeTextArea::subclass_proc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(hwnd_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELONG(8, 8));
    }

    ~Win32NativeTextArea() override
    {
        if (hwnd_)
        {
            RemoveWindowSubclass(hwnd_, &Win32NativeTextArea::subclass_proc, 1);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void set_rect(Rect r) override
    {
        if (!hwnd_)
        {
            return;
        }
        if (r.x == last_rect_.x && r.y == last_rect_.y && r.w == last_rect_.w &&
            r.h == last_rect_.h)
        {
            return;
        }
        last_rect_ = r;
        // Multi-line EDIT controls draw text top-aligned within their
        // HWND. Size the HWND to the content's natural height and centre
        // it within the rect (matching Win32NativeTextField); when content
        // overflows the rect, fill it so the control scrolls instead.
        // r is in logical pixels; SetWindowPos needs physical pixels.
        const float s = dip_scale();
        int rh = static_cast<int>(std::round(r.h * s));
        int nh = static_cast<int>(std::round(natural_height() * s));
        // Keep one physical pixel of clearance at the top and bottom so
        // the parent surface's card border stroke is never covered.
        const int border_px = std::max(2, static_cast<int>(std::ceil(s)));
        const int max_h = std::max(1, rh - 2 * border_px);
        int h  = (nh > 0 && nh <= max_h) ? nh : max_h;
        int y  = static_cast<int>(std::floor(r.y * s)) + (rh - h) / 2;
        SetWindowPos(hwnd_, nullptr,
                     static_cast<int>(std::floor(r.x * s)), y,
                     static_cast<int>(std::round(r.w * s)), h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void set_text(std::string text) override
    {
        if (!hwnd_)
        {
            return;
        }
        suppress_changed_ = true;
        std::wstring w = utf8_to_wide(text);
        SetWindowTextW(hwnd_, w.c_str());
        suppress_changed_ = false;
        mentions_.clear();
        float h = natural_height();
        if (h != last_height_ && on_height_changed_)
        {
            last_height_ = h;
            on_height_changed_(h);
        }
    }
    std::string text() const override
    {
        if (!hwnd_)
        {
            return {};
        }
        int len = GetWindowTextLengthW(hwnd_);
        if (len <= 0)
        {
            return {};
        }
        std::wstring w(len, L'\0');
        GetWindowTextW(hwnd_, w.data(), len + 1);
        return wide_to_utf8(w);
    }
    void set_placeholder(std::string text) override
    {
        // RichEdit (RICHEDIT50W) does not support EM_SETCUEBANNER.  Store the
        // placeholder text and draw it manually in WM_PAINT when the control
        // is empty and unfocused (see subclass_proc below).
        placeholder_ = utf8_to_wide(text);
        if (hwnd_)
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    void set_focused(bool focused) override
    {
        if (hwnd_ && focused)
        {
            SetFocus(hwnd_);
        }
    }
    void set_visible(bool visible) override
    {
        visible_ = visible;
        if (hwnd_)
        {
            ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
        }
    }
    bool visible() const override
    {
        return visible_;
    }
    void set_enabled(bool enabled) override
    {
        if (hwnd_)
        {
            EnableWindow(hwnd_, enabled ? TRUE : FALSE);
        }
    }
    float natural_height() const override
    {
        if (!hwnd_)
        {
            return 0.f;
        }

        HDC hdc = GetDC(hwnd_);
        HFONT font =
            reinterpret_cast<HFONT>(SendMessageW(hwnd_, WM_GETFONT, 0, 0));
        HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        int per_line = tm.tmHeight + tm.tmExternalLeading;

        // EM_GETLINECOUNT only reflects the control's internal layout,
        // which updates a keystroke *after* the text changes — so a wrap
        // (or unwrap) made the compose box auto-grow one keystroke late.
        // Instead measure the wrapped text directly with DrawText: it
        // depends only on the text + wrap width, so it's exact and never
        // lags. The wrap width is the EDIT's formatting rectangle, which
        // is the same limiting rect the control itself wraps within
        // (already inset by the EC_LEFTMARGIN/EC_RIGHTMARGIN margins).
        RECT fr{};
        SendMessageW(hwnd_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&fr));
        int wrap_w = fr.right - fr.left;

        int len = GetWindowTextLengthW(hwnd_);
        int total_h;
        if (len <= 0 || wrap_w <= 0)
        {
            total_h = per_line; // one empty line
        }
        else
        {
            std::wstring buf(len, L'\0');
            GetWindowTextW(hwnd_, buf.data(), len + 1);
            RECT calc{0, 0, wrap_w, 0};
            // DT_EDITCONTROL: wrap exactly like a multi-line EDIT.
            // DT_EXTERNALLEADING: count external leading per line, as
            // the EDIT does. DT_NOPREFIX: keep '&' literal.
            DrawTextW(hdc, buf.c_str(), len, &calc,
                      DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL |
                          DT_EXTERNALLEADING | DT_NOPREFIX);
            total_h = calc.bottom - calc.top;
            // DT_CALCRECT ignores a trailing newline, but the EDIT shows
            // a blank line there for the caret — mirror that.
            if (buf[len - 1] == L'\n')
            {
                total_h += per_line;
            }
            if (total_h < per_line)
            {
                total_h = per_line;
            }
        }

        if (old)
        {
            SelectObject(hdc, old);
        }
        ReleaseDC(hwnd_, hdc);
        // total_h is in physical pixels; return logical (DIP) pixels.
        return static_cast<float>(total_h + 8) / dip_scale();
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }
    void set_on_height_changed(std::function<void(float)> cb) override
    {
        on_height_changed_ = std::move(cb);
    }
    void set_on_image_paste(ImagePasteHandler cb) override
    {
        on_image_paste_ = std::move(cb);
    }
    void insert_at_cursor(std::string text) override
    {
        if (!hwnd_)
        {
            return;
        }
        std::wstring w = utf8_to_wide(text);
        SendMessageW(hwnd_, EM_REPLACESEL, TRUE,
                     reinterpret_cast<LPARAM>(w.c_str()));
    }

    tk::Rect cursor_rect() const override
    {
        if (!hwnd_)
        {
            return {};
        }
        DWORD sel_start = 0;
        SendMessageW(hwnd_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), 0);

        TEXTMETRICW tm{};
        HDC hdc = GetDC(hwnd_);
        GetTextMetricsW(hdc, &tm);
        ReleaseDC(hwnd_, hdc);

        // RichEdit (RICHEDIT50W) uses a different EM_POSFROMCHAR signature from
        // the legacy EDIT control:
        //   EDIT:     SendMessage(hwnd, EM_POSFROMCHAR, charIndex, 0)
        //             → MAKELONG(x, y), or -1 on failure
        //   RichEdit: SendMessage(hwnd, EM_POSFROMCHAR, (WPARAM)POINTL*, charIndex)
        //             → 0 on success, -1 on failure; coordinates written into POINTL
        // Helper lambda wraps the RichEdit form.
        auto pos_from_char = [&](DWORD idx, int& out_x, int& out_y) -> bool
        {
            POINTL pt{};
            LRESULT r = SendMessageW(hwnd_, EM_POSFROMCHAR,
                                     reinterpret_cast<WPARAM>(&pt),
                                     static_cast<LPARAM>(idx));
            if (r == -1)
            {
                return false;
            }
            out_x = static_cast<int>(pt.x);
            out_y = static_cast<int>(pt.y);
            return true;
        };

        int cx = 0, cy = 0;
        if (pos_from_char(sel_start, cx, cy))
        {
            // got valid coordinates
        }
        else if (sel_start > 0)
        {
            // Anchor to the previous glyph and step right by its advance.
            int px = 0, py = 0;
            if (pos_from_char(sel_start - 1, px, py))
            {
                cx = px + tm.tmAveCharWidth;
                cy = py;
            }
            else
            {
                RECT fr{};
                SendMessageW(hwnd_, EM_GETRECT, 0,
                             reinterpret_cast<LPARAM>(&fr));
                cx = fr.left;
                cy = fr.top;
            }
        }
        else
        {
            // Empty control, or caret at the very start.
            RECT fr{};
            SendMessageW(hwnd_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&fr));
            cx = fr.left;
            cy = fr.top;
        }

        POINT pt{cx, cy};
        MapWindowPoints(hwnd_, GetParent(hwnd_), &pt, 1);
        return {float(pt.x), float(pt.y), 1.0f, float(tm.tmHeight)};
    }

    void replace_range(int start, int end, std::string utf8_text) override
    {
        if (!hwnd_)
        {
            return;
        }
        std::string current = this->text();
        int w_start = utf8_byte_to_utf16_len(current, start);
        int w_end = utf8_byte_to_utf16_len(current, end);
        suppress_changed_ = true;
        SendMessageW(hwnd_, EM_SETSEL, w_start, w_end);
        int n =
            MultiByteToWideChar(CP_UTF8, 0, utf8_text.c_str(), -1, nullptr, 0);
        std::wstring wide(n - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8_text.c_str(), -1, wide.data(), n);
        SendMessageW(hwnd_, EM_REPLACESEL, TRUE, (LPARAM)wide.c_str());
        suppress_changed_ = false;
        if (on_changed_)
        {
            on_changed_(this->text());
        }
    }

    void set_on_popup_nav(std::function<bool(NavKey)> fn) override
    {
        popup_nav_ = std::move(fn);
    }

    void set_on_edit_last(std::function<bool()> fn) override
    {
        on_edit_last_ = std::move(fn);
    }

    int cursor_byte_pos() const override
    {
        if (!hwnd_)
        {
            return 0;
        }
        DWORD sel_start = 0, sel_end = 0;
        SendMessageW(hwnd_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start),
                     reinterpret_cast<LPARAM>(&sel_end));
        int len = GetWindowTextLengthW(hwnd_);
        if (len <= 0)
        {
            return 0;
        }
        std::wstring w(len, L'\0');
        GetWindowTextW(hwnd_, w.data(), len + 1);
        int caret = (int)std::min<DWORD>(sel_end, (DWORD)len);
        int bytes = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), caret, nullptr,
                                        0, nullptr, nullptr);
        return bytes;
    }

    // RichEdit supports per-run character formatting (EM_SETCHARFORMAT), but a
    // styled inline chip would require significant additional work.  For now a
    // mention is inserted as plain "@DisplayName" text and tracked in a
    // registry; mention_draft() reconstructs the outgoing segments by matching
    // these against the current text.
    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool is_room) override
    {
        std::string visual = is_room ? "@room" : ("@" + display_name);
        replace_range(start, end, visual + " ");
        mentions_.push_back({visual, user_id, display_name, is_room});
    }

    std::vector<tesseract::MentionSeg> mention_draft() const override
    {
        std::vector<tesseract::MentionSeg> segs;
        std::string t = text();
        std::size_t pos = 0;
        auto push_text = [&](const std::string& s)
        {
            if (!s.empty())
            {
                tesseract::MentionSeg seg;
                seg.kind = tesseract::MentionSeg::Kind::Text;
                seg.text = s;
                segs.push_back(std::move(seg));
            }
        };
        for (const auto& e : mentions_)
        {
            std::size_t at = t.find(e.visual, pos);
            if (at == std::string::npos)
            {
                continue; // mention text was edited/removed by the user
            }
            push_text(t.substr(pos, at - pos));
            tesseract::MentionSeg seg;
            seg.kind = tesseract::MentionSeg::Kind::Mention;
            seg.user_id = e.user_id;
            seg.display_name = e.display_name;
            seg.is_room = e.is_room;
            segs.push_back(std::move(seg));
            pos = at + e.visual.size();
        }
        push_text(t.substr(pos));
        return segs;
    }

    void set_mention_colors(Color, Color) override
    {
        // EDIT control has no per-run styling; no-op (functional mentions only).
    }

    void notify_changed()
    {
        if (suppress_changed_)
        {
            return;
        }
        std::string t = text();
        if (on_changed_)
        {
            on_changed_(t);
        }
        float h = natural_height();
        if (h != last_height_ && on_height_changed_)
        {
            last_height_ = h;
            on_height_changed_(h);
        }
    }
    int ctrl_id() const
    {
        return id_;
    }
    HWND hwnd() const
    {
        return hwnd_;
    }

private:
    static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR /*id*/,
                                          DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<Win32NativeTextArea*>(ref);
        if (msg == WM_NCPAINT)
        {
            return 0;
        }
        // Draw the placeholder text (cue banner) when the control is empty
        // and unfocused.  RichEdit does not support EM_SETCUEBANNER, so we
        // render it manually after the control's own WM_PAINT.
        if (msg == WM_PAINT)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            if (!self->placeholder_.empty() &&
                GetWindowTextLengthW(hwnd) == 0 &&
                GetFocus() != hwnd)
            {
                HDC hdc = GetDC(hwnd);
                HFONT font = reinterpret_cast<HFONT>(
                    SendMessageW(hwnd, WM_GETFONT, 0, 0));
                HGDIOBJ old_font = font ? SelectObject(hdc, font) : nullptr;
                RECT fr{};
                SendMessageW(hwnd, EM_GETRECT, 0,
                             reinterpret_cast<LPARAM>(&fr));
                SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, self->placeholder_.c_str(), -1, &fr,
                          DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
                if (old_font)
                {
                    SelectObject(hdc, old_font);
                }
                ReleaseDC(hwnd, hdc);
            }
            return r;
        }
        // Repaint when focus arrives/leaves so the placeholder appears and
        // disappears at the correct moment.
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
        {
            if (!self->placeholder_.empty())
            {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }
        if (msg == WM_KEYDOWN && self->popup_nav_)
        {
            NativeTextArea::NavKey nk{};
            bool is_nav = true;
            if (wParam == VK_UP)
            {
                nk = NativeTextArea::NavKey::Up;
            }
            else if (wParam == VK_DOWN)
            {
                nk = NativeTextArea::NavKey::Down;
            }
            else if (wParam == VK_ESCAPE)
            {
                nk = NativeTextArea::NavKey::Escape;
            }
            else if (wParam == VK_TAB)
            {
                nk = (GetKeyState(VK_SHIFT) & 0x8000)
                         ? NativeTextArea::NavKey::ShiftTab
                         : NativeTextArea::NavKey::Tab;
            }
            else
            {
                is_nav = false;
            }
            // Same re-entrancy hazard as Qt6: Tab calls replace_range()
            // → on_changed_() → set_on_popup_nav(nullptr), which zeros the
            // closure storage in-place.  Copy to keep it alive.
            auto nav = self->popup_nav_;
            if (is_nav && nav && nav(nk))
            {
                return 0;
            }
        }
        // Up in an empty composer (popup didn't consume it above) → edit
        // the last own message (Element/Slack convention).
        if (msg == WM_KEYDOWN && wParam == VK_UP && self->on_edit_last_ &&
            GetWindowTextLengthW(self->hwnd_) == 0)
        {
            if (self->on_edit_last_())
            {
                return 0;
            }
        }
        // TranslateMessage queues the WM_CHAR for VK_TAB *before* the
        // WM_KEYDOWN above is dispatched, so consuming the keydown alone
        // doesn't stop the multiline EDIT from inserting a literal tab —
        // which would mutate the compose text and dismiss the popup. While
        // the popup nav hook is live (popup open), swallow the tab WM_CHAR.
        if (msg == WM_CHAR && self->popup_nav_ && wParam == VK_TAB)
        {
            return 0;
        }
        if (msg == WM_KEYDOWN && wParam == VK_RETURN)
        {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!shift)
            {
                if (self->on_submit_)
                {
                    self->on_submit_();
                }
                // TranslateMessage already queued WM_CHAR('\r') — drain it now
                // so it doesn't insert a newline into the cleared field.
                MSG cr{};
                PeekMessage(&cr, self->hwnd_, WM_CHAR, WM_CHAR, PM_REMOVE);
                return 0;
            }
        }
        // Intercept Ctrl+V / Shift+Insert / right-click "Paste" before
        // EDIT inserts text. If clipboard holds a DIB and we have an
        // image-paste handler, route to it and skip the default.
        if (msg == WM_PASTE && self->on_image_paste_ && self->wic_)
        {
            if (IsClipboardFormatAvailable(CF_DIBV5) ||
                IsClipboardFormatAvailable(CF_DIB))
            {
                std::vector<std::uint8_t> bytes;
                if (clipboard_image_to_png(self->wic_, hwnd, bytes))
                {
                    self->on_image_paste_(std::move(bytes), "image/png");
                    return 0;
                }
            }
        }
        if (msg == WM_GETDLGCODE)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            return r | DLGC_WANTALLKEYS;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    static int utf8_byte_to_utf16_len(const std::string& utf8, int byte_offset)
    {
        byte_offset = std::clamp(byte_offset, 0, (int)utf8.size());
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), byte_offset,
                                       nullptr, 0);
        return wlen < 0 ? 0 : wlen;
    }

    float dip_scale() const
    {
        const float dpi = static_cast<float>(GetDpiForWindow(parent_));
        return dpi > 0.f ? dpi / 96.f : 1.f;
    }

    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    int id_ = 0;
    bool suppress_changed_ = false;
    // Tracks the last value passed to set_visible(). Matches the WS_VISIBLE
    // style on the CreateWindowExW call above.
    bool visible_ = true;
    float last_height_ = 0.f;
    Rect last_rect_ = {-1.f, -1.f, -1.f, -1.f};
    IWICImagingFactory* wic_ = nullptr;
    // Placeholder (cue-banner) text stored as UTF-16 for DrawTextW.  Rendered
    // manually in WM_PAINT because RichEdit does not support EM_SETCUEBANNER.
    std::wstring placeholder_;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<void(float)> on_height_changed_;
    ImagePasteHandler on_image_paste_;
    std::function<bool(NativeTextArea::NavKey)> popup_nav_;
    std::function<bool()> on_edit_last_;

    // Inserted mentions, in document order. `visual` is the plain text shown in
    // the EDIT control (e.g. "@Alice"); mention_draft() reconstructs the
    // outgoing segments by matching these against the current text.
    struct MentionEntry
    {
        std::string visual;
        std::string user_id;
        std::string display_name;
        bool is_room;
    };
    std::vector<MentionEntry> mentions_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Win32RichEditArea — windowless RichEdit via ITextServices2 + TxDrawD2D
// ─────────────────────────────────────────────────────────────────────────
//
// Architecture:
//   - A plain WS_CHILD HWND (class "tk_re_host") is the visible host window.
//   - This object implements ITextHost2, which the windowless RichEdit calls
//     for all host services (DC, caret, invalidation, notifications, …).
//   - CreateTextServices() (GetProcAddress from Msftedit.dll) creates the
//     ITextServices2 object; we forward WM_* through TxSendMessage.
//   - WM_PAINT calls ITextServices2::TxDrawD2D() on an ID2D1HwndRenderTarget.
//   - TXTBIT_D2DDWRITE (returned by TxGetPropertyBits) routes the control's
//     internal rendering through D2D/DirectWrite, enabling colour emoji.

class Win32RichEditArea : public NativeTextArea,
                          public Win32TextAreaBase,
                          public ITextHost2,
                          public IProvideFontInfo
{
public:
    Win32RichEditArea(HWND parent, int ctrl_id, IWICImagingFactory* wic,
                      ID2D1Device* d2d_device, ID3D11Device* d3d_device,
                      IDWriteFactory2* dwrite,
                      const Theme* theme, IDWriteFontFace* noto_face)
        : parent_(parent), id_(ctrl_id), wic_(wic),
          d2d_device_(d2d_device), d3d_device_(d3d_device),
          dwrite_(dwrite),
          theme_(theme), noto_face_(noto_face)
    {
        // Register the host window class once per process.
        static bool s_cls = []() -> bool
        {
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.style         = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc   = &Win32RichEditArea::wnd_proc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.hCursor       = LoadCursorW(nullptr, IDC_IBEAM);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = L"tk_re_host";
            return RegisterClassExW(&wc) != 0 ||
                   GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }();
        (void)s_cls;

        // Create the host HWND; pass `this` as lpCreateParams so WM_NCCREATE
        // can stash it in GWLP_USERDATA before any other message arrives.
        hwnd_ = CreateWindowExW(0, L"tk_re_host", L"",
                                 WS_CHILD | WS_VISIBLE, 0, 0, 200, 40,
                                 parent_,
                                 reinterpret_cast<HMENU>(
                                     static_cast<INT_PTR>(id_)),
                                 GetModuleHandleW(nullptr), this);
        if (!hwnd_)
            return;

        // Load Msftedit.dll and locate CreateTextServices (once per process).
        typedef HRESULT(WINAPI* PCreateTS)(IUnknown*, ITextHost*, IUnknown**);
        static HMODULE s_msftedit = LoadLibraryW(L"MSFTEDIT.DLL");
        static PCreateTS s_create =
            s_msftedit
                ? reinterpret_cast<PCreateTS>(
                      GetProcAddress(s_msftedit, "CreateTextServices"))
                : nullptr;
        if (!s_create)
            return;

        // Default character format: Segoe UI Variable Text, font_body pt.
        char_fmt_.cbSize    = sizeof(char_fmt_);
        char_fmt_.dwMask    = CFM_FACE | CFM_SIZE | CFM_CHARSET | CFM_COLOR;
        char_fmt_.dwEffects = CFE_AUTOCOLOR;
        // yHeight is in twips (1 pt = 20 twips).
        char_fmt_.yHeight  = tesseract::Settings::instance().font_body * 20;
        char_fmt_.bCharSet = DEFAULT_CHARSET;
        wcscpy_s(char_fmt_.szFaceName, L"Segoe UI Variable Text");

        // Default paragraph format: left-aligned.
        para_fmt_.cbSize     = sizeof(para_fmt_);
        para_fmt_.dwMask     = PFM_ALIGNMENT;
        para_fmt_.wAlignment = PFA_LEFT;

        // Create the windowless text services object, then QI ITextServices2.
        using Microsoft::WRL::ComPtr;
        ComPtr<IUnknown> unk;
        if (FAILED(s_create(nullptr, static_cast<ITextHost*>(this),
                            unk.GetAddressOf())))
            return;
        if (FAILED(unk->QueryInterface(kIID_ITextServices2,
                reinterpret_cast<void**>(text_svc_.GetAddressOf()))))
            return;

        // Enable colour emoji + colour font display through DirectWrite.
        LRESULT lr = 0;
        text_svc_->TxSendMessage(EM_SETTYPOGRAPHYOPTIONS,
            TO_DEFAULTCOLOREMOJI | TO_DISPLAYFONTCOLOR,
            TO_DEFAULTCOLOREMOJI | TO_DISPLAYFONTCOLOR, &lr);

        // Notify text services of the initial client rect.
        text_svc_->OnTxPropertyBitsChange(TXTBIT_CLIENTRECTCHANGE,
                                           TXTBIT_CLIENTRECTCHANGE);

        // Pre-build an IDWriteTextFormat for the placeholder cue string.
        if (dwrite)
        {
            const float dip_size =
                tesseract::Settings::instance().font_body * (96.f / 72.f);
            dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, dip_size, L"",
                placeholder_fmt_.GetAddressOf());
        }
    }

    ~Win32RichEditArea() override
    {
        // Release in strict order:
        // 1. Text services holds a raw back-reference to us (ITextHost).
        //    Destroying it first prevents further TxNotify/TxGetDC calls.
        text_svc_.Reset();
        // 2. D2D device context must be detached from its target before
        //    the swap chain can be destroyed.
        if (dc_)
            dc_->SetTarget(nullptr);
        dc_bitmap_.Reset();
        dc_.Reset();
        swap_chain_.Reset();
        // 3. Now it is safe to destroy the host window.
        if (hwnd_)
        {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    // ── IUnknown ──────────────────────────────────────────────────────────
    //
    // Win32RichEditArea is owned by the caller (unique_ptr); ref counting is
    // a no-op.  We respond to QI for ITextHost/ITextHost2 so the text
    // services object can locate its host interface.

    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || IsEqualGUID(riid, kIID_ITextHost) ||
            IsEqualGUID(riid, kIID_ITextHost2))
        {
            *ppv = static_cast<ITextHost2*>(this);
            return S_OK;
        }
        // IProvideFontInfo intentionally NOT exposed: when registered, MSFTEDIT
        // replaces its DWrite font fallback with our callbacks.  Returning
        // fontFaceIdCurrent / nullptr from the callbacks suppresses fallback
        // entirely, producing .notdef boxes.  Without the interface, MSFTEDIT
        // runs its own DWrite fallback and selects Segoe UI Emoji normally.
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // ── ITextHost ─────────────────────────────────────────────────────────

    HDC TxGetDC() override { return GetDC(hwnd_); }
    INT TxReleaseDC(HDC hdc) override { return ReleaseDC(hwnd_, hdc); }

    BOOL TxShowScrollBar(INT fnBar, BOOL fShow) override
    {
        (void)fnBar;
        (void)fShow;
        return FALSE;
    }
    BOOL TxEnableScrollBar(INT fuSBFlags, INT fuArrowflags) override
    {
        (void)fuSBFlags;
        (void)fuArrowflags;
        return FALSE;
    }
    BOOL TxSetScrollRange(INT fnBar, LONG nMinPos, INT nMaxPos,
                          BOOL fRedraw) override
    {
        (void)fnBar;
        (void)nMinPos;
        (void)nMaxPos;
        (void)fRedraw;
        return FALSE;
    }
    BOOL TxSetScrollPos(INT fnBar, INT nPos, BOOL fRedraw) override
    {
        (void)fnBar;
        (void)nPos;
        (void)fRedraw;
        return FALSE;
    }

    void TxInvalidateRect(LPCRECT prc, BOOL fMode) override
    {
        // Suppress re-invalidation during TxDrawD2D: MSFTEDIT calls this on
        // every render in D2D mode (smooth caret/cursor updates). Forwarding
        // it as another InvalidateRect would queue a new WM_PAINT before the
        // current one completes, creating a continuous WM_PAINT loop that
        // starves WM_TIMER (WM_PAINT has higher priority in GetMessage).
        // MSFTEDIT's own TxSetTimer fires TxViewChange outside of paint and
        // correctly drives the repaint cadence from there.
        if (!in_paint_)
            InvalidateRect(hwnd_, prc, fMode);
        // else: suppressed; MSFTEDIT's external timer will trigger the next repaint.
    }
    void TxViewChange(BOOL fUpdate) override
    {
        // Use async InvalidateRect instead of synchronous UpdateWindow.
        // UpdateWindow sends WM_PAINT synchronously and causes reentrancy when
        // called during TxDrawD2D (MSFTEDIT calls TxViewChange mid-draw).
        // Suppress when in_paint_ for the same reason as TxInvalidateRect above.
        if (fUpdate && !in_paint_)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // MSFTEDIT in D2D mode may call these when the cursor position changes
    // (e.g. after a click or arrow key).  TxSetCaretPos triggers a repaint so
    // our self-drawn caret follows; the others are no-ops.
    BOOL TxCreateCaret(HBITMAP, INT, INT) override { return TRUE; }
    BOOL TxShowCaret(BOOL) override { return TRUE; }
    BOOL TxSetCaretPos(INT /*x*/, INT /*y*/) override
    {
        // Force a caret repaint whenever MSFTEDIT tells us the cursor moved.
        if (!in_paint_)
            InvalidateRect(hwnd_, nullptr, FALSE);
        return TRUE;
    }
    BOOL TxSetTimer(UINT idTimer, UINT uTimeout) override
    {
        return SetTimer(hwnd_, idTimer, uTimeout, nullptr) != 0;
    }
    void TxKillTimer(UINT idTimer) override { KillTimer(hwnd_, idTimer); }

    void TxScrollWindowEx(INT dx, INT dy, LPCRECT lprcScroll,
                          LPCRECT lprcClip, HRGN hrgnUpdate,
                          LPRECT lprcUpdate, UINT fuScroll) override
    {
        ScrollWindowEx(hwnd_, dx, dy, lprcScroll, lprcClip, hrgnUpdate,
                       lprcUpdate, fuScroll);
    }

    void TxSetCapture(BOOL fCapture) override
    {
        if (fCapture)
            SetCapture(hwnd_);
        else
            ReleaseCapture();
    }
    void TxSetFocus() override
    {
        // Intentional no-op.
        //
        // hwnd_ acquires focus through normal OS mechanics: the user clicks
        // it, Win32 delivers WM_SETFOCUS, and our wnd_proc forwards that to
        // TxSendMessage(WM_SETFOCUS) — MSFTEDIT is fully notified.
        //
        // MSFTEDIT also calls TxSetFocus() from its own internal timer
        // callbacks (set via TxSetTimer / TxKillTimer).  Calling
        // SetFocus(hwnd_) here would steal focus from any other control that
        // the user legitimately focused (e.g. the room-search text field),
        // breaking keyboard input there.  With a real host HWND there is no
        // need to programmatically re-focus: if the user wants the compose
        // bar they will click it.
    }
    void TxSetCursor(HCURSOR hcur, BOOL /*fText*/) override
    {
        SetCursor(hcur);
    }
    BOOL TxScreenToClient(LPPOINT lppt) override
    {
        return ScreenToClient(hwnd_, lppt);
    }
    BOOL TxClientToScreen(LPPOINT lppt) override
    {
        return ClientToScreen(hwnd_, lppt);
    }

    HRESULT TxActivate(LONG* plOldState) override
    {
        (void)plOldState;
        return S_OK;
    }
    HRESULT TxDeactivate(LONG lNewState) override
    {
        (void)lNewState;
        return S_OK;
    }

    HRESULT TxGetClientRect(LPRECT prc) override
    {
        GetClientRect(hwnd_, prc);
        return S_OK;
    }
    HRESULT TxGetViewInset(LPRECT prc) override
    {
        // No internal padding; natural_height() adds 8 px externally.
        SetRectEmpty(prc);
        return S_OK;
    }

    HRESULT TxGetCharFormat(const CHARFORMATW** ppCF) override
    {
        *ppCF = reinterpret_cast<const CHARFORMATW*>(&char_fmt_);
        return S_OK;
    }
    HRESULT TxGetParaFormat(const PARAFORMAT** ppPF) override
    {
        *ppPF = reinterpret_cast<const PARAFORMAT*>(&para_fmt_);
        return S_OK;
    }

    COLORREF TxGetSysColor(int nIndex) override
    {
        // Map text foreground to the theme's text_primary colour so it tracks
        // dark/light mode; fall back to system colours for everything else
        // (selection highlight, etc.).
        if (nIndex == COLOR_WINDOWTEXT && theme_)
        {
            const auto& c = theme_->palette.text_primary;
            return RGB(c.r, c.g, c.b);
        }
        return GetSysColor(nIndex);
    }

    HRESULT TxGetBackStyle(TXTBACKSTYLE* pstyle) override
    {
        // We clear to compose_card_bg in on_paint() before TxDrawD2D, so
        // the text services object renders transparently over our fill.
        *pstyle = TXTBACK_TRANSPARENT;
        return S_OK;
    }

    void on_theme_changed(const Theme& t) override
    {
        theme_ = &t;
        if (text_svc_)
        {
            // Force ITextServices2 to discard cached TxGetSysColor values
            // (text colour, selection highlight, etc.) and re-read them.
            LRESULT lr = 0;
            text_svc_->TxSendMessage(WM_SYSCOLORCHANGE, 0, 0, &lr);
        }
        if (hwnd_)
            InvalidateRect(hwnd_, nullptr, TRUE);
    }

    HRESULT TxGetMaxLength(DWORD* plength) override
    {
        *plength = INFINITE;
        return S_OK;
    }
    HRESULT TxGetScrollBars(DWORD* pdwScrollBar) override
    {
        *pdwScrollBar = 0; // compose bar has no scrollbars; it auto-grows
        return S_OK;
    }
    HRESULT TxGetPasswordChar(_Out_ TCHAR* pch) override
    {
        *pch = 0;
        return S_FALSE;
    }
    HRESULT TxGetAcceleratorPos(LONG* pcp) override
    {
        *pcp = -1;
        return S_FALSE;
    }
    HRESULT TxGetExtent(LPSIZEL /*lpExtent*/) override { return E_NOTIMPL; }

    HRESULT OnTxCharFormatChange(const CHARFORMATW* /*pcf*/) override
    {
        return S_OK;
    }
    HRESULT OnTxParaFormatChange(const PARAFORMAT* /*ppf*/) override
    {
        return S_OK;
    }

    // CRITICAL: TXTBIT_D2DDWRITE switches the control's internal rendering
    // from GDI/Uniscribe to D2D/DirectWrite.  Without this bit, colour emoji
    // render as monochrome outlines.
    HRESULT TxGetPropertyBits(DWORD dwMask, DWORD* pdwBits) override
    {
        constexpr DWORD kBits = TXTBIT_RICHTEXT | TXTBIT_MULTILINE |
                                TXTBIT_WORDWRAP | TXTBIT_D2DDWRITE;
        *pdwBits = kBits & dwMask;
        return S_OK;
    }

    // EN_CHANGE fires synchronously inside TxSendMessage.  The
    // suppress_changed_ guard is already set whenever we call EM_REPLACESEL
    // from set_text / replace_range, so spurious on_changed_ calls are
    // filtered inside notify_changed().
    HRESULT TxNotify(DWORD iNotify, void* pv) override
    {
        (void)pv;
        if (iNotify == EN_CHANGE)
            notify_changed();
        return S_OK;
    }

    HIMC TxImmGetContext() override { return ImmGetContext(hwnd_); }
    void TxImmReleaseContext(HIMC himc) override
    {
        ImmReleaseContext(hwnd_, himc);
    }
    HRESULT TxGetSelectionBarWidth(LONG* pl) override
    {
        *pl = 0;
        return S_OK;
    }

    // ── ITextHost2 ────────────────────────────────────────────────────────

    BOOL TxIsDoubleClickPending() override { return FALSE; }

    HRESULT TxGetWindow(HWND* phwnd) override
    {
        *phwnd = hwnd_;
        return S_OK;
    }
    HRESULT TxSetForegroundWindow() override
    {
        SetForegroundWindow(hwnd_);
        return S_OK;
    }
    HPALETTE TxGetPalette() override { return nullptr; }
    HRESULT  TxGetEastAsianFlags(LONG* pFlags) override
    {
        (void)pFlags;
        return E_NOTIMPL;
    }
    HCURSOR TxSetCursor2(HCURSOR hcur, BOOL /*bText*/) override
    {
        HCURSOR prev = GetCursor();
        if (hcur)
            SetCursor(hcur);
        return prev;
    }
    // Called when text services is being destroyed.  Our destructor resets
    // text_svc_ first, so by the time this fires we are already done with it.
    void TxFreeTextServicesNotification() override {}

    HRESULT TxGetEditStyle(DWORD /*dwItem*/, DWORD* pdwData) override
    {
        *pdwData = 0;
        return S_OK;
    }
    HRESULT TxGetWindowStyles(DWORD* pdwStyle, DWORD* pdwExStyle) override
    {
        *pdwStyle   = hwnd_ ? static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE))   : 0;
        *pdwExStyle = hwnd_ ? static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)) : 0;
        return S_OK;
    }
    // TxShowDropCaret — D2D-mode caret hook.
    // MSFTEDIT does NOT draw the caret inside TxDrawD2D; instead it calls us
    // on each blink-timer tick with the current show/hide state + caret rect.
    // We store the state and let on_paint() paint the 1px vertical bar after
    // TxDrawD2D returns.
    HRESULT TxShowDropCaret(BOOL /*fShow*/, HDC /*hdc*/, LPCRECT prc) override
    {
        // Not used in D2D mode (MSFTEDIT uses TxShowCaret/TxSetCaretPos).
        InvalidateRect(hwnd_, prc, FALSE);
        return S_OK;
    }
    HRESULT TxDestroyCaret() override { return S_OK; }
    HRESULT TxGetHorzExtent(LONG* plHorzExtent) override
    {
        RECT r{};
        GetClientRect(hwnd_, &r);
        *plHorzExtent = r.right - r.left;
        return S_OK;
    }

    // ── IProvideFontInfo ──────────────────────────────────────────────────
    //
    // MSFTEDIT QIs our ITextHost2 for IProvideFontInfo to let the host inject
    // custom IDWriteFontFace pointers per text run.  We use this to route emoji
    // runs to our embedded Noto Color Emoji face so the compose bar matches the
    // rest of the application (canvas_d2d uses the same face via its fallback).

    BSTR STDMETHODCALLTYPE GetDefaultFont() override
    {
        return SysAllocString(L"Segoe UI Variable Text");
    }

    DWORD STDMETHODCALLTYPE GetRunFontFaceId(
        const wchar_t* /*pCurrentFontFamilyName*/,
        DWRITE_FONT_WEIGHT  /*weight*/,
        DWRITE_FONT_STRETCH /*stretch*/,
        DWRITE_FONT_STYLE   /*style*/,
        LCID                /*lcid*/,
        const wchar_t*      pText,
        UINT                cch,
        DWORD               fontFaceIdCurrent,
        UINT&               runCount) override
    {
        if (!pText || cch == 0)
        {
            runCount = cch;
            return fontFaceIdCurrent;
        }

        // Decode the first Unicode codepoint (handle UTF-16 surrogate pairs).
        char32_t first_cp;
        UINT first_size;
        if (cch >= 2 &&
            pText[0] >= 0xD800 && pText[0] <= 0xDBFF &&
            pText[1] >= 0xDC00 && pText[1] <= 0xDFFF)
        {
            first_cp = 0x10000u +
                       (static_cast<char32_t>(pText[0] - 0xD800u) << 10) +
                       (pText[1] - 0xDC00u);
            first_size = 2;
        }
        else
        {
            first_cp   = pText[0];
            first_size = 1;
        }

        const bool first_emoji = is_emoji_codepoint(first_cp);

        // Extend the run while characters share the same emoji/non-emoji class.
        UINT pos = first_size;
        while (pos < cch)
        {
            char32_t cp;
            UINT     sz;
            if (pos + 1 < cch &&
                pText[pos] >= 0xD800 && pText[pos] <= 0xDBFF &&
                pText[pos + 1] >= 0xDC00 && pText[pos + 1] <= 0xDFFF)
            {
                cp = 0x10000u +
                     (static_cast<char32_t>(pText[pos] - 0xD800u) << 10) +
                     (pText[pos + 1] - 0xDC00u);
                sz = 2;
            }
            else
            {
                cp = pText[pos];
                sz = 1;
            }
            if (is_emoji_codepoint(cp) != first_emoji) break;
            pos += sz;
        }
        runCount = pos;

        // Don't intercept — let MSFTEDIT's own DWrite font fallback select
        // Segoe UI Emoji, which it colour-renders via TO_DEFAULTCOLOREMOJI +
        // ID2D1DeviceContext.  IProvideFontInfo with an externally-injected
        // IDWriteFontFace* only works for fonts in MSFTEDIT's internal
        // collection; Noto (loaded in-memory) renders blank through this path.
        (void)first_emoji;
        return fontFaceIdCurrent;
    }

    IDWriteFontFace* STDMETHODCALLTYPE GetFontFace(DWORD fontFaceId) override
    {
        (void)fontFaceId;
        return nullptr; // pass-through — MSFTEDIT uses its own face
    }

    BSTR STDMETHODCALLTYPE GetSerializableFontName(DWORD fontFaceId) override
    {
        if (fontFaceId == kNotoFaceId)
            return SysAllocString(L"Noto Color Emoji");
        return SysAllocString(L"Segoe UI Variable Text");
    }

    // ── Win32TextAreaBase ─────────────────────────────────────────────────

    void notify_changed() override
    {
        if (suppress_changed_)
            return;
        std::string t = text();
        if (on_changed_)
            on_changed_(t);
        float h = natural_height();
        if (h != last_height_ && on_height_changed_)
        {
            last_height_ = h;
            on_height_changed_(h);
        }
    }
    int  ctrl_id() const override { return id_; }
    HWND hwnd()    const override { return hwnd_; }

    // ── NativeTextArea ────────────────────────────────────────────────────

    void set_rect(Rect r) override
    {
        if (!hwnd_)
            return;
        if (r.x == last_rect_.x && r.y == last_rect_.y &&
            r.w == last_rect_.w && r.h == last_rect_.h)
            return;
        last_rect_ = r;
        // Mirror Win32NativeTextArea::set_rect: size the HWND to the natural
        // text height and centre it vertically inside `r` so the parent
        // D2D surface can draw the card border in the space above and below.
        // If content overflows the envelope, fill the full height instead.
        // r is in logical pixels; SetWindowPos needs physical pixels.
        const float s  = dip_scale();
        const int rh   = static_cast<int>(std::round(r.h * s));
        const int nh   = static_cast<int>(std::round(natural_height() * s));
        // Keep one physical pixel of clearance at the top and bottom so
        // the parent surface's card border stroke is never covered.
        const int border_px = std::max(2, static_cast<int>(std::ceil(s)));
        const int max_h = std::max(1, rh - 2 * border_px);
        const int h    = (nh > 0 && nh <= max_h) ? nh : max_h;
        const int y    = static_cast<int>(std::floor(r.y * s)) + (rh - h) / 2;
        SetWindowPos(hwnd_, nullptr,
                     static_cast<int>(std::floor(r.x * s)), y,
                     static_cast<int>(std::round(r.w * s)), h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void set_text(std::string text) override
    {
        if (!text_svc_)
            return;
        suppress_changed_ = true;
        std::wstring w = utf8_to_wide(text);
        LRESULT lr = 0;
        text_svc_->TxSendMessage(WM_SETTEXT, 0,
                                  reinterpret_cast<LPARAM>(w.c_str()), &lr);
        suppress_changed_ = false;
        mentions_.clear();
        float h = natural_height();
        if (h != last_height_ && on_height_changed_)
        {
            last_height_ = h;
            on_height_changed_(h);
        }
    }

    std::string text() const override
    {
        if (!text_svc_)
            return {};
        LRESULT len = 0;
        const_cast<ITextServices2*>(text_svc_.Get())
            ->TxSendMessage(WM_GETTEXTLENGTH, 0, 0, &len);
        if (len <= 0)
            return {};
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        const_cast<ITextServices2*>(text_svc_.Get())
            ->TxSendMessage(WM_GETTEXT, static_cast<WPARAM>(len + 1),
                            reinterpret_cast<LPARAM>(w.data()), &len);
        std::string s = wide_to_utf8(w);
        // RichEdit uses \r as paragraph separator; normalise to \n.
        for (auto& c : s)
            if (c == '\r')
                c = '\n';
        return s;
    }

    void set_placeholder(std::string text) override
    {
        placeholder_ = utf8_to_wide(text);
        if (hwnd_)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void set_focused(bool focused) override
    {
        if (hwnd_ && focused)
            SetFocus(hwnd_);
    }

    void set_visible(bool visible) override
    {
        visible_ = visible;
        if (hwnd_)
            ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
    bool visible() const override { return visible_; }

    void set_enabled(bool enabled) override
    {
        if (hwnd_)
            EnableWindow(hwnd_, enabled ? TRUE : FALSE);
    }

    float natural_height() const override
    {
        if (!text_svc_ || !hwnd_)
            return 0.f;

        const float s = dip_scale();
        // Floor: one line of body text expressed in DIPs.
        const float min_dip =
            static_cast<float>(tesseract::Settings::instance().font_body) *
            (96.f / 72.f);

        // TxGetNaturalSize(TXTNS_FITTOCONTENT) returns stale or zero values in
        // TXTBIT_D2DDWRITE mode and cannot be used to drive compose-bar growth.
        // build_text_layout() constructs an IDWriteTextLayout at the current
        // wrap width; GetMetrics().height is the exact content height and is
        // reliable in D2D mode — the same path used by scroll-to-caret and
        // hit-testing.
        float scale = 1.f;
        auto layout = build_text_layout(&scale, nullptr, nullptr);
        float content_dip = min_dip;
        if (layout)
        {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(layout->GetMetrics(&m)) && m.height > content_dip)
                content_dip = m.height;
        }
        // Add 4 px top + 4 px bottom padding, returned as DIPs.
        return content_dip + 8.f / s;
    }

    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }
    void set_on_height_changed(std::function<void(float)> cb) override
    {
        on_height_changed_ = std::move(cb);
    }
    void set_on_image_paste(ImagePasteHandler cb) override
    {
        on_image_paste_ = std::move(cb);
    }

    void insert_at_cursor(std::string text) override
    {
        if (!text_svc_)
            return;
        std::wstring w = utf8_to_wide(text);
        LRESULT lr = 0;
        text_svc_->TxSendMessage(EM_REPLACESEL, TRUE,
                                  reinterpret_cast<LPARAM>(w.c_str()), &lr);
    }

    tk::Rect cursor_rect() const override
    {
        if (!text_svc_ || !hwnd_)
            return {};
        LRESULT sel_start = 0;
        const_cast<ITextServices2*>(text_svc_.Get())
            ->TxSendMessage(EM_GETSEL,
                            reinterpret_cast<WPARAM>(&sel_start), 0, nullptr);

        // RichEdit EM_POSFROMCHAR: wParam = POINTL*, lParam = char index.
        POINTL pt{};
        LRESULT r = 0;
        const_cast<ITextServices2*>(text_svc_.Get())
            ->TxSendMessage(EM_POSFROMCHAR,
                            reinterpret_cast<WPARAM>(&pt),
                            static_cast<LPARAM>(sel_start), &r);

        POINT win_pt{static_cast<LONG>(pt.x), static_cast<LONG>(pt.y)};
        MapWindowPoints(hwnd_, GetParent(hwnd_), &win_pt, 1);

        // Estimate line height from the char format (twips → pixels).
        HDC hdc  = GetDC(hwnd_);
        int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(hwnd_, hdc);
        float line_h =
            static_cast<float>(char_fmt_.yHeight) / 20.f * (dpiY / 72.f);

        return {static_cast<float>(win_pt.x), static_cast<float>(win_pt.y),
                1.f, line_h};
    }

    void replace_range(int start, int end, std::string utf8_text) override
    {
        if (!text_svc_)
            return;
        std::string cur = text();
        int ws = utf8_byte_to_utf16_len(cur, start);
        int we = utf8_byte_to_utf16_len(cur, end);
        suppress_changed_ = true;
        LRESULT lr = 0;
        text_svc_->TxSendMessage(EM_SETSEL, static_cast<WPARAM>(ws),
                                  static_cast<LPARAM>(we), &lr);
        std::wstring wide = utf8_to_wide(utf8_text);
        text_svc_->TxSendMessage(EM_REPLACESEL, TRUE,
                                  reinterpret_cast<LPARAM>(wide.c_str()), &lr);
        suppress_changed_ = false;
        if (on_changed_)
            on_changed_(text());
    }

    void set_on_popup_nav(std::function<bool(NavKey)> fn) override
    {
        popup_nav_ = std::move(fn);
    }
    void set_on_edit_last(std::function<bool()> fn) override
    {
        on_edit_last_ = std::move(fn);
    }

    int cursor_byte_pos() const override
    {
        if (!text_svc_)
            return 0;
        LRESULT sel_end = 0;
        const_cast<ITextServices2*>(text_svc_.Get())
            ->TxSendMessage(EM_GETSEL, 0,
                            reinterpret_cast<LPARAM>(&sel_end), nullptr);
        std::string t = text();
        std::wstring w = utf8_to_wide(t);
        int caret = static_cast<int>(
            std::min(static_cast<LRESULT>(w.size()), sel_end));
        return WideCharToMultiByte(CP_UTF8, 0, w.c_str(), caret,
                                   nullptr, 0, nullptr, nullptr);
    }

    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool is_room) override
    {
        std::string visual = is_room ? "@room" : ("@" + display_name);
        replace_range(start, end, visual + " ");
        mentions_.push_back({visual, user_id, display_name, is_room});
    }

    std::vector<tesseract::MentionSeg> mention_draft() const override
    {
        std::vector<tesseract::MentionSeg> segs;
        std::string t = text();
        std::size_t pos = 0;
        auto push_text = [&](const std::string& s)
        {
            if (!s.empty())
            {
                tesseract::MentionSeg seg;
                seg.kind = tesseract::MentionSeg::Kind::Text;
                seg.text = s;
                segs.push_back(std::move(seg));
            }
        };
        for (const auto& e : mentions_)
        {
            std::size_t at = t.find(e.visual, pos);
            if (at == std::string::npos)
                continue;
            push_text(t.substr(pos, at - pos));
            tesseract::MentionSeg seg;
            seg.kind         = tesseract::MentionSeg::Kind::Mention;
            seg.user_id      = e.user_id;
            seg.display_name = e.display_name;
            seg.is_room      = e.is_room;
            segs.push_back(std::move(seg));
            pos = at + e.visual.size();
        }
        push_text(t.substr(pos));
        return segs;
    }

    void set_mention_colors(Color, Color) override {}

private:
    // ── D2D render target helpers ─────────────────────────────────────────

    bool is_text_empty() const
    {
        if (!text_svc_)
            return true;
        LRESULT lr = 0;
        const_cast<ITextServices2*>(text_svc_.Get())
            ->TxSendMessage(WM_GETTEXTLENGTH, 0, 0, &lr);
        return lr == 0;
    }

    // Attach the swap chain's back buffer as the device context render target.
    void attach_swap_chain_bitmap()
    {
        using Microsoft::WRL::ComPtr;
        if (!swap_chain_ || !dc_) return;
        ComPtr<IDXGISurface> surface;
        if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()))))
            return;
        // Read the HWND DPI so D2D coordinates match the window's pixel density.
        float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        if (dpi == 0.f) dpi = 96.f;
        // Alpha mode MUST match the swap chain (IGNORE = opaque, no per-pixel alpha).
        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            dpi, dpi);
        ComPtr<ID2D1Bitmap1> bmp;
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bmpProps,
                                                     bmp.GetAddressOf())))
            return;
        dc_bitmap_ = bmp;
        dc_->SetTarget(bmp.Get());
        dc_->SetDpi(dpi, dpi);
    }

    // Ensure a DXGI flip-model swap chain + ID2D1DeviceContext exists for the
    // host HWND.  Replaces the old ID2D1HwndRenderTarget so that TxDrawD2D
    // receives a real ID2D1DeviceContext, which MSFTEDIT QIs for internally to
    // enable its full colour-glyph pipeline (COLR v0/v1, PNG bitmap emoji).
    void ensure_render_target(int w, int h)
    {
        using Microsoft::WRL::ComPtr;
        if (!d2d_device_ || !d3d_device_ || !hwnd_ || w <= 0 || h <= 0)
            return;

        if (swap_chain_ && dc_)
        {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            swap_chain_->GetDesc1(&desc);
            if (static_cast<int>(desc.Width)  == w &&
                static_cast<int>(desc.Height) == h)
                return; // already the right size

            // Resize: detach bitmap, resize buffers, re-attach.
            dc_->SetTarget(nullptr);
            dc_bitmap_.Reset();
            HRESULT hr = swap_chain_->ResizeBuffers(
                0, static_cast<UINT>(w), static_cast<UINT>(h),
                DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr))
            {
                dc_.Reset();
                swap_chain_.Reset();
            }
            else
            {
                attach_swap_chain_bitmap();
            }
            return;
        }

        // First-time creation: get the DXGI factory from the D3D device.
        ComPtr<IDXGIDevice> dxgi_dev;
        if (FAILED(d3d_device_->QueryInterface(IID_PPV_ARGS(dxgi_dev.GetAddressOf()))))
            return;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgi_dev->GetAdapter(adapter.GetAddressOf())))
            return;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
            return;

        // Use the blit-model swap chain (DISCARD, BufferCount=1).
        // FLIP_DISCARD is not reliably supported on child HWNDs on all Win10
        // configurations and silently fails, leaving nothing on screen.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width       = static_cast<UINT>(w);
        desc.Height      = static_cast<UINT>(h);
        desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
        desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> sc;
        {
            HRESULT hr = factory->CreateSwapChainForHwnd(
                d3d_device_, hwnd_, &desc, nullptr, nullptr,
                sc.GetAddressOf());
            if (FAILED(hr))
            {
                wchar_t buf[128];
                swprintf_s(buf, L"[tk_re] CreateSwapChainForHwnd failed hr=0x%08X\n",
                           static_cast<unsigned>(hr));
                OutputDebugStringW(buf);
                return;
            }
        }

        ComPtr<ID2D1DeviceContext> dc;
        {
            HRESULT hr = d2d_device_->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.GetAddressOf());
            if (FAILED(hr))
            {
                wchar_t buf[128];
                swprintf_s(buf, L"[tk_re] CreateDeviceContext failed hr=0x%08X\n",
                           static_cast<unsigned>(hr));
                OutputDebugStringW(buf);
                return;
            }
        }

        swap_chain_ = sc;
        dc_         = dc;
        attach_swap_chain_bitmap();
    }

    void on_paint()
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        int w = client.right - client.left;
        int h = client.bottom - client.top;
        ensure_render_target(w, h);
        if (!dc_ || !swap_chain_ || !text_svc_)
            return;

        const tk::Color bg = theme_ ? theme_->palette.compose_card_bg
                                     : tk::Color{255, 255, 255, 255};

        dc_->BeginDraw();
        dc_->Clear(D2D1::ColorF(bg.r / 255.f, bg.g / 255.f, bg.b / 255.f));

        // Compute caret metrics first (always, when the control has text) so
        // scroll-to-caret can run before TxDrawD2D paints the scrolled view.
        // MSFTEDIT's EM_POSFROMCHAR returns (0,0) in windowless D2D mode, so
        // we route through IDWriteTextLayout::HitTestTextPosition instead.
        DWORD sel_start = 0, sel_end = 0;
        float caret_x_dip = 0.f, caret_y_dip = 0.f, caret_h_dip = 0.f;
        bool  have_caret = false;
        {
            LRESULT lr = 0;
            text_svc_->TxSendMessage(EM_GETSEL,
                reinterpret_cast<WPARAM>(&sel_start),
                reinterpret_cast<LPARAM>(&sel_end), &lr);
            UINT32 tlen = 0;
            auto layout = build_text_layout(nullptr, nullptr, &tlen);
            if (layout)
            {
                const UINT32 pos = std::min<UINT32>(tlen, sel_end);
                DWRITE_HIT_TEST_METRICS htm{};
                layout->HitTestTextPosition(pos, FALSE,
                    &caret_x_dip, &caret_y_dip, &htm);
                caret_h_dip = htm.height > 2.f ? htm.height : 16.f;
                have_caret = true;
            }
        }

        // Scroll-to-caret.  ComposeBar clamps the host HWND to kMaxHeight, so
        // once wrapped text exceeds that the document is taller than the
        // visible area.  Shift scroll_y_ to keep the caret in view.
        const float scale_for_view  = dip_scale();
        const float visible_h_dip = (client.bottom - client.top) / scale_for_view;
        if (have_caret)
        {
            if (caret_y_dip < scroll_y_)
                scroll_y_ = caret_y_dip;
            else if (caret_y_dip + caret_h_dip > scroll_y_ + visible_h_dip)
                scroll_y_ = caret_y_dip + caret_h_dip - visible_h_dip;
            if (scroll_y_ < 0.f) scroll_y_ = 0.f;
        }
        else
        {
            scroll_y_ = 0.f;
        }

        // Pass the ID2D1DeviceContext (as ID2D1RenderTarget*) to TxDrawD2D.
        // MSFTEDIT QIs for ID2D1DeviceContext internally to enable its full
        // colour-glyph pipeline (required for COLR v1 / PNG emoji fonts).
        // We give RichEdit a generous vertical bound and rely on the clip +
        // transform to mask everything outside the visible window.
        RECTL bounds{client.left, client.top, client.right,
                     client.top + 100000};
        const float client_w_dip = (client.right - client.left) / scale_for_view;
        dc_->PushAxisAlignedClip(
            D2D1::RectF(0.f, 0.f, client_w_dip, visible_h_dip),
            D2D1_ANTIALIAS_MODE_ALIASED);
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(0.f, -scroll_y_));
        in_paint_ = true;
        text_svc_->TxDrawD2D(dc_.Get(), &bounds, nullptr, TXTVIEW_ACTIVE);
        in_paint_ = false;

        // D2D-mode caret: MSFTEDIT never calls ITextHost caret APIs in D2D
        // mode.  Draw a 1.5 DIP rectangle at the layout-computed caret
        // position; the dc transform applies the scroll offset.  Blinking is
        // driven by kRichEditCaretTimerId.
        if (have_caret && sel_start == sel_end &&
            is_caret_on() && GetFocus() == hwnd_)
        {
            using Microsoft::WRL::ComPtr;
            ComPtr<ID2D1SolidColorBrush> caret_brush;
            const tk::Color tc = theme_
                ? theme_->palette.text_primary
                : tk::Color{0, 0, 0, 255};
            dc_->CreateSolidColorBrush(
                D2D1::ColorF(tc.r / 255.f, tc.g / 255.f, tc.b / 255.f),
                caret_brush.GetAddressOf());
            if (caret_brush)
            {
                const D2D1_RECT_F cr = D2D1::RectF(
                    caret_x_dip, caret_y_dip,
                    caret_x_dip + 1.5f, caret_y_dip + caret_h_dip);
                dc_->FillRectangle(cr, caret_brush.Get());
            }
        }

        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        dc_->PopAxisAlignedClip();

        // Placeholder: drawn when the control is empty and not focused.
        if (!placeholder_.empty() && is_text_empty() && GetFocus() != hwnd_)
        {
            using Microsoft::WRL::ComPtr;
            ComPtr<ID2D1SolidColorBrush> brush;
            dc_->CreateSolidColorBrush(
                D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.8f),
                brush.GetAddressOf());
            if (brush && placeholder_fmt_)
            {
                D2D1_RECT_F r = D2D1::RectF(
                    static_cast<float>(client.left) + 8.f,
                    static_cast<float>(client.top) + 4.f,
                    static_cast<float>(client.right) - 8.f,
                    static_cast<float>(client.bottom) - 4.f);
                dc_->DrawText(placeholder_.c_str(),
                              static_cast<UINT32>(placeholder_.size()),
                              placeholder_fmt_.Get(), r, brush.Get());
            }
        }

        HRESULT hr = dc_->EndDraw();
        if (SUCCEEDED(hr))
        {
            swap_chain_->Present(0, 0);
        }
        else if (hr == D2DERR_RECREATE_TARGET)
        {
            dc_->SetTarget(nullptr);
            dc_bitmap_.Reset();
            dc_.Reset();
            swap_chain_.Reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }

    }

    void on_size(int w, int h)
    {
        // Resize the swap chain to match the new HWND dimensions so there is
        // no gap or stretch between paint calls.  If the device context is not
        // yet created, ensure_render_target will create it at the right size.
        if (swap_chain_ && dc_ && w > 0 && h > 0)
        {
            dc_->SetTarget(nullptr);
            dc_bitmap_.Reset();
            swap_chain_->ResizeBuffers(0, static_cast<UINT>(w),
                                       static_cast<UINT>(h),
                                       DXGI_FORMAT_UNKNOWN, 0);
            attach_swap_chain_bitmap();
        }
        if (text_svc_)
            text_svc_->OnTxPropertyBitsChange(TXTBIT_CLIENTRECTCHANGE,
                                               TXTBIT_CLIENTRECTCHANGE);
    }

    // ── Hit-test helper ───────────────────────────────────────────────────
    // Returns true when the caret should be visible (elapsed-time based blink).
    // Because we record focus_tick_ on WM_SETFOCUS (not on each interaction),
    // the phase is never reset by mouse clicks, so blinking stays steady.
    bool is_caret_on() const
    {
        if (blink_ms_ == 0 || blink_ms_ == INFINITE) return true;
        const ULONGLONG elapsed = GetTickCount64() - focus_tick_;
        return (elapsed / blink_ms_) % 2 == 0;
    }

    // Shared: build an IDWriteTextLayout mirroring the current text + format
    // at the visible client width.  Used by every D2D-mode workaround that
    // needs glyph positions: broken EM_POSFROMCHAR, broken WM_LBUTTONDOWN hit
    // test, broken Home/End, and the scroll-to-caret tracking in on_paint.
    Microsoft::WRL::ComPtr<IDWriteTextLayout>
    build_text_layout(float* out_scale, float* out_dip_w,
                      UINT32* out_text_len) const
    {
        using Microsoft::WRL::ComPtr;
        if (out_scale)    *out_scale    = 1.f;
        if (out_dip_w)    *out_dip_w    = 0.f;
        if (out_text_len) *out_text_len = 0;
        if (!dwrite_ || !placeholder_fmt_ || !text_svc_ || !hwnd_)
            return nullptr;

        RECT cli{};
        GetClientRect(hwnd_, &cli);
        const float dpi   = static_cast<float>(GetDpiForWindow(hwnd_));
        const float scale = dpi > 0.f ? dpi / 96.f : 1.f;
        const float dip_w = (cli.right - cli.left) / scale;
        if (out_scale) *out_scale = scale;
        if (out_dip_w) *out_dip_w = dip_w;

        LRESULT tlen = 0;
        const_cast<ITextServices2*>(text_svc_.Get())
            ->TxSendMessage(WM_GETTEXTLENGTH, 0, 0, &tlen);
        std::wstring tw(static_cast<size_t>(tlen), L'\0');
        if (tlen > 0)
            const_cast<ITextServices2*>(text_svc_.Get())
                ->TxSendMessage(WM_GETTEXT,
                    static_cast<WPARAM>(tlen + 1),
                    reinterpret_cast<LPARAM>(tw.data()), &tlen);
        // WM_GETTEXT converts the internal '\r' paragraph separators to
        // '\r\n'; strip the extra '\n' so IDWriteTextLayout character
        // positions match EM_GETSEL / EM_SETSEL offsets exactly.
        {
            std::wstring::size_type w = 0;
            for (std::wstring::size_type r = 0; r < tw.size(); ++r) {
                if (tw[r] == L'\n' && w > 0 && tw[w - 1] == L'\r')
                    continue;
                tw[w++] = tw[r];
            }
            tw.resize(w);
        }
        if (out_text_len) *out_text_len = static_cast<UINT32>(tw.size());

        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_->CreateTextLayout(
                tw.c_str(), static_cast<UINT32>(tw.size()),
                placeholder_fmt_.Get(), dip_w, 100000.f,
                layout.GetAddressOf())))
            return nullptr;
        return layout;
    }

    // MSFTEDIT's WM_LBUTTONDOWN hit test is broken in D2D/windowless mode —
    // it always resolves to position 1 regardless of click x.  This helper
    // uses IDWriteTextLayout::HitTestPoint to map client-pixel coordinates to
    // a UTF-16 character index, which we then push back via EM_SETSEL.
    // y_px is offset by scroll_y_ so clicks land on the right character when
    // the content is scrolled (multi-line composer past kMaxHeight).
    UINT32 hit_test_client_pos(int x_px, int y_px) const
    {
        float scale = 1.f;
        auto layout = build_text_layout(&scale, nullptr, nullptr);
        if (!layout) return 0;
        BOOL isTrailing = FALSE, isInside = FALSE;
        DWRITE_HIT_TEST_METRICS htm{};
        layout->HitTestPoint(x_px / scale,
                              y_px / scale + scroll_y_,
                              &isTrailing, &isInside, &htm);
        return htm.textPosition + (isTrailing ? 1u : 0u);
    }

    // Compute the start/end UTF-16 position of the visual line that contains
    // `caret_pos`.  Used for plain Home/End in D2D mode where MSFTEDIT's own
    // line-boundary computation is broken (it depends on the layout that
    // EM_POSFROMCHAR also returns garbage from).
    void line_bounds_for_pos(UINT32 caret_pos,
                             UINT32& out_line_start,
                             UINT32& out_line_end) const
    {
        out_line_start = caret_pos;
        out_line_end   = caret_pos;
        float dip_w = 0.f;
        UINT32 tlen = 0;
        auto layout = build_text_layout(nullptr, &dip_w, &tlen);
        if (!layout || tlen == 0) return;

        float cx = 0.f, cy = 0.f;
        DWRITE_HIT_TEST_METRICS chtm{};
        layout->HitTestTextPosition(
            std::min<UINT32>(caret_pos, tlen), FALSE,
            &cx, &cy, &chtm);

        const float probe_y = cy + chtm.height * 0.5f;
        BOOL isTrailing = FALSE, isInside = FALSE;
        DWRITE_HIT_TEST_METRICS htm{};

        layout->HitTestPoint(0.f, probe_y, &isTrailing, &isInside, &htm);
        out_line_start = htm.textPosition + (isTrailing ? 1u : 0u);
        if (out_line_start > tlen) out_line_start = tlen;

        const float right_x = dip_w > 1.f ? dip_w - 0.5f : dip_w;
        layout->HitTestPoint(right_x, probe_y, &isTrailing, &isInside, &htm);
        out_line_end = htm.textPosition + (isTrailing ? 1u : 0u);
        if (out_line_end > tlen) out_line_end = tlen;
    }

    // ── Host WndProc ──────────────────────────────────────────────────────

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam)
    {
        Win32RichEditArea* self = reinterpret_cast<Win32RichEditArea*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCPAINT:
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (self)
                self->on_paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (self)
                self->on_size(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_GETDLGCODE:
            if (self && self->text_svc_)
            {
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                return r | DLGC_WANTALLKEYS;
            }
            return DLGC_WANTALLKEYS;

        case WM_KEYDOWN:
        {
            if (!self)
                break;
            // 1. Popup navigation (highest priority).
            if (self->popup_nav_)
            {
                NativeTextArea::NavKey nk{};
                bool is_nav = true;
                switch (wParam)
                {
                case VK_UP:
                    nk = NativeTextArea::NavKey::Up;
                    break;
                case VK_DOWN:
                    nk = NativeTextArea::NavKey::Down;
                    break;
                case VK_LEFT:
                    nk = NativeTextArea::NavKey::Left;
                    break;
                case VK_RIGHT:
                    nk = NativeTextArea::NavKey::Right;
                    break;
                case VK_ESCAPE:
                    nk = NativeTextArea::NavKey::Escape;
                    break;
                case VK_TAB:
                    nk = (GetKeyState(VK_SHIFT) & 0x8000)
                             ? NativeTextArea::NavKey::ShiftTab
                             : NativeTextArea::NavKey::Tab;
                    break;
                default:
                    is_nav = false;
                    break;
                }
                // Copy to guard against re-entrancy: Tab → replace_range →
                // on_changed_ → set_on_popup_nav(nullptr) zeros the closure.
                auto nav = self->popup_nav_;
                if (is_nav && nav && nav(nk))
                    return 0;
            }
            // 2. Up in an empty composer → edit last own message.
            if (wParam == VK_UP && self->on_edit_last_ &&
                self->is_text_empty())
            {
                if (self->on_edit_last_())
                    return 0;
            }
            // 3. Enter without Shift → submit.
            if (wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000))
            {
                if (self->on_submit_)
                    self->on_submit_();
                // Drain the queued WM_CHAR('\r') so it doesn't land in the
                // cleared field after the callback resets the text.
                MSG cr{};
                PeekMessage(&cr, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE);
                return 0;
            }
            // 4. Ctrl+V or Shift+Ins with an image on the clipboard →
            //    intercept BEFORE TxSendMessage.  Windowless RichEdit handles
            //    Ctrl+V internally via OLE and never sends WM_PASTE to the
            //    host HWND, so the WM_PASTE handler below cannot catch it.
            {
                const bool is_ctrl_v =
                    wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000);
                const bool is_shift_ins =
                    wParam == VK_INSERT && (GetKeyState(VK_SHIFT) & 0x8000);
                if ((is_ctrl_v || is_shift_ins) &&
                    self->on_image_paste_ && self->wic_ &&
                    (IsClipboardFormatAvailable(CF_DIBV5) ||
                     IsClipboardFormatAvailable(CF_DIB)))
                {
                    std::vector<std::uint8_t> bytes;
                    if (clipboard_image_to_png(self->wic_, hwnd, bytes))
                    {
                        self->on_image_paste_(std::move(bytes), "image/png");
                        return 0;
                    }
                }
            }
            // 5. Home / End — MSFTEDIT's line-boundary computation depends
            //    on the same layout that EM_POSFROMCHAR / WM_LBUTTONDOWN
            //    derive from, which is broken in D2D windowless mode.  Mirror
            //    the WM_LBUTTONDOWN workaround: compute the target position
            //    via IDWriteTextLayout and push it back through EM_SETSEL.
            if (wParam == VK_HOME || wParam == VK_END)
            {
                const bool is_end   = (wParam == VK_END);
                const bool is_ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool is_shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

                DWORD sel_start = 0, sel_end = 0;
                {
                    LRESULT lr = 0;
                    self->text_svc_->TxSendMessage(EM_GETSEL,
                        reinterpret_cast<WPARAM>(&sel_start),
                        reinterpret_cast<LPARAM>(&sel_end), &lr);
                }
                // EM_GETSEL always returns (min, max); RichEdit doesn't tell
                // us which end is the active caret.  Treat sel_end as active
                // (matches behaviour for the common case of a collapsed
                // selection or a forward Shift-extension) and pick the
                // opposite end as the Shift-anchor based on direction.
                UINT32 new_pos = 0;
                if (is_ctrl)
                {
                    if (is_end)
                    {
                        LRESULT lr = 0;
                        self->text_svc_->TxSendMessage(
                            WM_GETTEXTLENGTH, 0, 0, &lr);
                        new_pos = static_cast<UINT32>(lr);
                    }
                    else
                    {
                        new_pos = 0;
                    }
                }
                else
                {
                    UINT32 line_start = 0, line_end = 0;
                    self->line_bounds_for_pos(sel_end, line_start, line_end);
                    new_pos = is_end ? line_end : line_start;
                }

                const UINT32 anchor = is_shift
                    ? (is_end ? std::min<UINT32>(sel_start, sel_end)
                              : std::max<UINT32>(sel_start, sel_end))
                    : new_pos;

                LRESULT lr2 = 0;
                self->text_svc_->TxSendMessage(EM_SETSEL,
                    static_cast<WPARAM>(anchor),
                    static_cast<LPARAM>(new_pos), &lr2);
                if (!self->in_paint_)
                    InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // 5b. Up / Down — same broken EM_POSFROMCHAR root cause as
            //     Home / End: MSFTEDIT cannot compute the correct cross-line
            //     target in D2D windowless mode.  Use IDWriteTextLayout to
            //     find the character position one visual line above / below
            //     the caret and push it back through EM_SETSEL.
            if (wParam == VK_UP || wParam == VK_DOWN)
            {
                const bool is_up    = (wParam == VK_UP);
                const bool is_shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                DWORD sel_start = 0, sel_end = 0;
                {
                    LRESULT lr2 = 0;
                    self->text_svc_->TxSendMessage(EM_GETSEL,
                        reinterpret_cast<WPARAM>(&sel_start),
                        reinterpret_cast<LPARAM>(&sel_end), &lr2);
                }

                UINT32 tlen = 0;
                auto layout = self->build_text_layout(nullptr, nullptr, &tlen);
                if (layout && tlen > 0)
                {
                    float cx = 0.f, cy = 0.f;
                    DWRITE_HIT_TEST_METRICS chtm{};
                    layout->HitTestTextPosition(
                        std::min<UINT32>(sel_end, tlen),
                        FALSE, &cx, &cy, &chtm);

                    const float half_h   = chtm.height > 0.f
                        ? chtm.height * 0.5f : 8.f;
                    const float target_y = is_up
                        ? (cy - half_h)
                        : (cy + chtm.height + half_h);

                    BOOL trailing = FALSE, inside = FALSE;
                    DWRITE_HIT_TEST_METRICS htm{};
                    layout->HitTestPoint(cx, target_y,
                        &trailing, &inside, &htm);

                    const UINT32 new_pos = std::min<UINT32>(
                        htm.textPosition + (trailing ? 1u : 0u), tlen);
                    // Mirror Home/End shift-anchor convention:
                    // Shift+Up keeps the far end fixed; Shift+Down the near.
                    const UINT32 anchor = is_shift
                        ? (is_up
                            ? std::max<UINT32>(sel_start, sel_end)
                            : std::min<UINT32>(sel_start, sel_end))
                        : new_pos;

                    LRESULT lr2 = 0;
                    self->text_svc_->TxSendMessage(EM_SETSEL,
                        static_cast<WPARAM>(anchor),
                        static_cast<LPARAM>(new_pos), &lr2);
                    if (!self->in_paint_)
                        InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                // layout unavailable or empty text → fall through
            }
            // Fall through to text services.
            if (self->text_svc_)
            {
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                // Arrow keys (Home / End are intercepted above) move the
                // cursor without changing text — EN_CHANGE never fires.
                // Force a repaint so the self-drawn caret follows immediately.
                if (!self->in_paint_)
                    InvalidateRect(hwnd, nullptr, FALSE);
                // Backspace and Delete modify text; notify explicitly in case
                // TxNotify(EN_CHANGE) does not fire in D2D mode.
                if ((wParam == VK_BACK || wParam == VK_DELETE) &&
                    !self->suppress_changed_)
                    self->notify_changed();
                return r;
            }
            break;
        }

        case WM_CHAR:
            // Swallow Tab while a popup nav hook is live (TranslateMessage
            // queues WM_CHAR before WM_KEYDOWN returns, so we eat it here to
            // prevent a literal tab from being inserted and closing the popup).
            if (self && self->popup_nav_ && wParam == VK_TAB)
                return 0;
            if (self && self->text_svc_)
            {
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                // Belt-and-suspenders: TxNotify(EN_CHANGE) may not fire
                // synchronously in D2D mode, so call notify_changed()
                // explicitly.  notify_changed() is idempotent if TxNotify
                // already fired it.
                if (!self->suppress_changed_)
                    self->notify_changed();
                return r;
            }
            break;

        case WM_PASTE:
            // Image paste: intercept before the default text paste path.
            if (self && self->on_image_paste_ && self->wic_ &&
                (IsClipboardFormatAvailable(CF_DIBV5) ||
                 IsClipboardFormatAvailable(CF_DIB)))
            {
                std::vector<std::uint8_t> bytes;
                if (clipboard_image_to_png(self->wic_, hwnd, bytes))
                {
                    self->on_image_paste_(std::move(bytes), "image/png");
                    return 0;
                }
            }
            if (self && self->text_svc_)
            {
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                if (!self->suppress_changed_)
                    self->notify_changed();
                return r;
            }
            break;

        case WM_SETFOCUS:
            if (self)
            {
                // Record the focus timestamp so is_caret_on() can compute
                // phase from elapsed time.  Timer only drives repaints; it
                // does NOT toggle a bool, so re-firing SetTimer here (which
                // resets the countdown) can never suppress a blink.
                self->focus_tick_ = GetTickCount64();
                self->blink_ms_   = GetCaretBlinkTime();
                const UINT blink  = self->blink_ms_;
                if (blink != INFINITE && blink > 0)
                    SetTimer(hwnd, kRichEditCaretTimerId, blink, nullptr);
                if (!self->placeholder_.empty())
                    InvalidateRect(hwnd, nullptr, FALSE);
                if (self->text_svc_)
                {
                    LRESULT r = 0;
                    self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                }
            }
            return 0;

        case WM_KILLFOCUS:
            if (self)
            {
                KillTimer(hwnd, kRichEditCaretTimerId);
                // focus_tick_ is left as-is; is_caret_on() is only queried
                // when GetFocus()==hwnd_, so no caret is drawn after blur.
                if (!self->placeholder_.empty())
                    InvalidateRect(hwnd, nullptr, FALSE);
                if (self->text_svc_)
                {
                    LRESULT r = 0;
                    self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                }
            }
            return 0;

        case WM_TIMER:
            if (self && wParam == kRichEditCaretTimerId)
            {
                // Just trigger a repaint; is_caret_on() computes visibility
                // from elapsed time, so the phase never drifts from resets.
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (self && self->text_svc_)
            {
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (self && self->text_svc_)
            {
                // Explicitly acquire keyboard focus. Standard EDIT controls do
                // this themselves; our custom host HWND must do it too.
                SetFocus(hwnd);
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                // MSFTEDIT's D2D/windowless hit test is broken — it always
                // resolves to position 1.  Use IDWriteTextLayout::HitTestPoint
                // for accurate character placement, then commit via EM_SETSEL.
                {
                    const UINT32 pos = self->hit_test_client_pos(
                        GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                    self->drag_anchor_ = pos;
                    LRESULT lr = 0;
                    self->text_svc_->TxSendMessage(EM_SETSEL,
                        static_cast<WPARAM>(pos),
                        static_cast<LPARAM>(pos), &lr);
                }
                if (!self->in_paint_)
                    InvalidateRect(hwnd, nullptr, FALSE);
                return r;
            }
            break;

        case WM_MOUSEMOVE:
            if (self && self->text_svc_)
            {
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                // When dragging (LButton held), extend the selection from the
                // anchor set on WM_LBUTTONDOWN using our DWrite hit test.
                if (wParam & MK_LBUTTON)
                {
                    const UINT32 pos = self->hit_test_client_pos(
                        GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                    const UINT32 lo = std::min(self->drag_anchor_, pos);
                    const UINT32 hi = std::max(self->drag_anchor_, pos);
                    LRESULT lr = 0;
                    self->text_svc_->TxSendMessage(EM_SETSEL,
                        static_cast<WPARAM>(lo),
                        static_cast<LPARAM>(hi), &lr);
                    if (!self->in_paint_)
                        InvalidateRect(hwnd, nullptr, FALSE);
                }
                return r;
            }
            break;

        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
            if (self && self->text_svc_)
            {
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                return r;
            }
            break;

        case WM_IME_SETCONTEXT:
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_NOTIFY:
        case WM_IME_CHAR:
            if (self && self->text_svc_)
            {
                LRESULT r = 0;
                self->text_svc_->TxSendMessage(msg, wParam, lParam, &r);
                return r;
            }
            break;

        case WM_DESTROY:
            return 0;

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // ── Members ───────────────────────────────────────────────────────────

    float dip_scale() const
    {
        const float dpi = static_cast<float>(GetDpiForWindow(parent_));
        return dpi > 0.f ? dpi / 96.f : 1.f;
    }

    HWND parent_ = nullptr;
    HWND hwnd_   = nullptr; // host HWND (class "tk_re_host")
    int  id_     = 0;
    bool suppress_changed_ = false;
    bool visible_          = true;
    float last_height_     = 0.f;
    Rect  last_rect_       = {-1.f, -1.f, -1.f, -1.f};
    // D2D-mode caret: MSFTEDIT never calls ITextHost caret methods in D2D
    // mode.  We drive blinking via kRichEditCaretTimerId and derive position
    // from IDWriteTextLayout::HitTestTextPosition on each paint.
    // Visibility is computed from elapsed time (GetTickCount64 - focus_tick_)
    // so SetTimer resets only fire off the repaint — not toggle a bool.
    ULONGLONG focus_tick_  = 0;    // GetTickCount64() when we gained focus
    UINT      blink_ms_    = 530;  // GetCaretBlinkTime() result; INFINITE = no blink
    UINT32 drag_anchor_    = 0;    // text position where LButton was pressed
    // Paint-storm guard: set true while inside TxDrawD2D so TxInvalidateRect /
    // TxViewChange don't re-enter on_paint() or queue a follow-up WM_PAINT.
    bool in_paint_       = false;
    // Vertical scroll offset in DIPs.  ComposeBar caps the host HWND height
    // at kMaxHeight; once wrapped text exceeds that, on_paint scrolls the
    // document so the caret stays visible.  D2D-only — MSFTEDIT's own scroll
    // machinery does not work in windowless D2D mode (TxGetScrollBars=0).
    float scroll_y_      = 0.f;

    IWICImagingFactory* wic_         = nullptr; // borrowed; lifetime ≥ this
    ID2D1Device*        d2d_device_  = nullptr; // borrowed; lifetime ≥ this
    ID3D11Device*       d3d_device_  = nullptr; // borrowed; lifetime ≥ this
    IDWriteFactory2*    dwrite_      = nullptr; // borrowed; lifetime ≥ this
    const Theme*        theme_       = nullptr; // borrowed; lifetime ≥ this
    IDWriteFontFace*    noto_face_   = nullptr; // borrowed; Noto Color Emoji face (may be null)

    std::wstring placeholder_; // UTF-16 cue banner drawn in on_paint()

    Microsoft::WRL::ComPtr<ITextServices2>   text_svc_;
    // DXGI flip-model swap chain + ID2D1DeviceContext replace the old
    // ID2D1HwndRenderTarget so TxDrawD2D receives a real device context,
    // which MSFTEDIT QIs for internally to enable its full colour-glyph
    // pipeline (required for COLR v1 / PNG emoji fonts like Noto).
    Microsoft::WRL::ComPtr<IDXGISwapChain1>  swap_chain_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1>     dc_bitmap_; // current back-buffer target
    Microsoft::WRL::ComPtr<IDWriteTextFormat> placeholder_fmt_;

    CHARFORMAT2W char_fmt_{};
    PARAFORMAT2  para_fmt_{};

    std::function<void(const std::string&)>     on_changed_;
    std::function<void()>                       on_submit_;
    std::function<void(float)>                  on_height_changed_;
    ImagePasteHandler                           on_image_paste_;
    std::function<bool(NativeTextArea::NavKey)> popup_nav_;
    std::function<bool()>                       on_edit_last_;

    struct MentionEntry
    {
        std::string visual, user_id, display_name;
        bool is_room;
    };
    std::vector<MentionEntry> mentions_;
};

// Defined in audio_win32.cpp — wired here so Host::make_audio_player() can
// call it without a separate header (mirrors the qt6 / gtk / macos pattern).
std::unique_ptr<tk::AudioPlayer>
make_audio_player_win32(std::function<void(std::function<void()>)> post);

// Defined in audio_capture_win32.cpp.
std::unique_ptr<tk::AudioCapture>
make_audio_capture_win32(tk::AudioCapturePostFn post);

// Defined in video_win32.cpp.
std::unique_ptr<tk::VideoPlayer>
make_video_player_win32(std::function<void(std::function<void()>)> post,
                        tk::d2d::Backend* backend);

// ─────────────────────────────────────────────────────────────────────────
//  Host — owns the tree, paints into the d2d::Surface, dispatches input
// ─────────────────────────────────────────────────────────────────────────

class Host : public tk::Host, public tk::AnimDamageSink
{
public:
    Host(HWND hwnd, const Theme& theme, bool transparent = false)
        : hwnd_(hwnd), theme_(&theme), transparent_(transparent),
          d2d_surface_(std::make_unique<d2d::Surface>(backend_singleton(), hwnd,
                                                      transparent)),
          factory_(d2d::make_factory(backend_singleton()))
    {
    }

    void request_repaint() override
    {
        if (hwnd_)
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void set_anim_cache(const tk::AnimImageCache* cache)
    {
        anim_cache_ = cache;
    }

    // AnimDamageSink: record animated-image rects drawn during this paint.
    void note_image(const std::string& key, tk::Rect world) override
    {
        if (anim_cache_ && anim_cache_->has(key))
            anim_damage_.push_back(world);
    }

    // Invalidate just the rects that contain animated images from the last paint.
    void invalidate_anim_damage()
    {
        if (!hwnd_ || anim_damage_.empty())
            return;
        for (const auto& r : anim_damage_)
        {
            RECT rc;
            rc.left   = static_cast<LONG>(std::floor(r.x)) - 1;
            rc.top    = static_cast<LONG>(std::floor(r.y)) - 1;
            rc.right  = static_cast<LONG>(std::ceil(r.x + r.w)) + 1;
            rc.bottom = static_cast<LONG>(std::ceil(r.y + r.h)) + 1;
            InvalidateRect(hwnd_, &rc, FALSE);
        }
    }

    void post_to_ui(std::function<void()> task) override
    {
        if (!hwnd_)
        {
            return;
        }
        auto* heap = new std::function<void()>(std::move(task));
        if (!PostMessageW(hwnd_, post_to_ui_message(), 0,
                          reinterpret_cast<LPARAM>(heap)))
        {
            // PostMessage failed; reclaim the heap copy so we don't leak.
            delete heap;
        }
    }

    void post_delayed(int ms, std::function<void()> fn) override
    {
        if (!hwnd_)
        {
            return;
        }
        // Sleep on a detached one-shot thread, then marshal back through
        // the existing post_to_ui channel. We capture hwnd_ + the message
        // id by value (never `this`) so a Host destroyed within `ms` is
        // safe: PostMessageW to a dead HWND just fails and we free the
        // heap copy. Room switches are infrequent and superseded, so the
        // per-call thread cost is negligible.
        HWND hwnd = hwnd_;
        UINT msg = post_to_ui_message();
        std::thread(
            [ms, hwnd, msg, fn = std::move(fn)]() mutable
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                auto* heap = new std::function<void()>(std::move(fn));
                if (!PostMessageW(hwnd, msg, 0, reinterpret_cast<LPARAM>(heap)))
                {
                    delete heap;
                }
            })
            .detach();
    }

    std::unique_ptr<NativeTextField> make_text_field() override
    {
        int id = next_ctrl_id_++;
        auto field = std::make_unique<Win32NativeTextField>(hwnd_, id);
        fields_by_id_.emplace(id, field.get());
        return field;
    }

    std::unique_ptr<NativeTextArea> make_text_area() override
    {
        int id = next_ctrl_id_++;
        auto fac = d2d::factories(backend_singleton());
        auto area = std::make_unique<Win32RichEditArea>(
            hwnd_, id, fac.wic, fac.d2d_device, fac.d3d_device,
            fac.dwrite, theme_, fac.noto_emoji_face);
        areas_by_id_.emplace(id, area.get());
        return area;
    }

    std::unique_ptr<AudioPlayer> make_audio_player() override
    {
        return make_audio_player_win32(
            [this](std::function<void()> fn)
            {
                post_to_ui(std::move(fn));
            });
    }
    std::unique_ptr<AudioCapture> make_audio_capture() override
    {
        return make_audio_capture_win32(
            [this](std::function<void()> fn)
            {
                post_to_ui(std::move(fn));
            });
    }
    std::unique_ptr<VideoPlayer> make_video_player() override
    {
        return make_video_player_win32(
            [this](std::function<void()> fn)
            {
                post_to_ui(std::move(fn));
            },
            &backend_singleton());
    }

    EncodedImage encode_for_send(const std::uint8_t* data, std::size_t len,
                                 bool compress) override
    {
        EncodedImage out{};
        if (!data || len == 0)
        {
            return out;
        }

        using Microsoft::WRL::ComPtr;
        IWICImagingFactory* wic = d2d::factories(backend_singleton()).wic;
        if (!wic)
        {
            return out;
        }

        // Decode to inspect dimensions + source format.
        ComPtr<IWICStream> stream;
        if (FAILED(wic->CreateStream(stream.GetAddressOf())))
        {
            return out;
        }
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                                static_cast<DWORD>(len))))
        {
            return out;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnLoad,
                                                decoder.GetAddressOf())))
        {
            return out;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
        {
            return out;
        }

        UINT src_w = 0, src_h = 0;
        frame->GetSize(&src_w, &src_h);

        if (!compress)
        {
            out.bytes.assign(data, data + len);
            GUID container = {};
            decoder->GetContainerFormat(&container);
            if (container == GUID_ContainerFormatPng)
            {
                out.mime = "image/png";
            }
            else if (container == GUID_ContainerFormatJpeg)
            {
                out.mime = "image/jpeg";
            }
            else if (container == GUID_ContainerFormatGif)
            {
                out.mime = "image/gif";
            }
            else if (container == GUID_ContainerFormatBmp)
            {
                out.mime = "image/bmp";
            }
            else
            {
                out.mime = "image/png";
            }
            out.width = src_w;
            out.height = src_h;
            return out;
        }

        constexpr UINT kMaxW = 1600;
        constexpr UINT kMaxH = 1200;
        UINT dst_w = src_w, dst_h = src_h;
        if (src_w > kMaxW || src_h > kMaxH)
        {
            double s = std::min({1.0, static_cast<double>(kMaxW) / src_w,
                                 static_cast<double>(kMaxH) / src_h});
            dst_w = std::max<UINT>(1, static_cast<UINT>(std::round(src_w * s)));
            dst_h = std::max<UINT>(1, static_cast<UINT>(std::round(src_h * s)));
        }

        ComPtr<IWICBitmapSource> source;
        if (dst_w != src_w || dst_h != src_h)
        {
            ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())))
            {
                return EncodedImage{};
            }
            if (FAILED(scaler->Initialize(frame.Get(), dst_w, dst_h,
                                          WICBitmapInterpolationModeFant)))
            {
                return EncodedImage{};
            }
            source = scaler;
        }
        else
        {
            source = frame;
        }

        // Encode JPEG into an in-memory IStream.
        ComPtr<IStream> mem;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, mem.GetAddressOf())))
        {
            return EncodedImage{};
        }
        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                                      encoder.GetAddressOf())))
        {
            return EncodedImage{};
        }
        if (FAILED(encoder->Initialize(mem.Get(), WICBitmapEncoderNoCache)))
        {
            return EncodedImage{};
        }
        ComPtr<IWICBitmapFrameEncode> out_frame;
        ComPtr<IPropertyBag2> props;
        if (FAILED(encoder->CreateNewFrame(out_frame.GetAddressOf(),
                                           props.GetAddressOf())))
        {
            return EncodedImage{};
        }
        // Quality 0.75.
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.75f;
        props->Write(1, &opt, &v);
        VariantClear(&v);
        if (FAILED(out_frame->Initialize(props.Get())))
        {
            return EncodedImage{};
        }
        if (FAILED(out_frame->SetSize(dst_w, dst_h)))
        {
            return EncodedImage{};
        }
        if (FAILED(out_frame->WriteSource(source.Get(), nullptr)))
        {
            return EncodedImage{};
        }
        if (FAILED(out_frame->Commit()))
        {
            return EncodedImage{};
        }
        if (FAILED(encoder->Commit()))
        {
            return EncodedImage{};
        }

        HGLOBAL h_out = nullptr;
        if (FAILED(GetHGlobalFromStream(mem.Get(), &h_out)) || !h_out)
        {
            return EncodedImage{};
        }
        SIZE_T n = GlobalSize(h_out);
        void* p = GlobalLock(h_out);
        if (!p || n == 0)
        {
            if (p)
            {
                GlobalUnlock(h_out);
            }
            return EncodedImage{};
        }
        out.bytes.assign(static_cast<const std::uint8_t*>(p),
                         static_cast<const std::uint8_t*>(p) + n);
        GlobalUnlock(h_out);
        out.mime = "image/jpeg";
        out.width = dst_w;
        out.height = dst_h;
        return out;
    }

    void set_clipboard_text(std::string_view text) override
    {
        std::wstring wide = utf8_to_wide(std::string(text));
        if (!OpenClipboard(hwnd_))
            return;
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE,
                                    (wide.size() + 1) * sizeof(wchar_t));
        if (hMem)
        {
            auto* dst = static_cast<wchar_t*>(GlobalLock(hMem));
            if (dst)
            {
                std::copy(wide.begin(), wide.end(), dst);
                dst[wide.size()] = L'\0';
                GlobalUnlock(hMem);
            }
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }

    // Look up the NativeTextField owning a child EDIT control by ID.
    Win32NativeTextField* field_by_id(int id)
    {
        auto it = fields_by_id_.find(id);
        return it == fields_by_id_.end() ? nullptr : it->second;
    }
    Win32TextAreaBase* area_by_id(int id)
    {
        auto it = areas_by_id_.find(id);
        return it == areas_by_id_.end() ? nullptr : it->second;
    }

    // ── Internal ──────────────────────────────────────────────────────
    void set_root(std::unique_ptr<Widget> root)
    {
        root_ = std::move(root);
        relayout();
    }
    Widget* root() const
    {
        return root_.get();
    }
    const Theme& theme() const
    {
        return *theme_;
    }
    void set_theme(const Theme& t)
    {
        theme_ = &t;
        for (auto& [id, area] : areas_by_id_)
            area->on_theme_changed(t);
        // Invalidate the host HWND and all native-control child HWNDs so
        // WM_CTLCOLOREDIT / WM_PAINT fire with the new palette.
        if (hwnd_)
            RedrawWindow(hwnd_, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    CanvasFactory& factory()
    {
        return *factory_;
    }
    HWND hwnd() const
    {
        return hwnd_;
    }

    void relayout()
    {
        if (!root_ || !hwnd_)
        {
            return;
        }
        RECT rc;
        GetClientRect(hwnd_, &rc);
        LayoutCtx ctx{*factory_, *theme_};
        Rect bounds{0, 0,
                    phys_to_dip(static_cast<float>(rc.right - rc.left)),
                    phys_to_dip(static_cast<float>(rc.bottom - rc.top))};
        root_->measure(ctx, {bounds.w, bounds.h});
        root_->arrange(ctx, bounds);
        if (on_layout_)
        {
            on_layout_();
        }
        request_repaint();
    }

    void set_on_layout(std::function<void()> cb)
    {
        on_layout_ = std::move(cb);
    }

    void on_resize()
    {
        if (!hwnd_)
        {
            return;
        }
        RECT rc;
        GetClientRect(hwnd_, &rc);
        d2d_surface_->resize(rc.right - rc.left, rc.bottom - rc.top);
        relayout();
    }

    void on_paint()
    {
        if (!hwnd_)
        {
            return;
        }
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);

        Canvas& canvas = d2d_surface_->begin_paint();
        // Transparent surfaces (overlays) clear to fully transparent so DWM
        // composites the per-pixel alpha against the content behind the window.
        canvas.clear(transparent_ ? Color{0, 0, 0, 0} : theme_->palette.bg);
        if (root_)
        {
            pending_popup_ = nullptr;
            anim_damage_.clear();
            PaintCtx ctx{canvas, *factory_, *theme_, this, this};
            root_->paint(ctx);
            popup_ = pending_popup_;
            root_->paint_overlay(ctx);
        }
        if (drag_active_)
        {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            float w = static_cast<float>(rc.right - rc.left);
            float h = static_cast<float>(rc.bottom - rc.top);
            const float inset = 8.0f;
            Rect area{inset, inset, std::max(0.0f, w - inset * 2),
                      std::max(0.0f, h - inset * 2)};
            if (area.w > 0 && area.h > 0)
            {
                Color accent = theme_->palette.accent;
                Color fill = accent;
                fill.a = 28;
                Color stroke = accent;
                stroke.a = 192;
                canvas.fill_rounded_rect(area, 12.0f, fill);
                canvas.stroke_rounded_rect(area, 12.0f, stroke, 2.0f);
                TextStyle st{};
                st.role = FontRole::Title;
                auto layout = factory_->build_text("Drop to attach", st);
                if (layout)
                {
                    Size sz = layout->measure();
                    canvas.draw_text(*layout,
                                     {area.x + (area.w - sz.w) * 0.5f,
                                      area.y + (area.h - sz.h) * 0.5f},
                                     accent);
                }
            }
        }
        bool lost = d2d_surface_->end_paint();
        EndPaint(hwnd_, &ps);
        if (lost)
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void on_pointer_down(int x, int y)
    {
        fire_user_activity_();
        if (!root_)
        {
            return;
        }
        SetCapture(hwnd_);
        Point local{phys_to_dip(static_cast<float>(x)),
                    phys_to_dip(static_cast<float>(y))};
        pressed_widget_ = root_->dispatch_pointer_down(local);
        if (pressed_widget_)
        {
            request_repaint();
        }
    }

    void on_pointer_up(int x, int y)
    {
        if (GetCapture() == hwnd_)
        {
            ReleaseCapture();
        }
        if (!pressed_widget_)
        {
            return;
        }
        Point world{phys_to_dip(static_cast<float>(x)),
                    phys_to_dip(static_cast<float>(y))};
        Point ws = pressed_widget_->world_to_local(world);
        bool inside =
            (ws.x >= 0 && ws.y >= 0 && ws.x < pressed_widget_->bounds().w &&
             ws.y < pressed_widget_->bounds().h);
        pressed_widget_->on_pointer_up(ws, inside);
        pressed_widget_ = nullptr;
        request_repaint();
    }

    void on_pointer_move(int x, int y)
    {
        if (!root_)
        {
            return;
        }
        Point local{phys_to_dip(static_cast<float>(x)),
                    phys_to_dip(static_cast<float>(y))};
        if (pressed_widget_)
        {
            Point ws = pressed_widget_->world_to_local(local);
            pressed_widget_->on_pointer_drag(ws);
            request_repaint();
            return;
        }
        Widget* hit = root_->hit_test(local);
        Button* hovered = dynamic_cast<Button*>(hit);
        bool btn_changed = (hovered != hovered_btn_);
        if (btn_changed)
        {
            if (hovered_btn_)
            {
                hovered_btn_->set_hovered(false);
            }
            hovered_btn_ = hovered;
            if (hovered_btn_)
            {
                hovered_btn_->set_hovered(true);
            }
        }
        bool dirty = false;
        Widget* moved = root_->dispatch_pointer_move(local, &dirty);
        bool widget_changed = (moved != hovered_widget_);
        if (widget_changed)
        {
            if (hovered_widget_)
            {
                hovered_widget_->on_pointer_leave();
            }
            hovered_widget_ = moved;
        }
        if (btn_changed || widget_changed || dirty)
        {
            request_repaint();
        }
    }

    void on_pointer_leave()
    {
        if (hovered_btn_)
        {
            hovered_btn_->set_hovered(false);
            hovered_btn_ = nullptr;
        }
        if (hovered_widget_)
        {
            hovered_widget_->on_pointer_leave();
            hovered_widget_ = nullptr;
        }
        if (pressed_widget_)
        {
            pressed_widget_->on_pointer_up({-1, -1}, false);
            pressed_widget_ = nullptr;
        }
        request_repaint();
    }

    void on_wheel(int screen_x, int screen_y, int delta_steps)
    {
        fire_user_activity_();
        if (!root_ || !hwnd_)
        {
            return;
        }
        POINT pt{screen_x, screen_y};
        ScreenToClient(hwnd_, &pt);
        // WM_MOUSEWHEEL: positive WHEEL_DELTA = forward away from user.
        // The toolkit convention is positive dy = scroll content down,
        // so invert. One notch (120) maps to ~3 toolkit pixels per step.
        float dy = static_cast<float>(-delta_steps) * (3.0f / 120.0f) * 30.0f;
        if (root_->dispatch_wheel(
                {phys_to_dip(static_cast<float>(pt.x)),
                 phys_to_dip(static_cast<float>(pt.y))},
                0, dy))
        {
            request_repaint();
            on_pointer_move(pt.x, pt.y);
        }
    }

    void detach()
    {
        hwnd_ = nullptr;
    }

    // Cursor management. LoadCursor() returns a process-shared handle that
    // doesn't require DestroyCursor, so caching by raw value is safe.
    HCURSOR current_cursor() const
    {
        return current_cursor_;
    }
    void set_cursor(Cursor c)
    {
        HCURSOR newc = LoadCursorW(
            nullptr, (c == Cursor::Pointer) ? IDC_HAND : IDC_ARROW);
        if (newc == current_cursor_) return;
        current_cursor_ = newc;
        // Apply immediately so the change is visible before the next
        // WM_SETCURSOR. SetCursor only affects the visible cursor when the
        // pointer is over the calling thread's window, so this is a no-op
        // when the user has moved off the window entirely.
        SetCursor(newc);
    }

private:
    HWND hwnd_;
    const Theme* theme_;
    bool transparent_ = false;
    std::unique_ptr<d2d::Surface> d2d_surface_;
    std::unique_ptr<CanvasFactory> factory_;
    std::unique_ptr<Widget> root_;
    std::function<void()> on_layout_;
    Widget* pressed_widget_ = nullptr;
    Button* hovered_btn_ = nullptr;
    Widget* hovered_widget_ = nullptr;
    HCURSOR current_cursor_ = LoadCursorW(nullptr, IDC_ARROW);
    const tk::AnimImageCache* anim_cache_ = nullptr;
    std::vector<tk::Rect> anim_damage_;

    int next_ctrl_id_ = 0x4000;
    std::unordered_map<int, Win32NativeTextField*> fields_by_id_;
    std::unordered_map<int, Win32TextAreaBase*> areas_by_id_;

    float dpi_scale() const
    {
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        return dpi > 0.f ? dpi / 96.f : 1.f;
    }
    float phys_to_dip(float px) const { return px / dpi_scale(); }
    int dip_to_phys(float dip) const
    {
        return static_cast<int>(std::round(dip * dpi_scale()));
    }

public:
    void set_on_file_drop(FileDropHandler cb)
    {
        on_file_drop_ = std::move(cb);
    }
    void set_on_file_drop_error(FileDropErrorHandler cb)
    {
        on_file_drop_error_ = std::move(cb);
    }
    bool has_file_drop_handler() const
    {
        return static_cast<bool>(on_file_drop_);
    }
    void fire_file_drop(std::vector<std::uint8_t> bytes, std::string mime,
                        std::string filename)
    {
        if (on_file_drop_)
        {
            on_file_drop_(std::move(bytes), std::move(mime),
                          std::move(filename));
        }
    }
    void fire_file_drop_error(std::string reason)
    {
        if (on_file_drop_error_)
            on_file_drop_error_(std::move(reason));
    }
    void set_on_right_click(std::function<void(tk::Point)> cb)
    {
        on_right_click_ = std::move(cb);
    }
    void fire_right_click(int x, int y)
    {
        tk::Point pt{phys_to_dip(static_cast<float>(x)),
                     phys_to_dip(static_cast<float>(y))};
        if (root_)
            root_->dispatch_right_click(pt);
        if (on_right_click_)
            on_right_click_(pt);
    }
    void set_drag_active(bool active)
    {
        if (drag_active_ == active)
        {
            return;
        }
        drag_active_ = active;
        if (hwnd_)
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    bool drag_active() const
    {
        return drag_active_;
    }

private:
    FileDropHandler on_file_drop_;
    FileDropErrorHandler on_file_drop_error_;
    std::function<void(tk::Point)> on_right_click_;
    bool drag_active_ = false;
};

// ─────────────────────────────────────────────────────────────────────────
//  Surface — child HWND + window-class registration
// ─────────────────────────────────────────────────────────────────────────

namespace
{

constexpr const wchar_t* kSurfaceClass = L"tk_win32_Surface";

LRESULT CALLBACK surface_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam)
{
    Host* host =
        reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // The runtime-registered post_to_ui message — handled before any
    // standard switch so it isn't confused with normal Win32 messages.
    if (host && msg == post_to_ui_message())
    {
        auto* fn = reinterpret_cast<std::function<void()>*>(lParam);
        if (fn)
        {
            if (*fn)
            {
                (*fn)();
            }
            delete fn;
        }
        return 0;
    }

    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_MOUSEACTIVATE:
        // Bring the top-level window to the foreground when clicked while
        // the app is inactive, but don't let the Surface steal keyboard
        // focus from native overlays (NativeTextArea, NativeTextField).
        // Without the SetForegroundWindow call the MA_NOACTIVATE return
        // suppresses the normal parent-activation that DefWindowProc would
        // perform, so the window never comes to front.
        if (HWND root = GetAncestor(hwnd, GA_ROOT))
            SetForegroundWindow(root);
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1; // we paint the full client area in WM_PAINT
    case WM_PAINT:
        if (host)
        {
            host->on_paint();
        }
        else
        {
            ValidateRect(hwnd, nullptr);
        }
        return 0;
    case WM_SIZE:
        if (host)
        {
            host->on_resize();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (host)
        {
            host->on_pointer_down(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;
    case WM_LBUTTONUP:
        if (host)
        {
            host->on_pointer_up(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;
    case WM_RBUTTONUP:
        if (host)
        {
            host->fire_right_click(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;
    case WM_SETCURSOR:
        // Override the window-class arrow only for the surface's own canvas
        // pixels (wParam == this HWND, hit-test == HTCLIENT). When the
        // cursor is over a child HWND — e.g. a Win32NativeTextField /
        // NativeTextArea EDIT control — wParam is the child's HWND; we
        // must NOT return TRUE there or the child's default WndProc never
        // runs and the I-beam is suppressed. Falling through to
        // DefWindowProc lets the message bubble back to the child.
        if (host && reinterpret_cast<HWND>(wParam) == hwnd &&
            LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(host->current_cursor());
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
    {
        if (host)
        {
            host->on_pointer_move(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        // Subscribe to WM_MOUSELEAVE for hover-out tracking.
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        // Guard against spurious WM_MOUSELEAVE that Windows delivers when a
        // WS_EX_TOPMOST popup (e.g. a tracking tooltip) appears over this
        // HWND. If the cursor is still physically inside our client rect, the
        // leave is false and we must ignore it — otherwise the tooltip hides
        // the instant it appears.
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor))
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (PtInRect(&rc, cursor))
                return 0;
        }
        if (host)
            host->on_pointer_leave();
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        if (host)
        {
            short delta = static_cast<short>(HIWORD(wParam));
            // WM_MOUSEWHEEL coordinates are in screen pixels; the
            // host converts via ScreenToClient.
            host->on_wheel(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), delta);
        }
        return 0;
    }
    case WM_CTLCOLOREDIT:
    {
        // Paint EDIT-control backgrounds with the theme's input-card colour
        // instead of the system default white. Multi-line areas (compose
        // bar) use compose_card_bg; single-line fields use bg.
        if (host)
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND ctrl = reinterpret_cast<HWND>(lParam);
            int id = GetDlgCtrlID(ctrl);
            const auto& pal = host->theme().palette;
            tk::Color col = host->area_by_id(id) ? pal.compose_card_bg : pal.bg;
            COLORREF bg = RGB(col.r, col.g, col.b);
            SetBkColor(dc, bg);
            SetTextColor(dc, RGB(pal.text_primary.r, pal.text_primary.g,
                                 pal.text_primary.b));
            SetDCBrushColor(dc, bg);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        break;
    }
    case WM_COMMAND:
    {
        // EN_CHANGE from a child EDIT belonging to one of our
        // NativeTextField overlays. wParam HIWORD = notification,
        // LOWORD = control id.
        int id = LOWORD(wParam);
        WORD code = HIWORD(wParam);
        if (host && code == EN_CHANGE)
        {
            if (auto* f = host->field_by_id(id))
            {
                f->notify_changed();
            }
            else if (auto* a = host->area_by_id(id))
            {
                a->notify_changed();
            }
        }
        return 0;
    }
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── DropTarget — OLE IDropTarget that funnels image drops to a Host ──
//
// One instance per Surface. The Host is borrowed; the Surface calls
// RegisterDragDrop in its ctor and RevokeDragDrop + Release in its dtor,
// so the lifetime of the COM ref overlaps the Host's lifetime safely.

class DropTarget final : public IDropTarget
{
public:
    explicit DropTarget(Host* host) : host_(host)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropTarget))
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs_;
        if (n == 0)
        {
            delete this;
        }
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,
                                        DWORD /*grfKeyState*/, POINTL /*pt*/,
                                        DWORD* pdwEffect) override
    {
        if (!pdwEffect)
        {
            return E_POINTER;
        }
        accept_ = host_ && host_->has_file_drop_handler() && acceptable(data);
        *pdwEffect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accept_ && host_)
        {
            host_->set_drag_active(true);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD /*grfKeyState*/, POINTL /*pt*/,
                                       DWORD* pdwEffect) override
    {
        if (!pdwEffect)
        {
            return E_POINTER;
        }
        *pdwEffect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        accept_ = false;
        if (host_)
        {
            host_->set_drag_active(false);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD /*grfKeyState*/,
                                   POINTL /*pt*/, DWORD* pdwEffect) override
    {
        if (pdwEffect)
        {
            *pdwEffect = DROPEFFECT_NONE;
        }
        if (host_)
        {
            host_->set_drag_active(false);
        }
        if (!accept_ || !host_)
        {
            return S_OK;
        }

        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg{};
        if (FAILED(data->GetData(&fe, &stg)))
        {
            return S_OK;
        }
        HDROP hdrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
        bool dispatched = false;
        if (hdrop)
        {
            UINT n_files = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n_files; ++i)
            {
                UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                if (len == 0)
                {
                    continue;
                }
                std::wstring path(len, L'\0');
                if (DragQueryFileW(hdrop, i, path.data(), len + 1) == 0)
                {
                    continue;
                }
                if (try_dispatch_file(path))
                {
                    dispatched = true;
                }
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);

        if (dispatched && pdwEffect)
        {
            *pdwEffect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

    void detach_host()
    {
        host_ = nullptr;
    }

private:
    // Extension → MIME table used as a fallback when content-sniffing
    // via FindMimeFromData isn't conclusive. Covers the common chat
    // payloads; the rest fall back to application/octet-stream.
    static const char* mime_from_ext(const std::wstring& ext_lower)
    {
        if (ext_lower == L"png")
        {
            return "image/png";
        }
        if (ext_lower == L"jpg" || ext_lower == L"jpeg")
        {
            return "image/jpeg";
        }
        if (ext_lower == L"webp")
        {
            return "image/webp";
        }
        if (ext_lower == L"bmp")
        {
            return "image/bmp";
        }
        if (ext_lower == L"gif")
        {
            return "image/gif";
        }
        if (ext_lower == L"pdf")
        {
            return "application/pdf";
        }
        if (ext_lower == L"zip")
        {
            return "application/zip";
        }
        if (ext_lower == L"txt")
        {
            return "text/plain";
        }
        if (ext_lower == L"json")
        {
            return "application/json";
        }
        return nullptr;
    }

    static std::wstring path_extension_lower(const std::wstring& p)
    {
        size_t slash = p.find_last_of(L"\\/");
        size_t dot = p.find_last_of(L'.');
        if (dot == std::wstring::npos ||
            (slash != std::wstring::npos && dot < slash))
        {
            return {};
        }
        std::wstring ext = p.substr(dot + 1);
        for (wchar_t& c : ext)
        {
            if (c >= L'A' && c <= L'Z')
            {
                c = static_cast<wchar_t>(c + (L'a' - L'A'));
            }
        }
        return ext;
    }

    static std::wstring basename(const std::wstring& p)
    {
        size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? p : p.substr(slash + 1);
    }

    // Returns true when the drop carries at least one local file.
    static bool acceptable(IDataObject* data)
    {
        if (!data)
        {
            return false;
        }
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return data->QueryGetData(&fe) == S_OK;
    }

    bool try_dispatch_file(const std::wstring& path)
    {
        if (!host_)
        {
            return false;
        }

        // Size guard via GetFileAttributesEx — single syscall, no open.
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        {
            if (host_)
                host_->fire_file_drop_error("Could not read file");
            return false;
        }
        ULARGE_INTEGER sz{};
        sz.LowPart = fa.nFileSizeLow;
        sz.HighPart = fa.nFileSizeHigh;
        if (sz.QuadPart == 0 || sz.QuadPart > kMaxDroppedFileBytes)
        {
            return false;
        }

        HANDLE h =
            CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            if (host_)
                host_->fire_file_drop_error("Could not read file");
            return false;
        }

        std::vector<std::uint8_t> bytes(static_cast<size_t>(sz.QuadPart));
        DWORD read_total = 0;
        while (read_total < bytes.size())
        {
            DWORD got = 0;
            BOOL ok = ReadFile(h, bytes.data() + read_total,
                               static_cast<DWORD>(bytes.size() - read_total),
                               &got, nullptr);
            if (!ok || got == 0)
            {
                break;
            }
            read_total += got;
        }
        CloseHandle(h);
        if (read_total != bytes.size())
        {
            return false;
        }

        // Mime: extension table first (cheap), default to
        // application/octet-stream when unknown. FindMimeFromData would
        // need urlmon.lib; the table covers the common chat payloads.
        std::string mime = "application/octet-stream";
        if (const char* m = mime_from_ext(path_extension_lower(path)))
        {
            mime = m;
        }

        host_->fire_file_drop(std::move(bytes), std::move(mime),
                              wide_to_utf8(basename(path)));
        return true;
    }

    Host* host_;
    std::atomic<ULONG> refs_{1};
    bool accept_ = false;
};

bool ensure_class_registered(HINSTANCE inst)
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &surface_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we paint everything
    wc.lpszClassName = kSurfaceClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }
    registered = true;
    return true;
}

} // namespace

// One IDropTarget per Surface, indexed by hwnd so the dtor can find it
// at teardown. Keeping it out of Host avoids forward-declaration churn
// (DropTarget is defined later in this TU than Host).
namespace
{
std::unordered_map<HWND, DropTarget*>& drop_targets_by_hwnd()
{
    static std::unordered_map<HWND, DropTarget*> instance;
    return instance;
}
} // namespace

Surface::Surface(HINSTANCE inst, HWND parent, const Theme& theme,
                 bool transparent)
{
    if (!ensure_class_registered(inst))
    {
        return;
    }

    // WS_EX_NOREDIRECTIONBITMAP is required for DXGI_ALPHA_MODE_PREMULTIPLIED
    // swap chains: it tells DWM not to create a GDI redirection surface for
    // this HWND, so the flip-model swap chain's alpha channel reaches the
    // compositor unchanged.
    const DWORD ex_style = transparent ? WS_EX_NOREDIRECTIONBITMAP : 0;
    HWND hwnd = CreateWindowExW(ex_style, kSurfaceClass, L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN |
                                    WS_CLIPSIBLINGS,
                                0, 0, 100, 100, parent, nullptr, inst,
                                /*lpCreateParams=*/nullptr);
    if (!hwnd)
    {
        return;
    }

    host_ = std::make_unique<Host>(hwnd, theme, transparent);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(host_.get()));

    // Register an OLE drop target. The handler isn't wired yet — the
    // target stays a no-op until the shell calls set_on_image_drop.
    // RegisterDragDrop fails silently when the caller hasn't OleInitialize'd
    // their thread; the shell is responsible for that (main.cpp).
    auto* dt = new DropTarget(host_.get());
    if (SUCCEEDED(RegisterDragDrop(hwnd, dt)))
    {
        drop_targets_by_hwnd().emplace(hwnd, dt);
    }
    else
    {
        dt->Release();
    }
}

Surface::~Surface()
{
    if (host_ && host_->hwnd())
    {
        HWND hwnd = host_->hwnd();
        auto& map = drop_targets_by_hwnd();
        auto it = map.find(hwnd);
        if (it != map.end())
        {
            RevokeDragDrop(hwnd);
            it->second->detach_host();
            it->second->Release();
            map.erase(it);
        }
        host_->detach();
        DestroyWindow(hwnd);
    }
}

HWND Surface::hwnd() const
{
    return host_ ? host_->hwnd() : nullptr;
}

tk::Host& Surface::host()
{
    return *host_;
}
const Theme& Surface::theme() const
{
    return host_->theme();
}

void Surface::set_root(std::unique_ptr<Widget> root)
{
    host_->set_root(std::move(root));
}

Widget* Surface::root() const
{
    return host_->root();
}

void Surface::relayout()
{
    host_->relayout();
}

void Surface::set_theme(const Theme& t)
{
    host_->set_theme(t);
    relayout();
}

void Surface::set_anim_cache(const tk::AnimImageCache* cache)
{
    host_->set_anim_cache(cache);
}

void Surface::update_anim_regions()
{
    host_->invalidate_anim_damage();
}

void Surface::set_on_layout(std::function<void()> cb)
{
    host_->set_on_layout(std::move(cb));
}

CanvasFactory& Surface::factory()
{
    return host_->factory();
}

void Surface::set_on_file_drop(FileDropHandler cb)
{
    host_->set_on_file_drop(std::move(cb));
}

void Surface::set_on_file_drop_error(FileDropErrorHandler cb)
{
    host_->set_on_file_drop_error(std::move(cb));
}

void Surface::set_on_right_click(std::function<void(tk::Point)> cb)
{
    host_->set_on_right_click(std::move(cb));
}

void Surface::set_cursor(Cursor c)
{
    host_->set_cursor(c);
}

std::vector<tk::d2d::AnimatedFrame>
decode_animation(std::span<const std::uint8_t> bytes)
{
    return tk::d2d::decode_animation(backend_singleton(), bytes);
}

IDWriteFontFallback* dwrite_font_fallback()
{
    return tk::d2d::factories(backend_singleton()).font_fallback;
}

} // namespace tk::win32
