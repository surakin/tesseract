#include "host_win32.h"
#include "anim_image_cache.h"
#include "canvas_d2d.h"
#include "controls.h"
#include "views/html_spans.h"

#include <BetterText/BetterText.h>

#include <tesseract/settings.h>

#include <commctrl.h>
#include <d2d1.h>           // ID2D1HwndRenderTarget, D2D1_RENDER_TARGET_PROPERTIES
#include <d2d1_1.h>         // ID2D1Factory1, ID2D1DeviceContext, ID2D1Bitmap1
#include <d2d1_1helper.h>   // D2D1::BitmapProperties1
#include <d3d11.h>          // ID3D11Device (for swap chain creation)
#include <dxgi1_2.h>        // IDXGISwapChain1, IDXGIFactory2
#include <dwrite.h>         // IDWriteTextFormat, DWRITE_FONT_WEIGHT_* enums
#include <dwrite_2.h>       // IDWriteFactory2::CreateTextFormat
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
#include <initguid.h>
#include <netlistmgr.h>

// Device enumeration: WASAPI (audio) + Media Foundation (camera).
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mfidl.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tk::win32
{

// ─────────────────────────────────────────────────────────────────────────
//  Process-wide D2D backend + post-to-UI message
// ─────────────────────────────────────────────────────────────────────────

// External linkage (declared in host_win32.h). The WIC factory is STA-bound;
// see host_win32.h for the threading contract.
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

// Process-wide font for native EDIT overlays — sized to FontRole::Body so
// text input fields render at the same size as message body text.
// On systems without "Segoe UI Variable Text" (pre-Win11) GDI silently
// substitutes "Segoe UI" at the same size, which is visually identical.
HFONT body_font()
{
    static HFONT cached = []() -> HFONT
    {
        const int pt =
            tk::font_role_pt(tk::FontRole::Body, tk::d2d::win32_system_base_pt());
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
// Used by BetterTextArea for replace_range().
// Returns the number of UTF-16 code units in the first `byte_offset` bytes
// of `utf8`; clamped to [0, utf8.size()].
static int utf8_byte_to_utf16_len(const std::string& utf8, int byte_offset)
{
    byte_offset = std::clamp(byte_offset, 0, (int)utf8.size());
    int wlen =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), byte_offset, nullptr, 0);
    return wlen < 0 ? 0 : wlen;
}

// ── WASAPI device enumeration helper ─────────────────────────────────────
//
// Enumerates active endpoints for the given data-flow direction (eCapture for
// microphones, eRender for speakers). Used by Host::enumerate_audio_inputs()
// and Host::enumerate_audio_outputs().

std::vector<tk::DeviceListing> enumerate_wasapi_endpoints(EDataFlow flow)
{
    std::vector<tk::DeviceListing> result;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return result;

    IMMDeviceCollection* collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                              &collection)))
    {
        enumerator->Release();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device)))
            continue;

        LPWSTR id_raw = nullptr;
        device->GetId(&id_raw);

        IPropertyStore* props = nullptr;
        device->OpenPropertyStore(STGM_READ, &props);
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::wstring friendly;
        if (props &&
            SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR)
        {
            friendly = pv.pwszVal;
        }
        PropVariantClear(&pv);
        if (props) props->Release();

        if (id_raw)
        {
            tk::DeviceListing entry;
            entry.id = wide_to_utf8(id_raw);
            CoTaskMemFree(id_raw);
            entry.display_name = friendly.empty() ? entry.id
                                                   : wide_to_utf8(friendly);
            result.push_back(std::move(entry));
        }
        device->Release();
    }
    collection->Release();
    enumerator->Release();
    return result;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Win32TextAreaBase — internal base stored in Host::areas_by_id_
// ─────────────────────────────────────────────────────────────────────────
//
// Both BetterTextField and BetterTextArea below inherit from this so the
// Host can store either in areas_by_id_ and call notify_changed() uniformly.

class Win32TextAreaBase
{
public:
    virtual ~Win32TextAreaBase()
    {
        // Erase this control's areas_by_id_ entry before it becomes a
        // dangling pointer. `id` is captured by value when
        // Host::make_text_field()/make_text_area() calls
        // set_on_destroyed() right after construction — NOT derived here
        // via ctrl_id(), since calling a (pure) virtual from a base-class
        // destructor after the most-derived part has already unwound is
        // undefined behavior.
        if (on_destroyed_)
            on_destroyed_();
    }
    virtual void notify_changed() = 0;
    virtual int  ctrl_id() const = 0;
    virtual HWND hwnd()    const = 0;
    virtual void on_theme_changed(const Theme& /*t*/) {}

    void set_on_destroyed(std::function<void()> cb)
    {
        on_destroyed_ = std::move(cb);
    }

    // Wired by Host::make_text_field()/make_text_area() to the enclosing
    // Host's set_cursor() — see Cursor::IBeam's doc comment (host_win32.h)
    // and NativeTextField::set_hovering() (host.h) for the full rationale.
    // Stored here (not duplicated per subclass) since both
    // BetterTextField::set_hovering() and BetterTextArea::set_hovering()
    // need it identically.
    void set_on_hover_changed(std::function<void(bool)> cb)
    {
        on_hover_changed_ = std::move(cb);
    }

protected:
    std::function<void(bool)> on_hover_changed_;

private:
    std::function<void()> on_destroyed_;
};

// ─────────────────────────────────────────────────────────────────────────
//  BetterTextField / BetterTextArea — BetterText-backed NativeTextField /
//  NativeTextArea (see third_party/bettertext)
// ─────────────────────────────────────────────────────────────────────────
//
// Both wrap a BETTERTEXT_CLASS_NAME child HWND. BetterText owns its own D3D11
// device + DXGI swap chain per HWND and renders itself entirely through
// BetterTextXxx() calls. The control posts nothing to its parent on its own
// (no WM_COMMAND/WM_NOTIFY); text-changed and Enter notifications arrive
// through a per-control BetterTextSetNotifyCallback.

namespace
{

std::uint32_t bt_rgba(tk::Color c)
{
    return (static_cast<std::uint32_t>(c.r) << 24) |
           (static_cast<std::uint32_t>(c.g) << 16) |
           (static_cast<std::uint32_t>(c.b) << 8) |
           static_cast<std::uint32_t>(c.a);
}

BetterTextTheme bt_theme_from_palette(const tk::Palette& p, tk::Color background)
{
    BetterTextTheme theme{};
    theme.background_rgba  = bt_rgba(background);
    theme.foreground_rgba  = bt_rgba(p.text_primary);
    theme.selection_rgba   = bt_rgba(p.selection);
    theme.caret_rgba       = bt_rgba(p.text_primary);
    theme.placeholder_rgba = bt_rgba(p.text_muted);
    return theme;
}

void bt_apply_default_font(HWND hwnd)
{
    BetterTextTextStyle style{};
    style.font_family = L"Segoe UI Variable Text";
    style.font_size = static_cast<float>(
        tk::font_role_pt(tk::FontRole::Body, tk::d2d::win32_system_base_pt())) *
        (96.f / 72.f);
    style.font_weight = FW_REGULAR;
    style.italic = FALSE;
    style.underline = FALSE;
    BetterTextSetDefaultTextStyle(hwnd, &style);
}

void bt_register_control_once()
{
    static bool registered = BetterTextRegisterControl(
        reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr))) != FALSE;
    (void)registered;
}

// Inline size for a custom-emoji image run in the compose box — roughly one
// line height, matching how a Unicode emoji glyph sits inline with body text.
constexpr float kInlineEmoticonSizeDip = 20.0f;

// URI prefix used to distinguish synthetic mention-pill image runs from real
// mxc:// emoticon URIs when enumerating BetterText's image runs (see
// BetterTextArea::composer_draft / mention_runs_). Never resolved as media.
constexpr wchar_t kMentionUriPrefix[] = L"tesseract-mention:";

// Padding (DIPs) around the pill text and how much taller than the text
// layout the chip is — mirrors host_qt.cpp's render_pill() so the composer
// mention chip reads the same across platforms.
constexpr float kMentionPillPadX = 8.0f;
constexpr float kMentionPillPadY = 2.0f;

struct MentionPillBitmap
{
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    float width_dip = 0.f;
    float height_dip = 0.f;
};

// Renders a rounded-rect chip with centered text into an offscreen WIC
// bitmap, using a WIC-backed D2D render target plus the same
// tk::Canvas/CanvasFactory abstraction (fill_rounded_rect/draw_text/
// build_text) the rest of the app paints with — the D2D analogue of
// host_qt.cpp's QPainter-based render_pill(). `dpi_scale` oversamples the
// bitmap so the chip stays crisp on HiDPI displays (bitmap pixels = DIPs *
// dpi_scale); BetterTextInsertImageUri still wants the logical DIP size.
MentionPillBitmap render_mention_pill(const std::string& text, Color bg, Color fg,
                                      float dpi_scale)
{
    using Microsoft::WRL::ComPtr;
    MentionPillBitmap out;

    auto factory = d2d::make_factory(backend_singleton());
    if (!factory)
    {
        return out;
    }
    TextStyle style;
    style.role = FontRole::Body;
    std::unique_ptr<TextLayout> layout = factory->build_text(text, style);
    if (!layout)
    {
        return out;
    }
    const Size sz = layout->measure();

    const float w_dip = std::ceil(sz.w) + kMentionPillPadX * 2.f;
    const float h_dip = std::ceil(sz.h) + kMentionPillPadY * 2.f;
    const float radius = h_dip * 0.5f;

    const UINT pw = static_cast<UINT>(std::max(1.f, std::round(w_dip * dpi_scale)));
    const UINT ph = static_cast<UINT>(std::max(1.f, std::round(h_dip * dpi_scale)));

    auto fac = d2d::factories(backend_singleton());
    if (!fac.wic || !fac.d2d)
    {
        return out;
    }

    ComPtr<IWICBitmap> bmp;
    if (FAILED(fac.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapCacheOnDemand, bmp.GetAddressOf())))
    {
        return out;
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f * dpi_scale, 96.f * dpi_scale);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(fac.d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, rt.GetAddressOf())))
    {
        return out;
    }

    std::unique_ptr<Canvas> canvas = d2d::make_canvas(backend_singleton(), rt.Get());
    rt->BeginDraw();
    canvas->clear(Color::rgba(0, 0, 0, 0));
    canvas->fill_rounded_rect({0.f, 0.f, w_dip, h_dip}, radius, bg);
    canvas->draw_text(*layout, {(w_dip - sz.w) * 0.5f, (h_dip - sz.h) * 0.5f}, fg);
    if (FAILED(rt->EndDraw()))
    {
        return out;
    }

    out.bitmap     = bmp;
    out.width_dip  = w_dip;
    out.height_dip = h_dip;
    return out;
}

// Routes BetterText's emoji glyph fallback to the same bundled Noto Color
// Emoji font (and collection) the rest of the app already uses via
// d2d::Backend::build_emoji_fallback — instead of BetterText's own default
// (whatever the OS resolves for "Segoe UI Emoji"), which would otherwise
// look visually inconsistent with emoji everywhere else in the app. Both
// Tesseract's D2D backend and BetterText's own EnsureFactories() request
// DWRITE_FACTORY_TYPE_SHARED, so a collection built against one factory
// reference is valid to hand to layouts built against the other — no
// cross-factory copy needed. Stateless; one process-wide instance suffices.
class BetterTextNotoFontProvider final : public IBetterTextFontProvider
{
public:
    HRESULT CreateFontCollection(IDWriteFactory*, IDWriteFontCollection** collection) override
    {
        auto fac = d2d::factories(backend_singleton());
        if (!fac.noto_emoji_collection)
        {
            *collection = nullptr;
            return E_FAIL;
        }
        // EnsureEmojiFontCollection (BetterTextControl.cpp) Attach()es the
        // returned pointer, taking ownership of exactly one reference.
        fac.noto_emoji_collection->AddRef();
        *collection = fac.noto_emoji_collection;
        return S_OK;
    }

    const wchar_t* EmojiFallbackFamily() const override
    {
        return L"Noto Color Emoji";
    }
};

BetterTextNotoFontProvider& bt_noto_font_provider()
{
    static BetterTextNotoFontProvider instance;
    return instance;
}

} // namespace

class BetterTextField : public NativeTextField, public Win32TextAreaBase
{
public:
    BetterTextField(HWND parent, int ctrl_id, const Theme* theme)
        : parent_(parent), id_(ctrl_id), theme_(theme)
    {
        bt_register_control_once();
        hwnd_ = CreateWindowExW(
            // Canvas-drawn-text spike (see NativeTextField::rendered_image()'s
            // doc comment in host.h). An earlier attempt used WS_EX_LAYERED +
            // SetLayeredWindowAttributes to hide hwnd_ while keeping it real/
            // focusable/hit-tested — reverted, it broke BetterText entirely
            // on real hardware. Root cause, confirmed by reading BetterText's
            // own source (third_party/bettertext/src/BetterTextControl.cpp,
            // CreateSwapChain()): it presents through a flip-model DXGI swap
            // chain (DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) bound directly to
            // hwnd_ via CreateSwapChainForHwnd — a combination Microsoft
            // documents as incompatible with layered-window presentation.
            // Confirmed by checking how Qt's own Windows QPA backend
            // (qwindowswindow.cpp, setWindowLayered/setWindowOpacity) handles
            // this: it only ever applies WS_EX_LAYERED to top-level windows,
            // never to a child HWND owning its own flip-model swap chain like
            // this one.
            0, BETTERTEXT_CLASS_NAME, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 100, 24, parent_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
            nullptr);
        if (!hwnd_)
        {
            return;
        }
        // SetWindowRgn is a different mechanism from WS_EX_LAYERED — it only
        // clips what DWM composites from hwnd_'s already-rendered surface.
        // An empty region means hwnd_ paints nothing on screen
        // (tk::TextField::paint() draws rendered_image() instead — see
        // refresh_image() below, which reads pixels straight off
        // BetterText's offscreen render target via
        // BetterTextRequestCaptureBGRA()/BetterTextReadCaptureBGRA(),
        // independent of on-screen visibility).
        // The region does stop normal OS mouse hit-testing though, unlike
        // the old real-overlay behavior — see forward_pointer_down/drag/up
        // below for the SendMessage-based synthetic-click replacement.
        // SetWindowRgn takes ownership of the HRGN; do not delete it.
        SetWindowRgn(hwnd_, CreateRectRgn(0, 0, 0, 0), TRUE);
        // hwnd_ never actually reaches the screen (see above) — this also
        // switches the render target itself to an offscreen texture with no
        // swap chain at all (see BetterTextSetPresentEnabled's doc comment),
        // since on real hardware a flip-model swap chain's Present() can
        // still visibly flicker the whole top-level window even with an
        // empty region. Capture via
        // BetterTextRequestCaptureBGRA/BetterTextReadCaptureBGRA is
        // unaffected either way.
        BetterTextSetPresentEnabled(hwnd_, FALSE);
        BetterTextSetSingleLine(hwnd_, TRUE);
        bt_apply_default_font(hwnd_);
        BetterTextSetFontProvider(hwnd_, &bt_noto_font_provider());
        if (theme)
        {
            // Transparent background: BetterText's own D2D render target now
            // supports real per-pixel alpha (see CreateTargetBitmap in
            // third_party/bettertext and refresh_image()'s opaque=false
            // call below), so instead of painting a flat backdrop that has
            // to be colour-matched to whatever card the caller happens to
            // draw behind this field (the old approach — every
            // NativeTextField call site draws its own card using
            // compose_card_bg, e.g. "Search field card — same style as the
            // compose input" in RoomListView.cpp), the field can just paint
            // nothing and let that card show through directly, matching
            // how Qt/GTK's native controls already behave
            // (background: transparent stylesheet/CSS).
            BetterTextTheme bt = bt_theme_from_palette(
                theme->palette, theme->palette.compose_card_bg.with_alpha(0));
            BetterTextSetTheme(hwnd_, &bt);
        }
        SetWindowSubclass(hwnd_, &BetterTextField::subclass_proc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        BetterTextSetNotifyCallback(hwnd_, &BetterTextField::on_notify, this);
        // Caret rendering is owned by the canvas from here on (see
        // caret_rect()/caret_blink_visible() below) — permanently off so
        // Paint() never bakes a caret into the capture, and the blink timer
        // can toggle canvas-side visibility without ever needing to
        // recapture the whole control.
        BetterTextSetCaretVisible(hwnd_, FALSE);
        // Fields sit in fixed-height compact rows (e.g. the 28-DIP room
        // search card) — BetterText's 8-DIP default vertical padding alone
        // (16 DIP top+bottom) doesn't fit. Keep the 8-DIP horizontal inset
        // (matches the old EDIT's EM_SETMARGINS left/right margin) but shrink
        // vertical to 2 DIP, mirroring the old EDIT's tm.tmHeight + 4 budget.
        // Must happen before measuring line_h_dip_ below, which bakes it in.
        BetterTextSetPadding(hwnd_, 8.0f, 2.0f);
        // Single-line + no-wrap: the natural height never changes with
        // content, so measure it once up front for set_rect's centering math.
        line_h_dip_ = BetterTextGetContentHeight(hwnd_);
    }

    ~BetterTextField() override
    {
        if (hwnd_)
        {
            BetterTextSetNotifyCallback(hwnd_, nullptr, nullptr);
            RemoveWindowSubclass(hwnd_, &BetterTextField::subclass_proc, 1);
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
        // A pure position change (e.g. a scrolling ancestor re-arranging every
        // paint) doesn't alter the control's rendered pixels — only its size
        // does. Skip the expensive refresh_image() GPU readback in that case;
        // TextField::paint() re-reads rendered_image_rect() fresh every call,
        // so the existing cached_image_ lands at the new position for free.
        const bool size_changed = (r.w != last_rect_.w || r.h != last_rect_.h);
        last_rect_ = r;
        const float s = dip_scale();
        int x  = static_cast<int>(std::floor(r.x * s));
        int w  = static_cast<int>(std::round(r.w * s));
        int rh = static_cast<int>(std::round(r.h * s));
        int h = line_h_dip_ > 0.f ? static_cast<int>(std::round(line_h_dip_ * s)) : rh;
        // Never exceed the row the caller actually gave us — mirrors
        // BetterTextArea::set_rect's max_h fallback, so an unexpectedly
        // short row clips gracefully instead of painting over its border.
        h = std::min(h, rh);
        int y = static_cast<int>(std::floor(r.y * s)) + (rh - h) / 2;
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        // Applied rect back in DIPs — see rendered_image_rect().
        applied_rect_ = {static_cast<float>(x) / s, static_cast<float>(y) / s,
                        static_cast<float>(w) / s, static_cast<float>(h) / s};
        if (size_changed)
        {
            refresh_image();
        }
    }
    void set_text(std::string text) override
    {
        if (!hwnd_)
        {
            return;
        }
        // Room switching (MainWindow::on_room_selected) calls set_text("")
        // unconditionally on every switch, even when the box was already
        // empty — skip the expensive refresh_image() GPU readback when the
        // content isn't actually changing. BetterTextGetTextLength/GetText
        // are plain CPU-side queries, not a capture.
        if (text == this->text())
        {
            return;
        }
        suppress_changed_ = true;
        std::wstring w = utf8_to_wide(text);
        BetterTextSetText(hwnd_, w.c_str());
        suppress_changed_ = false;
        refresh_image();
    }
    std::string text() const override
    {
        if (!hwnd_)
        {
            return {};
        }
        int len = BetterTextGetTextLength(hwnd_);
        if (len <= 0)
        {
            return {};
        }
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        BetterTextGetText(hwnd_, w.data(), len + 1);
        return wide_to_utf8(w);
    }
    void set_placeholder(std::string text) override
    {
        if (!hwnd_)
        {
            return;
        }
        BetterTextSetPlaceholder(hwnd_, utf8_to_wide(text).c_str());
    }
    void set_focused(bool focused) override
    {
        if (!hwnd_)
        {
            return;
        }
        if (focused)
        {
            SetFocus(hwnd_);
        }
        else if (parent_)
        {
            // Yield focus back to the surface HWND rather than leaving it on
            // this control — matches the bidirectional behaviour callers
            // (e.g. Host::request_focus() handing focus to a canvas widget)
            // expect from set_focused(false).
            SetFocus(parent_);
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
        BetterTextSetPasswordMode(hwnd_, password ? TRUE : FALSE);
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
    void set_on_focus_changed(std::function<void(bool)> cb) override
    {
        on_focus_changed_ = std::move(cb);
    }
    void set_on_pointer_down(std::function<void()> cb) override
    {
        on_pointer_down_ = std::move(cb);
    }

    const tk::Image* rendered_image() const override
    {
        return cached_image_.get();
    }
    Rect rendered_image_rect() const override
    {
        return applied_rect_;
    }
    void set_on_repaint_needed(std::function<void(Rect)> cb) override
    {
        on_repaint_needed_ = std::move(cb);
    }
    // BetterTextSetCaretVisible(hwnd_, FALSE) in the ctor means Paint()
    // never draws a caret into the capture — the canvas draws its own,
    // driven by caret_rect()/caret_blink_visible() below, so it never has
    // to recapture the whole control just to blink.
    bool caret_owned_by_canvas() const override
    {
        return true;
    }
    bool caret_blink_visible() const override
    {
        return has_focus_ && caret_blink_visible_;
    }
    Rect caret_rect() const override
    {
        if (!hwnd_)
        {
            return {};
        }
        RECT r{};
        if (!BetterTextGetCaretRect(hwnd_, &r))
        {
            return {};
        }
        // BetterTextGetCaretRect's coordinates come straight out of
        // DirectWrite's HitTestTextPosition against a device context that
        // had SetDpi(dpi, dpi) applied (see BetterTextControl.cpp) — i.e.
        // they're already in the same true-DIP space applied_rect_ is in,
        // not hwnd_'s raw client pixels. Dividing by dip_scale() a second
        // time here used to shrink the caret rect by the DPI factor on any
        // non-100%-scaled display, so just offset by it directly.
        return {applied_rect_.x + static_cast<float>(r.left),
                applied_rect_.y + static_cast<float>(r.top),
                static_cast<float>(r.right - r.left),
                static_cast<float>(r.bottom - r.top)};
    }
    // See Cursor::IBeam's doc comment (host_win32.h) — hwnd_'s
    // SetWindowRgn(empty) means the OS never asks it for a cursor via
    // WM_SETCURSOR, so canvas-level hover (tk::TextField::on_pointer_move)
    // requests it explicitly instead, routed through the enclosing Host's
    // sticky-across-WM_SETCURSOR set_cursor() via on_hover_changed_ (wired
    // in make_text_field()).
    void set_hovering(bool hovering) override
    {
        if (on_hover_changed_)
        {
            on_hover_changed_(hovering);
        }
    }

    // ── Win32TextAreaBase — reused purely so this field re-themes on a
    // live light/dark toggle the same way BetterTextArea already does.
    void notify_changed() override
    {
        if (!suppress_changed_ && on_changed_)
        {
            on_changed_(text());
        }
    }
    int ctrl_id() const override { return id_; }
    HWND hwnd() const override { return hwnd_; }
    void on_theme_changed(const Theme& t) override
    {
        theme_ = &t;
        if (hwnd_)
        {
            // See the ctor's BetterTextSetTheme call — same transparent
            // background rationale, kept in sync on every theme change.
            BetterTextTheme bt = bt_theme_from_palette(
                t.palette, t.palette.compose_card_bg.with_alpha(0));
            BetterTextSetTheme(hwnd_, &bt);
        }
    }

private:
    static void on_notify(HWND, int event, void* user_data)
    {
        auto* self = static_cast<BetterTextField*>(user_data);
        if (event == BetterTextEvent_Changed)
        {
            self->refresh_image();
            if (!self->suppress_changed_ && self->on_changed_)
            {
                self->on_changed_(self->text());
            }
        }
        else if (event == BetterTextEvent_Submit)
        {
            if (self->on_submit_)
            {
                self->on_submit_();
            }
        }
    }

    // Captures hwnd_'s current rendering synchronously — reads pixels
    // straight off BetterText's offscreen render target (added to
    // third_party/bettertext for this) — independent of hwnd_'s on-screen
    // visibility, unlike the PrintWindow-based approach this replaced.
    // BetterTextRequestCaptureBGRA repaints (if dirty) and submits a GPU
    // copy; BetterTextReadCaptureBGRA blocks until that copy completes and
    // reads it. This control's render target is a single text field's
    // worth of pixels — a sub-millisecond copy on any hardware — so paying
    // for that blocking wait here, synchronously, is simpler and strictly
    // more correct than the non-blocking-poll-plus-retry-budget design this
    // replaced, which could run out of retries with the capture never
    // having completed (observed as text staying stale for several
    // keystrokes, or indefinitely during a rapid key-repeat flood, since
    // WM_TIMER — which drove those retries — is only delivered once the
    // message queue goes idle).
    void refresh_image()
    {
        if (!hwnd_)
        {
            return;
        }
        int w = 0, h = 0;
        if (!BetterTextRequestCaptureBGRA(hwnd_, &w, &h) || w <= 0 || h <= 0)
        {
            return;
        }
        pending_pixels_.resize(static_cast<std::size_t>(w) * h * 4);
        if (!BetterTextReadCaptureBGRA(hwnd_, pending_pixels_.data(),
                                       static_cast<int>(pending_pixels_.size()), &w, &h))
        {
            return;
        }
        // opaque=false: BetterTextReadCaptureBGRA's alpha byte is real
        // (premultiplied) coverage data whenever BetterText's D2D target
        // landed on D2D1_ALPHA_MODE_PREMULTIPLIED (see CreateTargetBitmap
        // in third_party/bettertext), which is what lets the field's own
        // transparent background (see the ctor's BetterTextSetTheme call)
        // show the canvas content behind it through instead of painting an
        // opaque backdrop. Falls back to forced-opaque bytes automatically
        // if the driver rejected PREMULTIPLIED — safe either way, since
        // alpha=255 straight and alpha=255 premultiplied are numerically
        // identical (RGB unchanged at full opacity).
        cached_image_ = d2d::make_image_from_bgra(backend_singleton(),
                                                   pending_pixels_.data(), w, h,
                                                   /*opaque=*/false);
        if (on_repaint_needed_)
        {
            on_repaint_needed_(applied_rect_);
        }
    }

    // Scoped repaint over just the caret's own rect — see caret_rect()/
    // caret_blink_visible() above and the WM_SETFOCUS/WM_KILLFOCUS/
    // kBlinkTimerId handlers below, none of which need a capture.
    void notify_caret_repaint()
    {
        if (on_repaint_needed_)
        {
            on_repaint_needed_(caret_rect());
        }
    }

    // hwnd_'s SetWindowRgn(empty) (see ctor comment) stops it from receiving
    // real OS mouse messages, so tk::TextField forwards canvas-level clicks
    // here instead — translated into synthetic WM_* messages via
    // SendMessage, which BetterText's own WndProc (subclassed below) handles
    // identically to a real click, including caret placement from the
    // message's lParam-encoded position.
    void forward_pointer_down(Point world) override
    {
        if (!hwnd_)
        {
            return;
        }
        SetFocus(hwnd_);
        POINT p = to_local_px(world);
        // SendMessageW is synchronous — BetterText's WndProc has already
        // updated caret/selection state by the time it returns, so
        // refresh_image() immediately after picks up the new state. Without
        // this, a click or drag-select changes hwnd_'s internal state but
        // nothing tells the canvas to re-capture it — the click/selection
        // silently has no visible effect (only set_rect()/set_text()/the
        // Changed notification/focus/blink-timer paths call refresh_image()
        // otherwise, none of which fire on a bare click or drag).
        SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        // BetterText's own WndProc just stole Win32 mouse capture onto hwnd_
        // via its internal SetCapture (see WM_LBUTTONDOWN in
        // BetterTextControl.cpp), overriding the Surface's own SetCapture
        // from on_pointer_down. Hand it back to the Surface so the real
        // WM_LBUTTONUP is delivered there and reaches Host::dispatch_pointer_up
        // — otherwise dispatch_pointer_up never clears pressed_widget_, and
        // dispatch_pointer_move's drag early-return never ends, permanently
        // suspending hover/cursor updates after the first click.
        if (GetCapture() == hwnd_)
        {
            SetCapture(parent_);
        }
        refresh_image();
    }
    void forward_pointer_drag(Point world) override
    {
        if (!hwnd_)
        {
            return;
        }
        POINT p = to_local_px(world);
        // Still forwarded eagerly, and synchronously — BetterText's WndProc
        // needs every move to keep the drag-selection's live extent correct
        // (see forward_pointer_down's comment on why SendMessageW is
        // synchronous). Only the resulting refresh_image() capture is
        // coalesced below.
        SendMessageW(hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        // A fast drag-select can forward many WM_MOUSEMOVEs per rendered
        // frame; capturing on every single one is wasted work (each capture
        // reflects the very latest selection extent regardless of how many
        // moves preceded it). Coalesce into at most one refresh_image() per
        // short tick — mirrors Qt6/GTK4's request_capture() coalescing (see
        // their doc comments) via a short one-shot timer, since Win32 has no
        // idle-callback/singleShot(0) equivalent readily in scope here.
        if (!drag_refresh_pending_)
        {
            drag_refresh_pending_ = true;
            SetTimer(hwnd_, kDragCoalesceTimerId, 1, nullptr);
        }
    }
    void forward_pointer_up(Point world) override
    {
        if (!hwnd_)
        {
            return;
        }
        POINT p = to_local_px(world);
        SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
        // A pending coalesced drag capture is now stale (the drag just
        // ended) — cancel it and capture the final state directly instead
        // of waiting out the timer.
        if (drag_refresh_pending_)
        {
            KillTimer(hwnd_, kDragCoalesceTimerId);
            drag_refresh_pending_ = false;
        }
        refresh_image();
    }

    static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR /*id*/,
                                          DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<BetterTextField*>(ref);
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
            else if (wParam == VK_TAB)
            {
                nk = (GetKeyState(VK_SHIFT) & 0x8000) ? NavKey::ShiftTab
                                                       : NavKey::Tab;
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
        // TranslateMessage queues the WM_CHAR for VK_TAB *before* the
        // WM_KEYDOWN above is dispatched, so consuming the keydown alone
        // doesn't stop BetterText from inserting a literal tab character.
        // While popup_nav_ is installed (permanently, once a tk::TextField
        // wraps this control), swallow the tab WM_CHAR too — mirrors
        // BetterTextArea's identical guard.
        if (msg == WM_CHAR && self->popup_nav_ && wParam == VK_TAB)
        {
            return 0;
        }
        // See BetterTextArea's identical WM_CHAR handler for the full
        // rationale: BetterText's own HandleChar mutates the document via
        // BetterTextInsertText, whose synchronous NotifyChanged reentrantly
        // triggers on_notify's refresh_image() before HandleChar's own later
        // EnsureCaretVisibleHorizontally call (still further down inside
        // this same DefSubclassProc call) gets to correct scroll_x for the
        // just-typed character. Recapture again here, once HandleChar has
        // fully finished and scroll_x is settled.
        if (msg == WM_CHAR)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            self->refresh_image();
            return r;
        }
        // Arrow/Home/End/Ctrl+A move the caret and/or extend the selection
        // synchronously inside BetterText's own WndProc (HandleKeyDown in
        // BetterTextControl.cpp) before DefSubclassProc returns below — but
        // the caret is canvas-owned (see caret_rect()/caret_blink_visible()
        // above) and the selection highlight lives in the captured bitmap,
        // so nothing else notices either changed. Recapture immediately so
        // both show up right away, instead of waiting for the next blink
        // tick — which only repaints the caret's *current* rect, never the
        // stale one it just moved away from (the visible symptom: multiple
        // caret afterimages left behind while arrowing across a long line).
        if (msg == WM_KEYDOWN &&
            (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP ||
             wParam == VK_DOWN || wParam == VK_HOME || wParam == VK_END ||
             (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))))
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            // Force the caret solid and restart the blink phase — without
            // this, a move that lands mid-"off" blink phase leaves the
            // caret invisible until the next tick, and rapid arrowing would
            // otherwise look like it's blinking while moving.
            self->caret_blink_visible_ = true;
            SetTimer(hwnd, kBlinkTimerId, 530, nullptr);
            self->refresh_image();
            return r;
        }
        if (msg == WM_GETDLGCODE)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            return r | DLGC_WANTALLKEYS;
        }
        if (msg == WM_SETFOCUS)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            self->has_focus_ = true;
            self->caret_blink_visible_ = true;
            // Caret is canvas-owned (see caret_rect()/caret_blink_visible()
            // below) — BetterTextSetCaretVisible/refresh_image() are
            // deliberately NOT called here; a scoped repaint over just the
            // caret's own rect is all that's needed to show it.
            self->notify_caret_repaint();
            // Drives caret blink: BetterText has no internal blink state
            // (unlike a real focused native widget on the other three
            // platforms, whose OS caret blinks on its own) and, since the
            // caret is canvas-owned, each tick only flips
            // caret_blink_visible_ and requests a scoped repaint of the
            // caret's own small rect — no capture, no GPU readback. Runs
            // only while focused, restarted fresh (phase reset to visible)
            // on every WM_SETFOCUS.
            SetTimer(hwnd, kBlinkTimerId, 530, nullptr);
            if (self->on_focus_changed_) self->on_focus_changed_(true);
            return r;
        }
        if (msg == WM_KILLFOCUS)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            self->has_focus_ = false;
            KillTimer(hwnd, kBlinkTimerId);
            // Scoped repaint to erase the caret (caret_blink_visible()
            // returns false once has_focus_ is false — see its doc
            // comment) — no capture needed, the control's own content
            // didn't change.
            self->notify_caret_repaint();
            if (self->on_focus_changed_) self->on_focus_changed_(false);
            return r;
        }
        if (msg == WM_TIMER && wParam == kBlinkTimerId)
        {
            self->caret_blink_visible_ = !self->caret_blink_visible_;
            self->notify_caret_repaint();
            return 0;
        }
        if (msg == WM_TIMER && wParam == kDragCoalesceTimerId)
        {
            KillTimer(hwnd, kDragCoalesceTimerId);
            self->drag_refresh_pending_ = false;
            self->refresh_image();
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            if (self->on_pointer_down_) self->on_pointer_down_();
            return r;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    float dip_scale() const
    {
        const float dpi = static_cast<float>(GetDpiForWindow(parent_));
        return dpi > 0.f ? dpi / 96.f : 1.f;
    }

    // `world` is in the same widget-tree (DIP) coordinate space as
    // set_rect()'s `r`; last_rect_ (also DIPs) is that same `r`, so
    // subtracting it and scaling to pixels maps world back to hwnd_'s own
    // client-local pixel coordinates — independent of hwnd_'s real screen
    // position, which SetWindowRgn(empty) never touches (see the ctor
    // comment), so this stays correct without needing GetWindowRect.
    POINT to_local_px(Point world) const
    {
        const float s = dip_scale();
        return POINT{static_cast<LONG>((world.x - last_rect_.x) * s),
                     static_cast<LONG>((world.y - last_rect_.y) * s)};
    }

    static constexpr UINT_PTR kBlinkTimerId = 0xBE77;
    // One-shot timer coalescing forward_pointer_drag()'s per-WM_MOUSEMOVE
    // refresh_image() calls — see its doc comment.
    static constexpr UINT_PTR kDragCoalesceTimerId = 0xBE79;
    // Current blink phase, flipped on each kBlinkTimerId tick and reset to
    // visible on every WM_SETFOCUS — see the WM_TIMER handler above.
    bool caret_blink_visible_ = true;
    // Guards kDragCoalesceTimerId — see forward_pointer_drag()'s doc comment.
    bool drag_refresh_pending_ = false;
    // Tracks WM_SETFOCUS/WM_KILLFOCUS — see caret_blink_visible() above,
    // which must never report visible while unfocused (Paint() itself would
    // never have drawn a native caret unfocused either, before this).
    bool has_focus_ = false;

    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    int id_ = 0;
    const Theme* theme_ = nullptr;
    float line_h_dip_ = 0.f;
    bool suppress_changed_ = false;
    Rect last_rect_ = {-1.f, -1.f, -1.f, -1.f};
    // Rect actually applied to hwnd_ (in DIPs) by the last set_rect() call —
    // see rendered_image_rect().
    Rect applied_rect_{};
    std::unique_ptr<tk::Image> cached_image_;
    // Reused scratch buffer for refresh_image()'s synchronous capture.
    std::vector<std::uint8_t> pending_pixels_;
    std::function<void(Rect)> on_repaint_needed_;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<bool(NavKey)> popup_nav_;
    std::function<void(bool)> on_focus_changed_;
    std::function<void()> on_pointer_down_;
};

class BetterTextArea : public NativeTextArea, public Win32TextAreaBase
{
public:
    BetterTextArea(HWND parent, int ctrl_id, IWICImagingFactory* wic, const Theme* theme)
        : parent_(parent), id_(ctrl_id), wic_(wic), theme_(theme)
    {
        bt_register_control_once();
        hwnd_ = CreateWindowExW(
            // Canvas-drawn-text spike — see BetterTextField's ctor comment
            // for why WS_EX_LAYERED is unusable here (BetterText's flip-model
            // DXGI swap chain doesn't tolerate a layered HWND) and for
            // SetWindowRgn(empty)'s use below instead.
            0, BETTERTEXT_CLASS_NAME, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 200, 40, parent_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
            nullptr);
        if (!hwnd_)
        {
            return;
        }
        // See BetterTextField's ctor comment — same rationale. SetWindowRgn
        // takes ownership of the HRGN; do not delete it.
        SetWindowRgn(hwnd_, CreateRectRgn(0, 0, 0, 0), TRUE);
        // See BetterTextField's ctor comment — same rationale.
        BetterTextSetPresentEnabled(hwnd_, FALSE);
        BetterTextSetSubmitOnEnter(hwnd_, TRUE);
        bt_apply_default_font(hwnd_);
        BetterTextSetFontProvider(hwnd_, &bt_noto_font_provider());
        if (theme_)
        {
            // See BetterTextField's ctor comment — same transparent-
            // background rationale.
            BetterTextTheme bt = bt_theme_from_palette(
                theme_->palette, theme_->palette.compose_card_bg.with_alpha(0));
            BetterTextSetTheme(hwnd_, &bt);
        }
        SetWindowSubclass(hwnd_, &BetterTextArea::subclass_proc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        BetterTextSetNotifyCallback(hwnd_, &BetterTextArea::on_notify, this);
        BetterTextSetImageProvider(hwnd_, &image_provider_);
        // See BetterTextField's ctor — same canvas-owned-caret rationale.
        BetterTextSetCaretVisible(hwnd_, FALSE);
    }

    ~BetterTextArea() override
    {
        if (hwnd_)
        {
            BetterTextSetNotifyCallback(hwnd_, nullptr, nullptr);
            RemoveWindowSubclass(hwnd_, &BetterTextArea::subclass_proc, 1);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    // ── NativeTextArea ────────────────────────────────────────────────────

    void set_rect(Rect r) override
    {
        if (!hwnd_)
        {
            return;
        }
        if (r.x == last_rect_.x && r.y == last_rect_.y &&
            r.w == last_rect_.w && r.h == last_rect_.h)
        {
            return;
        }
        // See BetterTextField::set_rect's comment — a pure position change
        // doesn't alter the rendered pixels, so skip refresh_image()'s GPU
        // readback unless the size actually changed.
        const bool size_changed = (r.w != last_rect_.w || r.h != last_rect_.h);
        last_rect_ = r;
        const float s  = dip_scale();
        const int rh   = static_cast<int>(std::round(r.h * s));
        const int nh   = static_cast<int>(std::round(natural_height() * s));
        // Never exceed the row the caller actually gave us — mirrors
        // BetterTextField::set_rect's identical fallback. No extra border
        // inset here (there used to be one): ComposeBar already sizes r.h to
        // exactly match natural_height() in the common case, so shrinking
        // rh any further before comparing against nh made nh<=max_h false
        // even for a perfectly-fitting box, permanently under-sizing the
        // control by a few px — enough to make DrawScrollbarThumb think a
        // single line of text overflows, and to flicker as sub-pixel
        // rounding tipped that razor-thin margin frame to frame.
        const int max_h = std::max(1, rh);
        const int h    = (nh > 0 && nh <= max_h) ? nh : max_h;
        const int y    = static_cast<int>(std::floor(r.y * s)) + (rh - h) / 2;
        const int x    = static_cast<int>(std::floor(r.x * s));
        const int w    = static_cast<int>(std::round(r.w * s));
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        // Applied rect back in DIPs — see rendered_image_rect().
        applied_rect_ = {static_cast<float>(x) / s, static_cast<float>(y) / s,
                        static_cast<float>(w) / s, static_cast<float>(h) / s};
        if (size_changed)
        {
            refresh_image();
        }
    }

    void set_text(std::string text) override
    {
        if (!hwnd_)
        {
            return;
        }
        // See BetterTextField::set_text's comment — room switching clears
        // the composer unconditionally on every switch even when it's
        // already empty; skip the expensive refresh_image() GPU readback
        // (and the rest of the reset work) when content isn't changing.
        if (text == this->text())
        {
            return;
        }
        suppress_changed_ = true;
        std::wstring w = utf8_to_wide(text);
        BetterTextSetText(hwnd_, w.c_str());
        suppress_changed_ = false;
        mention_runs_.clear();
        refresh_height();
        refresh_image();
    }

    std::string text() const override
    {
        if (!hwnd_)
        {
            return {};
        }
        int len = BetterTextGetTextLength(hwnd_);
        if (len <= 0)
        {
            return {};
        }
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        BetterTextGetText(hwnd_, w.data(), len + 1);
        return wide_to_utf8(w);
    }

    void set_placeholder(std::string text) override
    {
        if (hwnd_)
        {
            BetterTextSetPlaceholder(hwnd_, utf8_to_wide(text).c_str());
            // A placeholder can now wrap to multiple lines while the
            // document is empty (see LayoutHeight()'s placeholder branch),
            // so re-report natural_height() the same way set_text() does.
            refresh_height();
        }
    }

    void set_focused(bool focused) override
    {
        if (!hwnd_)
        {
            return;
        }
        if (focused)
        {
            SetFocus(hwnd_);
        }
        else if (parent_)
        {
            // Yield focus back to the surface HWND — see BetterTextField's
            // identical set_focused(false) branch for why this matters.
            SetFocus(parent_);
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
    bool visible() const override { return visible_; }

    void set_enabled(bool enabled) override
    {
        if (hwnd_)
        {
            EnableWindow(hwnd_, enabled ? TRUE : FALSE);
        }
    }

    float natural_height() const override
    {
        return hwnd_ ? BetterTextGetContentHeight(hwnd_) : 0.f;
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
    void set_image_resolver(std::function<const tk::Image*(const std::string&)> fn) override
    {
        image_resolver_ = std::move(fn);
    }

    void insert_at_cursor(std::string text) override
    {
        if (!hwnd_)
        {
            return;
        }
        std::wstring w = utf8_to_wide(text);
        BetterTextInsertText(hwnd_, w.c_str());
    }

    tk::Rect cursor_rect() const override
    {
        if (!hwnd_)
        {
            return {};
        }
        RECT r{};
        if (!BetterTextGetCaretRect(hwnd_, &r))
        {
            return {};
        }
        POINT tl{r.left, r.top};
        POINT br{r.right, r.bottom};
        MapWindowPoints(hwnd_, GetParent(hwnd_), &tl, 1);
        MapWindowPoints(hwnd_, GetParent(hwnd_), &br, 1);
        return {static_cast<float>(tl.x), static_cast<float>(tl.y),
                static_cast<float>(br.x - tl.x), static_cast<float>(br.y - tl.y)};
    }

    void replace_range(int start, int end, std::string utf8_text) override
    {
        if (!hwnd_)
        {
            return;
        }
        std::string cur = text();
        int ws = utf8_byte_to_utf16_len(cur, start);
        int we = utf8_byte_to_utf16_len(cur, end);
        suppress_changed_ = true;
        BetterTextSetSelection(hwnd_, ws, we);
        std::wstring wide = utf8_to_wide(utf8_text);
        BetterTextInsertText(hwnd_, wide.c_str());
        suppress_changed_ = false;
        // The insert above runs under suppress_changed_, so on_notify()
        // never gets a chance to resize any emoji this inserted (e.g. the
        // shortcode popup replacing ":wave:" with 👋) — do it explicitly
        // here, before measuring height, instead of waiting for the next
        // keystroke. Likewise on_notify() never gets a chance to capture the
        // updated content into cached_image_/request a repaint — see
        // BetterTextArea::set_text's identical suppress_changed_ pattern —
        // so refresh_image() must be called explicitly here too, or the
        // glyph stays invisible until some later edit happens to trigger it.
        reformat_emoji_runs();
        if (on_changed_)
        {
            on_changed_(text());
        }
        refresh_height();
        refresh_image();
    }

    void set_on_popup_nav(std::function<bool(NavKey)> fn) override
    {
        popup_nav_ = std::move(fn);
    }
    void set_on_focus_changed(std::function<void(bool)> cb) override
    {
        on_focus_changed_ = std::move(cb);
    }
    void set_on_pointer_down(std::function<void()> cb) override
    {
        on_pointer_down_ = std::move(cb);
    }
    void set_on_edit_last(std::function<bool()> fn) override
    {
        on_edit_last_ = std::move(fn);
    }

    const tk::Image* rendered_image() const override
    {
        return cached_image_.get();
    }
    Rect rendered_image_rect() const override
    {
        return applied_rect_;
    }
    void set_on_repaint_needed(std::function<void(Rect)> cb) override
    {
        on_repaint_needed_ = std::move(cb);
    }
    // See BetterTextField::caret_owned_by_canvas/caret_blink_visible/
    // caret_rect — same rationale and coordinate math, mirrored here for
    // the multi-line control (BetterTextGetCaretRect is the same API for
    // both single- and multi-line BetterText controls).
    bool caret_owned_by_canvas() const override
    {
        return true;
    }
    bool caret_blink_visible() const override
    {
        return has_focus_ && caret_blink_visible_;
    }
    Rect caret_rect() const override
    {
        if (!hwnd_)
        {
            return {};
        }
        RECT r{};
        if (!BetterTextGetCaretRect(hwnd_, &r))
        {
            return {};
        }
        // See BetterTextField::caret_rect's comment — already true-DIP
        // coordinates, not hwnd_'s raw client pixels, so no /dip_scale()
        // here either.
        return {applied_rect_.x + static_cast<float>(r.left),
                applied_rect_.y + static_cast<float>(r.top),
                static_cast<float>(r.right - r.left),
                static_cast<float>(r.bottom - r.top)};
    }
    // Scoped repaint over just the caret's own rect — see
    // BetterTextField::notify_caret_repaint.
    void notify_caret_repaint()
    {
        if (on_repaint_needed_)
        {
            on_repaint_needed_(caret_rect());
        }
    }
    // See BetterTextField::set_hovering — same rationale.
    void set_hovering(bool hovering) override
    {
        if (on_hover_changed_)
        {
            on_hover_changed_(hovering);
        }
    }

    int cursor_byte_pos() const override
    {
        if (!hwnd_)
        {
            return 0;
        }
        BetterTextSelection sel{};
        BetterTextGetSelection(hwnd_, &sel);
        std::string t = text();
        std::wstring w = utf8_to_wide(t);
        int caret = static_cast<int>(
            std::min<int64_t>(static_cast<int64_t>(w.size()), sel.caret));
        return WideCharToMultiByte(CP_UTF8, 0, w.c_str(), caret,
                                   nullptr, 0, nullptr, nullptr);
    }

    // Real inline image run — same mechanism as insert_emoticon below, but the
    // bitmap is rendered synchronously right here (a rounded-rect chip with
    // centered text, via render_mention_pill()) rather than resolved from a
    // media uri, since a mention pill has no mxc:// content to fetch. The
    // rendered bitmap is cached in mention_runs_ keyed by a synthetic
    // "tesseract-mention:<n>" uri so resolve_image_uri() can hand it back
    // when BetterText's image provider asks for it, and so composer_draft()
    // can recover user_id/display_name/is_room for the run without needing
    // to parse anything back out of the rendered pixels.
    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool is_room) override
    {
        if (!hwnd_)
        {
            return;
        }
        const std::string visual = is_room ? "@room" : ("@" + display_name);

        MentionPillBitmap pill =
            render_mention_pill(visual, mention_bg_, mention_fg_, dip_scale());
        if (!pill.bitmap)
        {
            // D2D/WIC failure — fall back to plain text so the mention is
            // never silently dropped (mirrors insert_emoticon's !image path).
            replace_range(start, end, visual + " ");
            return;
        }

        std::wstring uri = kMentionUriPrefix + std::to_wstring(mention_counter_++);
        mention_runs_[uri] = MentionRun{pill.bitmap, user_id, display_name, is_room};

        std::string cur = text();
        int ws = utf8_byte_to_utf16_len(cur, start);
        int we = utf8_byte_to_utf16_len(cur, end);
        suppress_changed_ = true;
        BetterTextSetSelection(hwnd_, ws, we);
        BetterTextInsertImageUri(hwnd_, uri.c_str(), utf8_to_wide(display_name).c_str(),
                                 pill.width_dip, pill.height_dip);
        suppress_changed_ = false;
        if (on_changed_)
        {
            on_changed_(text());
        }
        refresh_height();
        // As in replace_range(): on_notify() never gets a chance to capture
        // the updated content into cached_image_ while suppress_changed_ was
        // set, so refresh_image() must be called explicitly here too.
        refresh_image();
    }

    // Real inline image run (unlike Win32RichEditArea's plain-text fallback —
    // BetterText has no OLE-embedding conflict with the D2D swap chain, so
    // this renders an actual bitmap once set_image_resolver's callback
    // resolves `mxc_url`; see BetterTextInsertImageUri / resolve_image_uri).
    // `image` unused — resolution happens by uri, not by a caller-supplied
    // bitmap (kept in the signature to match the shared NativeTextArea
    // interface every platform's insertion implements).
    void insert_emoticon(int start, int end, const std::string& shortcode,
                         const std::string& mxc_url,
                         const tk::Image*) override
    {
        if (!hwnd_)
        {
            return;
        }
        std::string cur = text();
        int ws = utf8_byte_to_utf16_len(cur, start);
        int we = utf8_byte_to_utf16_len(cur, end);
        suppress_changed_ = true;
        BetterTextSetSelection(hwnd_, ws, we);
        BetterTextInsertImageUri(hwnd_, utf8_to_wide(mxc_url).c_str(), utf8_to_wide(shortcode).c_str(),
                                 kInlineEmoticonSizeDip, kInlineEmoticonSizeDip);
        suppress_changed_ = false;
        if (on_changed_)
        {
            on_changed_(text());
        }
        refresh_height();
        // See insert_mention()'s comment: refresh_image() must be called
        // explicitly here too, since suppress_changed_ blocked on_notify().
        refresh_image();
    }

    std::vector<tesseract::MentionSeg> composer_draft() const override
    {
        std::string t = text();

        struct Special
        {
            std::size_t byte_start;
            std::size_t byte_len;
            tesseract::MentionSeg seg;
        };
        std::vector<Special> specials;

        // Mentions and custom-emoji are both real BetterText Image atoms now
        // (see insert_mention/insert_emoticon above), each rendered as one
        // U+FFFC (EF BF BC in UTF-8) placeholder in text().
        // BetterTextGetImageRunCount/Uri/AltText enumerate them in document
        // order, which matches the order their placeholders appear in `t` —
        // so the i-th run is always the i-th remaining FFFC occurrence. A
        // run's uri distinguishes the two kinds: mention_runs_ holds the ones
        // insert_mention created (keyed by its synthetic uri); anything else
        // is a real mxc:// emoticon uri.
        constexpr char kObjectReplacementUtf8[] = "\xEF\xBF\xBC";
        std::size_t image_pos = 0;
        const int image_count = BetterTextGetImageRunCount(hwnd_);
        for (int i = 0; i < image_count; ++i)
        {
            std::size_t at = t.find(kObjectReplacementUtf8, image_pos);
            if (at == std::string::npos)
            {
                break; // shouldn't happen — defensive
            }
            const int uri_len = BetterTextGetImageRunUriLength(hwnd_, i);
            std::wstring wuri(static_cast<std::size_t>(uri_len), L'\0');
            BetterTextGetImageRunUri(hwnd_, i, wuri.data(), uri_len + 1);

            tesseract::MentionSeg seg;
            if (auto mit = mention_runs_.find(wuri); mit != mention_runs_.end())
            {
                seg.kind         = tesseract::MentionSeg::Kind::Mention;
                seg.user_id      = mit->second.user_id;
                seg.display_name = mit->second.display_name;
                seg.is_room      = mit->second.is_room;
            }
            else
            {
                const int alt_len = BetterTextGetImageRunAltTextLength(hwnd_, i);
                std::wstring walt(static_cast<std::size_t>(alt_len), L'\0');
                BetterTextGetImageRunAltText(hwnd_, i, walt.data(), alt_len + 1);
                seg.kind      = tesseract::MentionSeg::Kind::Emoticon;
                seg.shortcode = wide_to_utf8(walt);
                seg.mxc_url   = wide_to_utf8(wuri);
            }
            specials.push_back({at, sizeof(kObjectReplacementUtf8) - 1, std::move(seg)});
            image_pos = at + (sizeof(kObjectReplacementUtf8) - 1);
        }

        std::sort(specials.begin(), specials.end(),
                  [](const Special& a, const Special& b)
                  { return a.byte_start < b.byte_start; });

        std::vector<tesseract::MentionSeg> segs;
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
        std::size_t prev_end = 0;
        for (const auto& sp : specials)
        {
            if (sp.byte_start < prev_end || sp.byte_start > t.size())
            {
                continue; // overlap/out-of-range — ignore defensively
            }
            push_text(t.substr(prev_end, sp.byte_start - prev_end));
            segs.push_back(sp.seg);
            prev_end = sp.byte_start + sp.byte_len;
        }
        push_text(t.substr(std::min(prev_end, t.size())));
        return segs;
    }

    void set_mention_colors(Color bg, Color fg) override
    {
        mention_bg_ = bg;
        mention_fg_ = fg;
    }

    // ── Win32TextAreaBase ─────────────────────────────────────────────────

    void notify_changed() override
    {
        if (!suppress_changed_ && on_changed_)
        {
            on_changed_(text());
        }
    }
    int ctrl_id() const override { return id_; }
    HWND hwnd() const override { return hwnd_; }
    void on_theme_changed(const Theme& t) override
    {
        theme_ = &t;
        if (hwnd_)
        {
            // See BetterTextField's ctor comment — same transparent-
            // background rationale, kept in sync on every theme change.
            BetterTextTheme bt = bt_theme_from_palette(
                t.palette, t.palette.compose_card_bg.with_alpha(0));
            BetterTextSetTheme(hwnd_, &bt);
        }
    }

private:
    void refresh_height()
    {
        float h = natural_height();
        if (h != last_height_ && on_height_changed_)
        {
            last_height_ = h;
            on_height_changed_(h);
        }
    }

    // Re-scan the text and reapply FontRole::InlineEmoji sizing to exactly
    // the current emoji runs, resetting the whole range to the default style
    // first — full reset-and-reapply on every change avoids tracking stale
    // ranges across undo/paste/IME edits. Skips BetterTextSetTextStyle
    // entirely when there is no emoji now and none last time, so a message
    // with no emoji (the common case) doesn't push an undo entry every
    // keystroke. `base` is read fresh from BetterTextGetDefaultTextStyle, so
    // its foreground_rgba always matches the live default style and applying
    // it back never clobbers the theme-driven text colour (BetterText only
    // overrides colour for a run when that run's foreground_rgba differs
    // from the default's).
    //
    // Called only from on_notify()'s BetterTextEvent_Changed branch, inside
    // the `!suppress_changed_` guard — this function's own suppress_changed_
    // = true wrapping the BetterTextSetTextStyle calls below makes those
    // calls' own re-entrant Changed events land back in on_notify() with
    // suppress_changed_ already true, so they skip that guard (and thus
    // never re-enter reformat_emoji_runs()) without needing a second flag.
    //
    // OPEN RISK, needs manual verification: BetterText's header says style
    // changes participate in undo/redo. Confirmed by reading
    // third_party/bettertext/src/BetterText.cpp that every
    // BetterTextSetTextStyle call with length > 0 unconditionally calls
    // state->PushUndo()/ClearRedo() — i.e. every reformat pass that touches
    // any range pushes its own undo entry(ies) in addition to the character
    // edit's own. Needs manual on-device verification: type a message with
    // emoji, then Ctrl+Z repeatedly, and confirm each press removes exactly
    // one expected unit with no extra steps or visual glitches.
    void reformat_emoji_runs()
    {
        if (!hwnd_)
        {
            return;
        }
        std::string t = text();
        auto ranges = tesseract::views::find_emoji_byte_ranges(t);
        if (ranges.empty() && !had_emoji_runs_)
        {
            return; // nothing to apply, nothing stale to clear
        }

        BetterTextTextStyle base{};
        if (!BetterTextGetDefaultTextStyle(hwnd_, &base))
        {
            return;
        }
        std::wstring wide = utf8_to_wide(t);
        const int64_t total_len = static_cast<int64_t>(wide.size());

        suppress_changed_ = true;
        BetterTextSetTextStyle(hwnd_, 0, total_len, &base);

        if (!ranges.empty())
        {
            BetterTextTextStyle emoji = base;
            emoji.font_size = static_cast<float>(tk::font_role_pt(
                                   tk::FontRole::InlineEmoji,
                                   tk::d2d::win32_system_base_pt())) *
                              (96.f / 72.f);
            for (const auto& r : ranges)
            {
                int ws = utf8_byte_to_utf16_len(t, static_cast<int>(r.start_byte));
                int we = utf8_byte_to_utf16_len(t, static_cast<int>(r.end_byte));
                BetterTextSetTextStyle(hwnd_, ws, we - ws, &emoji);
            }
        }
        suppress_changed_ = false;
        had_emoji_runs_ = !ranges.empty();
    }

    static void on_notify(HWND, int event, void* user_data)
    {
        auto* self = static_cast<BetterTextArea*>(user_data);
        if (event == BetterTextEvent_Changed)
        {
            if (!self->suppress_changed_)
            {
                // Resize emoji runs BEFORE measuring height — refresh_height()
                // must see the corrected font sizes, or the reported height
                // lags one edit behind and the composer visibly jumps.
                self->reformat_emoji_runs();
                if (self->on_changed_)
                {
                    self->on_changed_(self->text());
                }
                self->refresh_height();
                self->refresh_image();
            }
        }
        else if (event == BetterTextEvent_Submit)
        {
            if (self->on_submit_)
            {
                self->on_submit_();
            }
        }
    }

    static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR /*id*/,
                                          DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<BetterTextArea*>(ref);
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
            auto nav = self->popup_nav_;
            if (is_nav && nav && nav(nk))
            {
                return 0;
            }
        }
        if (msg == WM_KEYDOWN && wParam == VK_UP && self->on_edit_last_ &&
            BetterTextGetTextLength(self->hwnd_) == 0)
        {
            if (self->on_edit_last_())
            {
                return 0;
            }
        }
        // TranslateMessage queues the WM_CHAR for VK_TAB *before* the
        // WM_KEYDOWN above is dispatched, so consuming the keydown alone
        // doesn't stop BetterText from inserting a literal tab character —
        // which would mutate the compose text and dismiss the popup. While
        // the popup nav hook is live (popup open), swallow the tab WM_CHAR.
        if (msg == WM_CHAR && self->popup_nav_ && wParam == VK_TAB)
        {
            return 0;
        }
        // Every other WM_CHAR (ordinary typing, Enter/newline) mutates the
        // document via BetterText's own HandleChar, which calls
        // BetterTextInsertText — and that calls NotifyChanged synchronously,
        // reentrantly triggering on_notify's own refresh_image() BEFORE
        // DefSubclassProc returns here. That capture runs too early: it
        // happens before HandleChar's own later EnsureCaretVisibleVertically
        // call (further down, still inside this same DefSubclassProc call)
        // gets to correct state->scroll_y for the just-inserted character —
        // e.g. Shift+Enter on the last line, which moves the caret onto a
        // new trailing empty line one line further down. The result: the
        // canvas keeps showing the pre-correction frame (scrolled one line
        // too high) until some unrelated later capture happens to catch up
        // — visibly, "press Shift+Enter and the caret looks like it's still
        // on the previous line; type another character and it jumps to
        // where it should have been already." Recapture again here, now
        // that HandleChar has fully finished and scroll_y is settled.
        if (msg == WM_CHAR)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            self->refresh_image();
            return r;
        }
        // Ctrl+V / Shift+Ins with an image on the clipboard → intercept
        // BEFORE BetterText's own WM_KEYDOWN handling. BetterText handles
        // Ctrl+V internally (text-only) and never lets WM_PASTE reach this
        // subclass proc, so the WM_PASTE handler below can't catch it.
        if (msg == WM_KEYDOWN)
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
        // Intercept Ctrl+V / Shift+Insert / right-click "Paste" before
        // BetterText inserts text. If clipboard holds a DIB and we have an
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
        // Custom-emoji images resolve asynchronously (the shell kicks off a
        // fetch as a side effect of set_image_resolver's callback and has no
        // completion hook — same fire-and-forget contract every other
        // ensure_media_image_ caller relies on) — retry any still-pending
        // uris each repaint until the shell's cache has them.
        if (msg == WM_PAINT && !self->pending_image_uris_.empty())
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            std::vector<std::wstring> pending(self->pending_image_uris_.begin(),
                                              self->pending_image_uris_.end());
            for (const auto& uri : pending)
            {
                self->resolve_image_uri(hwnd, uri.c_str());
            }
            return r;
        }
        // See BetterTextField's identical handler for the rationale: caret
        // movement/selection from arrow/Home/End/Ctrl+A happens synchronously
        // inside BetterText's own WndProc but is invisible to everything
        // outside it (canvas-owned caret, bitmap-baked selection highlight),
        // so recapture right away instead of waiting for the next blink tick.
        if (msg == WM_KEYDOWN &&
            (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP ||
             wParam == VK_DOWN || wParam == VK_HOME || wParam == VK_END ||
             (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))))
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            // Force the caret solid and restart the blink phase — without
            // this, a move that lands mid-"off" blink phase leaves the
            // caret invisible until the next tick, and rapid arrowing would
            // otherwise look like it's blinking while moving.
            self->caret_blink_visible_ = true;
            SetTimer(hwnd, kBlinkTimerId, 530, nullptr);
            self->refresh_image();
            return r;
        }
        // WM_MOUSEWHEEL reaches hwnd_ directly via focus routing (it targets
        // the focused window, not hit-testing, so the ctor's empty
        // SetWindowRgn — which only blocks hit-tested input — doesn't stop
        // it). BetterText's own WndProc adjusts its internal scroll_y and
        // calls InvalidateBetterText, but that only dirties the offscreen
        // D2D target; nothing recaptures it into cached_image_ (only
        // BetterTextEvent_Changed/Submit do, via on_notify), so the canvas
        // kept showing the pre-scroll bitmap and scrolling the composer
        // looked like it did nothing.
        if (msg == WM_MOUSEWHEEL)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            self->refresh_image();
            return r;
        }
        if (msg == WM_GETDLGCODE)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            return r | DLGC_WANTALLKEYS;
        }
        if (msg == WM_SETFOCUS)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            self->has_focus_ = true;
            self->caret_blink_visible_ = true;
            // See BetterTextField's identical WM_SETFOCUS handler — caret is
            // canvas-owned, so only a scoped repaint of its own rect is
            // needed, no BetterTextSetCaretVisible/refresh_image() capture.
            self->notify_caret_repaint();
            SetTimer(hwnd, kBlinkTimerId, 530, nullptr);
            if (self->on_focus_changed_) self->on_focus_changed_(true);
            return r;
        }
        if (msg == WM_KILLFOCUS)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            self->has_focus_ = false;
            KillTimer(hwnd, kBlinkTimerId);
            self->notify_caret_repaint();
            if (self->on_focus_changed_) self->on_focus_changed_(false);
            return r;
        }
        if (msg == WM_TIMER && wParam == kBlinkTimerId)
        {
            self->caret_blink_visible_ = !self->caret_blink_visible_;
            self->notify_caret_repaint();
            return 0;
        }
        if (msg == WM_TIMER && wParam == kDragCoalesceTimerId)
        {
            KillTimer(hwnd, kDragCoalesceTimerId);
            self->drag_refresh_pending_ = false;
            self->refresh_image();
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN)
        {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            if (self->on_pointer_down_) self->on_pointer_down_();
            return r;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    float dip_scale() const
    {
        const float dpi = static_cast<float>(GetDpiForWindow(parent_));
        return dpi > 0.f ? dpi / 96.f : 1.f;
    }

    static constexpr UINT_PTR kBlinkTimerId = 0xBE77;
    // See BetterTextField's identical constant for the rationale.
    static constexpr UINT_PTR kDragCoalesceTimerId = 0xBE79;
    // Current blink phase, flipped on each kBlinkTimerId tick and reset to
    // visible on every WM_SETFOCUS — see BetterTextField's identical
    // WM_TIMER/WM_SETFOCUS handlers for the rationale.
    bool caret_blink_visible_ = true;
    // Guards kDragCoalesceTimerId — see forward_pointer_drag()'s doc comment.
    bool drag_refresh_pending_ = false;
    // Tracks WM_SETFOCUS/WM_KILLFOCUS — see caret_blink_visible() above.
    bool has_focus_ = false;

    // Captures synchronously — see BetterTextField::refresh_image for the
    // full rationale (reads pixels straight off BetterText's offscreen
    // render target, independent of on-screen visibility; Request submits
    // the GPU copy, Read blocks until it completes and reads it — this
    // control's render target is a single text field's worth of pixels, a
    // sub-millisecond copy on any hardware).
    void refresh_image()
    {
        if (!hwnd_)
        {
            return;
        }
        int w = 0, h = 0;
        if (!BetterTextRequestCaptureBGRA(hwnd_, &w, &h) || w <= 0 || h <= 0)
        {
            return;
        }
        pending_pixels_.resize(static_cast<std::size_t>(w) * h * 4);
        if (!BetterTextReadCaptureBGRA(hwnd_, pending_pixels_.data(),
                                       static_cast<int>(pending_pixels_.size()), &w, &h))
        {
            return;
        }
        // opaque=false: BetterTextReadCaptureBGRA's alpha byte is real
        // (premultiplied) coverage data whenever BetterText's D2D target
        // landed on D2D1_ALPHA_MODE_PREMULTIPLIED (see CreateTargetBitmap
        // in third_party/bettertext), which is what lets the field's own
        // transparent background (see the ctor's BetterTextSetTheme call)
        // show the canvas content behind it through instead of painting an
        // opaque backdrop. Falls back to forced-opaque bytes automatically
        // if the driver rejected PREMULTIPLIED — safe either way, since
        // alpha=255 straight and alpha=255 premultiplied are numerically
        // identical (RGB unchanged at full opacity).
        cached_image_ = d2d::make_image_from_bgra(backend_singleton(),
                                                   pending_pixels_.data(), w, h,
                                                   /*opaque=*/false);
        if (on_repaint_needed_)
        {
            on_repaint_needed_(applied_rect_);
        }
    }

    // See BetterTextField::forward_pointer_down/to_local_px — same
    // SetWindowRgn(empty)-requires-synthetic-clicks rationale.
    POINT to_local_px(Point world) const
    {
        const float s = dip_scale();
        return POINT{static_cast<LONG>((world.x - last_rect_.x) * s),
                     static_cast<LONG>((world.y - last_rect_.y) * s)};
    }
    void forward_pointer_down(Point world) override
    {
        if (!hwnd_)
        {
            return;
        }
        SetFocus(hwnd_);
        POINT p = to_local_px(world);
        // See BetterTextField::forward_pointer_down — same "SendMessageW is
        // synchronous, so refresh_image() right after picks up the caret/
        // selection state it just updated" rationale.
        SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        // BetterText's own WndProc just stole Win32 mouse capture onto hwnd_
        // via its internal SetCapture (see WM_LBUTTONDOWN in
        // BetterTextControl.cpp), overriding the Surface's own SetCapture
        // from on_pointer_down. Hand it back to the Surface so the real
        // WM_LBUTTONUP is delivered there and reaches Host::dispatch_pointer_up
        // — otherwise dispatch_pointer_up never clears pressed_widget_, and
        // dispatch_pointer_move's drag early-return never ends, permanently
        // suspending hover/cursor updates after the first click.
        if (GetCapture() == hwnd_)
        {
            SetCapture(parent_);
        }
        refresh_image();
    }
    void forward_pointer_drag(Point world) override
    {
        if (!hwnd_)
        {
            return;
        }
        POINT p = to_local_px(world);
        // See BetterTextField::forward_pointer_drag — same "forward eagerly,
        // coalesce the resulting capture" rationale.
        SendMessageW(hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        if (!drag_refresh_pending_)
        {
            drag_refresh_pending_ = true;
            SetTimer(hwnd_, kDragCoalesceTimerId, 1, nullptr);
        }
    }
    void forward_pointer_up(Point world) override
    {
        if (!hwnd_)
        {
            return;
        }
        POINT p = to_local_px(world);
        SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
        if (drag_refresh_pending_)
        {
            KillTimer(hwnd_, kDragCoalesceTimerId);
            drag_refresh_pending_ = false;
        }
        refresh_image();
    }
    // See NativeTextArea::forward_wheel's doc comment in host.h — covers
    // the delivery path forward_pointer_down/drag/up don't: a hit-test-
    // routed WM_MOUSEWHEEL (precision touchpads) never reaches hwnd_ at
    // all, since it's dispatched by cursor position and hwnd_'s window
    // region is empty (see the ctor comment). No pixel conversion needed —
    // `dy` and BetterTextScrollBy's delta are both already the same true
    // (96-baseline) DIP unit BetterText itself uses internally, unlike
    // to_local_px's client-pixel conversion for point-based messages.
    void forward_wheel(Point /*world*/, float dy) override
    {
        if (!hwnd_)
        {
            return;
        }
        BetterTextScrollBy(hwnd_, dy);
        refresh_image();
    }

    // Adapts BetterText's C-style IBetterTextImageProvider to the shell's
    // set_image_resolver callback. Nested so it can reach the outer
    // instance's private resolve_image_uri without a forward declaration.
    class ImageProviderAdapter final : public IBetterTextImageProvider
    {
    public:
        explicit ImageProviderAdapter(BetterTextArea* owner) : owner_(owner) {}
        void ResolveImageUri(HWND control, uint64_t /*request_id*/, const wchar_t* uri,
                             float /*display_width*/, float /*display_height*/) override
        {
            owner_->resolve_image_uri(control, uri);
        }

    private:
        BetterTextArea* owner_;
    };

    void resolve_image_uri(HWND control, const wchar_t* uri)
    {
        if (!uri)
        {
            return;
        }
        if (auto mit = mention_runs_.find(uri); mit != mention_runs_.end())
        {
            BetterTextNotifyImageResolved(control, 0, uri, mit->second.bitmap.Get(), S_OK);
            return;
        }
        if (image_resolver_)
        {
            if (const tk::Image* image = image_resolver_(wide_to_utf8(uri)))
            {
                if (IWICBitmap* bitmap = tk::d2d::to_native_image(*image))
                {
                    BetterTextNotifyImageResolved(control, 0, uri, bitmap, S_OK);
                    pending_image_uris_.erase(uri);
                    return;
                }
            }
        }
        pending_image_uris_.insert(uri);
    }

    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    int id_ = 0;
    IWICImagingFactory* wic_ = nullptr;
    const Theme* theme_ = nullptr;
    bool suppress_changed_ = false;
    bool visible_ = true;
    float last_height_ = 0.f;
    Rect last_rect_ = {-1.f, -1.f, -1.f, -1.f};
    // Rect actually applied to hwnd_ (in DIPs) by the last set_rect() call —
    // see rendered_image_rect().
    Rect applied_rect_{};
    std::unique_ptr<tk::Image> cached_image_;
    // Reused scratch buffer for refresh_image()'s synchronous capture.
    std::vector<std::uint8_t> pending_pixels_;
    std::function<void(Rect)> on_repaint_needed_;
    std::function<void(const std::string&)>     on_changed_;
    std::function<void()>                       on_submit_;
    std::function<void(float)>                  on_height_changed_;
    ImagePasteHandler                           on_image_paste_;
    std::function<bool(NativeTextArea::NavKey)> popup_nav_;
    std::function<void(bool)>                   on_focus_changed_;
    std::function<void()>                       on_pointer_down_;
    std::function<bool()>                       on_edit_last_;
    std::function<const tk::Image*(const std::string&)> image_resolver_;
    ImageProviderAdapter                        image_provider_{ this };
    std::unordered_set<std::wstring>            pending_image_uris_;
    Color mention_bg_ = Color::rgb(0x0078D4);
    Color mention_fg_ = Color::rgba(255, 255, 255, 255);
    int mention_counter_ = 0;
    // Whether the last reformat_emoji_runs() pass found any emoji — lets a
    // change with no emoji (the common case) skip BetterTextSetTextStyle
    // entirely instead of issuing a whole-range reset (and its undo entry)
    // on every keystroke, once there is nothing stale left to clear.
    bool had_emoji_runs_ = false;

    // Mention pills are real BetterText Image atoms (see insert_mention /
    // render_mention_pill above), keyed by their synthetic
    // "tesseract-mention:<n>" uri — resolve_image_uri() hands back the
    // pre-rendered bitmap when asked, and composer_draft() looks runs up in
    // this map (rather than real mxc:// emoticon uris) to recover the original
    // user_id/display_name/is_room for reconstructing the draft.
    struct MentionRun
    {
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        std::string user_id, display_name;
        bool is_room;
    };
    std::unordered_map<std::wstring, MentionRun> mention_runs_;
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
//  Win32PopupSurfaceHandle — topmost WS_POPUP-backed tk::PopupSurfaceHandle
// ─────────────────────────────────────────────────────────────────────────
//
// One per Host::make_popup_surface(). Mirrors the hand-rolled pattern
// previously duplicated per popup type in each shell's RoomWindow.cpp/
// MainWindow.cpp (mention/slash/shortcode/gif popups): a topmost,
// tool-window-styled WS_POPUP HWND (no taskbar/alt-tab entry) hosting its
// own tk::win32::Surface as a child HWND, positioned via the same
// ClientToScreen + MonitorFromPoint + above/below-fallback logic each shell
// used to hand-roll as place_anchored_popup_. Outside-click auto-dismiss
// (on_dismiss_requested) is implemented via a thread-scoped WH_MOUSE hook —
// see watch_clicks_()/mouse_hook_proc() below — installed only while a
// caller actually sets on_dismiss_requested and the popup is visible,
// mirroring the Qt backend's app-wide QEvent filter (host_qt.cpp's
// QtPopupSurfaceHandle) without needing a system-wide low-level hook:
// WH_MOUSE only observes messages destined for the calling thread's own
// windows.
class Win32PopupSurfaceHandle : public tk::PopupSurfaceHandle
{
public:
    Win32PopupSurfaceHandle(HWND anchor_hwnd, HINSTANCE inst, const Theme& theme)
        : anchor_hwnd_(anchor_hwnd)
    {
        popup_hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, 0, 0,
            10, 10, nullptr, nullptr, inst, nullptr);
        surface_ = std::make_unique<Surface>(inst, popup_hwnd_, theme);
    }

    ~Win32PopupSurfaceHandle() override
    {
        unwatch_clicks_();
        surface_.reset(); // destroy the embedded Surface's child HWND first
        if (popup_hwnd_)
        {
            DestroyWindow(popup_hwnd_);
        }
    }

    void set_root(std::unique_ptr<Widget> root) override
    {
        surface_->set_root(std::move(root));
    }

    void set_rect(Rect anchor_world_rect, Size size,
                  tk::PopupPlacement placement) override
    {
        if (!popup_hwnd_ || !anchor_hwnd_)
        {
            return;
        }
        const int w = std::max(1, int(size.w));
        const int h = std::max(1, int(size.h));
        POINT pt{LONG(anchor_world_rect.x), LONG(anchor_world_rect.y)};
        ClientToScreen(anchor_hwnd_, &pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(mon, &mi);
        int x = pt.x;
        const int y_above = pt.y - h - 4;
        const int y_below = pt.y + int(anchor_world_rect.h) + 4;
        int y;
        if (placement == tk::PopupPlacement::PreferAbove)
            y = (y_above >= mi.rcWork.top) ? y_above : y_below;
        else
            y = (y_below + h <= mi.rcWork.bottom) ? y_below : y_above;
        x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
        y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);
        // No show/hide flags — visibility is set_visible()'s job alone, so
        // repositioning an already-open popup (e.g. row count changed) never
        // has a side effect on whether it's shown.
        SetWindowPos(popup_hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        if (HWND s = surface_->hwnd())
        {
            SetWindowPos(s, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        apply_region_(w, h);
        surface_->relayout();
    }

    void set_corner_radius(float radius_dip) override
    {
        corner_radius_dip_ = radius_dip;
        apply_region_(last_w_, last_h_);
    }

    void set_visible(bool visible) override
    {
        if (visible == visible_ || !popup_hwnd_)
        {
            return;
        }
        visible_ = visible;
        ShowWindow(popup_hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (visible)
        {
            watch_clicks_();
        }
        else
        {
            unwatch_clicks_();
        }
    }

    bool visible() const override
    {
        return visible_;
    }

    void set_theme(const Theme& theme) override
    {
        surface_->set_theme(theme);
        if (surface_->root())
        {
            surface_->root()->apply_theme(theme);
        }
    }

    void request_repaint() override
    {
        surface_->host().request_repaint();
    }

    void request_relayout() override
    {
        surface_->relayout();
    }

    void set_anim_cache(const tk::AnimImageCache* cache) override
    {
        surface_->set_anim_cache(cache);
    }

    void update_anim_regions() override
    {
        surface_->update_anim_regions();
    }

private:
    void watch_clicks_()
    {
        if (!on_dismiss_requested || watching_)
        {
            return;
        }
        watching_ = true;
        instances_().push_back(this);
        if (!hook_())
        {
            hook_() = SetWindowsHookExW(
                WH_MOUSE, &Win32PopupSurfaceHandle::mouse_hook_proc, nullptr,
                GetCurrentThreadId());
        }
    }

    void unwatch_clicks_()
    {
        if (!watching_)
        {
            return;
        }
        watching_ = false;
        auto& v = instances_();
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
        if (v.empty() && hook_())
        {
            UnhookWindowsHookEx(hook_());
            hook_() = nullptr;
        }
    }

    // WH_MOUSE (not WH_MOUSE_LL) — only observes messages destined for the
    // calling thread's own windows, i.e. this app's own windows, matching
    // the Qt backend's app-scoped (not system-wide) click observation.
    // Never claims/blocks the message: always falls through to
    // CallNextHookEx so normal click handling (caret placement, etc.) on
    // whatever native control the click actually landed in is unaffected.
    static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wParam,
                                            LPARAM lParam)
    {
        if (code == HC_ACTION &&
            (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN))
        {
            auto* info = reinterpret_cast<MOUSEHOOKSTRUCT*>(lParam);
            const POINT pt = info->pt;
            // Snapshot — on_dismiss_requested commonly hides the popup,
            // which reenters unwatch_clicks_() and mutates instances_()
            // mid-iteration otherwise.
            auto snapshot = instances_();
            for (auto* inst : snapshot)
            {
                if (!inst->visible_ || !inst->popup_hwnd_)
                {
                    continue;
                }
                RECT r;
                GetWindowRect(inst->popup_hwnd_, &r);
                if (!PtInRect(&r, pt) && inst->on_dismiss_requested)
                {
                    inst->on_dismiss_requested();
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    static std::vector<Win32PopupSurfaceHandle*>& instances_()
    {
        static std::vector<Win32PopupSurfaceHandle*> v;
        return v;
    }
    static HHOOK& hook_()
    {
        static HHOOK h = nullptr;
        return h;
    }

    // Re-applies popup_hwnd_'s window region from corner_radius_dip_ and the
    // window's current pixel size — called on every set_rect() (size may
    // have changed) and from set_corner_radius() itself. A radius of 0
    // clears the region (square window, the default/common case).
    void apply_region_(int w, int h)
    {
        last_w_ = w;
        last_h_ = h;
        if (!popup_hwnd_)
        {
            return;
        }
        if (corner_radius_dip_ <= 0.0f)
        {
            SetWindowRgn(popup_hwnd_, nullptr, TRUE);
            return;
        }
        // SetWindowRgn is a different mechanism from WS_EX_LAYERED/DXGI
        // alpha compositing — it's a hard, OS-level clip of what DWM shows
        // for this HWND (and its children), computed from the *already
        // rendered* content, with no swap-chain/presentation-model
        // implications. See BetterTextField's ctor comment for why the
        // WS_EX_LAYERED route was tried and rejected elsewhere in this file.
        UINT dpi = GetDpiForWindow(popup_hwnd_);
        const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
        const int d = std::max(1, static_cast<int>(std::round(corner_radius_dip_ * 2.0f * scale)));
        // CreateRoundRectRgn reports a region ~1px smaller than the rect
        // passed in (a long-documented GDI quirk) — pad right/bottom by 1
        // so the region doesn't clip the popup's own rightmost/bottommost
        // content column (e.g. the border stroke DropdownList paints along
        // its rounded edge).
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d);
        SetWindowRgn(popup_hwnd_, rgn, TRUE); // popup_hwnd_ now owns rgn
    }

    HWND anchor_hwnd_ = nullptr;
    HWND popup_hwnd_ = nullptr;
    std::unique_ptr<Surface> surface_;
    bool visible_ = false;
    bool watching_ = false;
    float corner_radius_dip_ = 0.0f;
    int last_w_ = 0;
    int last_h_ = 0;
};

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

    // Scoped repaint for canvas-drawn native text controls (see
    // NativeTextField::set_on_repaint_needed's doc comment in host.h) —
    // same rect-scoped InvalidateRect(hwnd,&rect,...) primitive
    // invalidate_anim_damage() already uses for animated-image damage,
    // just driven by a caller-supplied rect instead of the anim-damage list.
    void request_repaint_rect(Rect world) override
    {
        if (!hwnd_)
        {
            return;
        }
        // world is in DIP (logical) units, like every other widget-tree
        // rect — InvalidateRect wants client (physical) pixels, so this
        // must go through dip_to_phys() like every other DIP->pixel
        // conversion in this file (see on_paint()'s req_dirty for the
        // inverse). Skipping that conversion left the invalidated rect
        // too small and mispositioned on any display scaled above 100%,
        // so the resulting WM_PAINT never covered the pixels that actually
        // changed — visible as typed text not appearing until an unrelated
        // full repaint (e.g. request_repaint()'s hover-fade-driven
        // unscoped InvalidateRect on mouse move) caught it up.
        RECT rc;
        rc.left   = dip_to_phys(world.x) - 1;
        rc.top    = dip_to_phys(world.y) - 1;
        rc.right  = dip_to_phys(world.x + world.w) + 1;
        rc.bottom = dip_to_phys(world.y + world.h) + 1;
        InvalidateRect(hwnd_, &rc, FALSE);
    }

    void request_relayout() override
    {
        // relayout() (below) already ends with request_repaint(), so no
        // separate call is needed here.
        relayout();
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
            // See request_repaint_rect()'s comment — same DIP->physical-pixel
            // conversion needed here, for the same reason.
            RECT rc;
            rc.left   = dip_to_phys(r.x) - 1;
            rc.top    = dip_to_phys(r.y) - 1;
            rc.right  = dip_to_phys(r.x + r.w) + 1;
            rc.bottom = dip_to_phys(r.y + r.h) + 1;
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

    bool is_network_available() const override
    {
        Microsoft::WRL::ComPtr<INetworkListManager> mgr;
        if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(&mgr))) || !mgr)
            return true; // can't probe — don't block login on it
        VARIANT_BOOL connected = VARIANT_FALSE;
        if (FAILED(mgr->get_IsConnectedToInternet(&connected))) return true;
        return connected != VARIANT_FALSE;
    }

    std::unique_ptr<NativeTextField> make_text_field() override
    {
        int id = next_ctrl_id_++;
        auto field = std::make_unique<BetterTextField>(hwnd_, id, theme_);
        areas_by_id_.emplace(id, field.get());
        // Erase the registry entry when this control is destroyed —
        // otherwise a repeatedly opened/closed transient field (search bar,
        // quick switcher, ...) leaves a dangling pointer that set_theme()'s
        // areas_by_id_ iteration would dereference on the next theme change.
        field->set_on_destroyed([this, id] { areas_by_id_.erase(id); });
        field->set_on_hover_changed(
            [this](bool hovering) { set_cursor(hovering ? Cursor::IBeam : Cursor::Default); });
        return field;
    }

    std::unique_ptr<NativeTextArea> make_text_area() override
    {
        int id = next_ctrl_id_++;
        auto fac = d2d::factories(backend_singleton());
        auto area = std::make_unique<BetterTextArea>(hwnd_, id, fac.wic, theme_);
        areas_by_id_.emplace(id, area.get());
        area->set_on_destroyed([this, id] { areas_by_id_.erase(id); });
        area->set_on_hover_changed(
            [this](bool hovering) { set_cursor(hovering ? Cursor::IBeam : Cursor::Default); });
        return area;
    }

    std::unique_ptr<tk::PopupSurfaceHandle> make_popup_surface() override
    {
        if (!hwnd_)
        {
            return nullptr;
        }
        HINSTANCE inst =
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
        return std::make_unique<Win32PopupSurfaceHandle>(hwnd_, inst, *theme_);
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
    std::unique_ptr<AudioPlayback> make_audio_playback() override
    {
        return make_audio_playback_win32();
    }

    std::vector<tk::DeviceListing> enumerate_audio_inputs() const override
    {
        return enumerate_wasapi_endpoints(eCapture);
    }

    std::vector<tk::DeviceListing> enumerate_audio_outputs() const override
    {
        return enumerate_wasapi_endpoints(eRender);
    }

    std::vector<tk::DeviceListing> enumerate_cameras() const override
    {
        std::vector<tk::DeviceListing> result;

        IMFAttributes* attrs = nullptr;
        if (FAILED(MFCreateAttributes(&attrs, 1)))
            return result;
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count)))
        {
            for (UINT32 i = 0; i < count; ++i)
            {
                WCHAR* sym = nullptr;
                UINT32 sym_len = 0;
                devices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                    &sym, &sym_len);

                WCHAR* name = nullptr;
                UINT32 name_len = 0;
                devices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &name_len);

                if (sym)
                {
                    tk::DeviceListing entry;
                    entry.id           = wide_to_utf8(sym);
                    entry.display_name = name ? wide_to_utf8(name) : entry.id;
                    result.push_back(std::move(entry));
                    CoTaskMemFree(sym);
                }
                if (name) CoTaskMemFree(name);
                devices[i]->Release();
            }
            CoTaskMemFree(devices);
        }
        attrs->Release();
        return result;
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

    bool
    set_clipboard_image(std::span<const std::uint8_t> encoded_bytes) override
    {
        if (encoded_bytes.empty())
            return false;

        using Microsoft::WRL::ComPtr;
        IWICImagingFactory* wic = d2d::factories(backend_singleton()).wic;
        if (!wic)
            return false;

        // Decode the encoded blob and normalise to 32bpp BGRA.
        ComPtr<IWICStream> stream;
        if (FAILED(wic->CreateStream(stream.GetAddressOf())))
            return false;
        if (FAILED(stream->InitializeFromMemory(
                const_cast<BYTE*>(encoded_bytes.data()),
                static_cast<DWORD>(encoded_bytes.size()))))
            return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnLoad,
                                                decoder.GetAddressOf())))
            return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
            return false;
        ComPtr<IWICBitmapSource> bgra;
        if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA,
                                          frame.Get(), bgra.GetAddressOf())))
            return false;

        UINT w = 0, h = 0;
        bgra->GetSize(&w, &h);
        if (w == 0 || h == 0)
            return false;

        const SIZE_T stride = static_cast<SIZE_T>(w) * 4;
        const SIZE_T pixel_bytes = stride * h;
        HGLOBAL hg =
            GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + pixel_bytes);
        if (!hg)
            return false;
        auto* base = static_cast<std::uint8_t*>(GlobalLock(hg));
        if (!base)
        {
            GlobalFree(hg);
            return false;
        }

        auto* hdr = reinterpret_cast<BITMAPV5HEADER*>(base);
        std::memset(hdr, 0, sizeof(BITMAPV5HEADER));
        hdr->bV5Size = sizeof(BITMAPV5HEADER);
        hdr->bV5Width = static_cast<LONG>(w);
        hdr->bV5Height = -static_cast<LONG>(h); // top-down
        hdr->bV5Planes = 1;
        hdr->bV5BitCount = 32;
        hdr->bV5Compression = BI_BITFIELDS;
        hdr->bV5RedMask = 0x00FF0000;
        hdr->bV5GreenMask = 0x0000FF00;
        hdr->bV5BlueMask = 0x000000FF;
        hdr->bV5AlphaMask = 0xFF000000;
        hdr->bV5CSType = LCS_WINDOWS_COLOR_SPACE;

        std::uint8_t* pixels = base + sizeof(BITMAPV5HEADER);
        if (FAILED(bgra->CopyPixels(nullptr, static_cast<UINT>(stride),
                                    static_cast<UINT>(pixel_bytes), pixels)))
        {
            GlobalUnlock(hg);
            GlobalFree(hg);
            return false;
        }
        GlobalUnlock(hg);

        if (!OpenClipboard(hwnd_))
        {
            GlobalFree(hg);
            return false;
        }
        EmptyClipboard();
        if (!SetClipboardData(CF_DIBV5, hg))
        {
            // Clipboard did not take ownership; free our copy.
            GlobalFree(hg);
            CloseClipboard();
            return false;
        }
        CloseClipboard();
        return true;
    }

    Win32TextAreaBase* area_by_id(int id)
    {
        auto it = areas_by_id_.find(id);
        return it == areas_by_id_.end() ? nullptr : it->second;
    }

    // ── Internal ──────────────────────────────────────────────────────
    void set_root(std::unique_ptr<Widget> root)
    {
        auto wrapper = create_root_widget<RootWidget>(this);
        wrapper->add_child(std::move(root));
        root_ = std::move(wrapper);
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
        // WM_PAINT fires with the new palette.
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

        // Scope the paint to the actual invalidated rect (e.g. the small
        // region InvalidateRect'd by invalidate_anim_damage() for an
        // animated-image tick, or by a focused field's caret-blink timer —
        // see NativeTextField::caret_owned_by_canvas()) instead of always
        // repainting the whole window. ListView::paint reads this back via
        // canvas.clip_rect() to skip rows outside it entirely.
        const bool req_has_dirty = !IsRectEmpty(&ps.rcPaint);
        Rect req_dirty{};
        if (req_has_dirty)
        {
            req_dirty = {phys_to_dip(static_cast<float>(ps.rcPaint.left)),
                         phys_to_dip(static_cast<float>(ps.rcPaint.top)),
                         phys_to_dip(static_cast<float>(ps.rcPaint.right -
                                                        ps.rcPaint.left)),
                         phys_to_dip(static_cast<float>(ps.rcPaint.bottom -
                                                        ps.rcPaint.top))};
        }
        // begin_paint() may return a larger region than requested — for
        // this (opaque) surface it never does in practice, since a
        // single-buffer BLT-model swap chain has no "other buffer" to
        // fall behind on, but the contract is shared with transparent
        // surfaces' flip-model path (see canvas_d2d.h's Surface/
        // begin_paint doc comments), so always clip/fill/repaint to the
        // returned values, not req_dirty, to stay correct under either.
        auto [canvas, has_dirty, dirty_rect] =
            d2d_surface_->begin_paint(req_has_dirty, req_dirty);
        if (has_dirty)
        {
            canvas.push_clip_rect(dirty_rect);
        }
        // Transparent surfaces (overlays) clear to fully transparent so DWM
        // composites the per-pixel alpha against the content behind the
        // window — Canvas::clear() (ID2D1RenderTarget::Clear) always wipes
        // the *entire* render target regardless of any active clip, so it
        // can't be scoped to dirty_rect; fine, since overlays don't hit the
        // caret-blink/anim-damage scoped-repaint path often enough for the
        // extra full-surface cost to matter. Opaque windows (the common
        // case) use fill_rect instead when scoped, since — unlike clear() —
        // it respects the pushed clip: clearing the *whole* buffer ahead of
        // a scoped repaint would blank the rest of the window for this
        // frame, and with a flip-model swap chain that blank frame becomes
        // visible on Present, alternating with the real content on the
        // other back buffer — i.e. a flicker, synced to whatever's driving
        // the scoped repaint (every ~530ms for a blinking caret).
        if (transparent_)
        {
            canvas.clear(Color{0, 0, 0, 0});
        }
        else if (has_dirty)
        {
            canvas.fill_rect(dirty_rect, theme_->palette.bg);
        }
        else
        {
            canvas.clear(theme_->palette.bg);
        }
        if (root_)
        {
            pending_popup_.reset();
            pending_popup_trigger_.reset();
            anim_damage_.clear();
            PaintCtx ctx{canvas, *factory_, *theme_, this, this};
            root_->paint(ctx);
            popup_ = pending_popup_;
            popup_trigger_ = pending_popup_trigger_;
            root_->paint_overlay(ctx);
            RECT client_rc;
            GetClientRect(hwnd_, &client_rc);
            Rect surface_bounds{
                0, 0, phys_to_dip(static_cast<float>(client_rc.right)),
                phys_to_dip(static_cast<float>(client_rc.bottom))};
            paint_tooltip_overlay(ctx, surface_bounds);
            paint_focus_overlay(ctx);
            paint_toast_overlay(ctx, surface_bounds);
        }
        if (has_dirty)
        {
            canvas.pop_clip();
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
        // Native capture step: route subsequent moves/up to this window even
        // when the pointer leaves it during a drag. Kept here (not in the
        // shared dispatch) because it is Win32-specific. Gated on root_ to
        // match the original ordering (no capture when there is no tree).
        if (root_)
        {
            SetCapture(hwnd_);
        }
        dispatch_pointer_down({phys_to_dip(static_cast<float>(x)),
                               phys_to_dip(static_cast<float>(y))});
    }

    void on_pointer_up(int x, int y)
    {
        // Release the Win32 capture grabbed in on_pointer_down before running
        // the shared release logic.
        if (GetCapture() == hwnd_)
        {
            ReleaseCapture();
        }
        dispatch_pointer_up({phys_to_dip(static_cast<float>(x)),
                             phys_to_dip(static_cast<float>(y))});
    }

    void on_pointer_move(int x, int y)
    {
        dispatch_pointer_move({phys_to_dip(static_cast<float>(x)),
                               phys_to_dip(static_cast<float>(y))});
    }

    void on_pointer_leave() { dispatch_pointer_leave(); }

    void on_wheel(int screen_x, int screen_y, int delta_steps, bool is_touchpad = false)
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
        if (dispatch_wheel(
                {phys_to_dip(static_cast<float>(pt.x)),
                 phys_to_dip(static_cast<float>(pt.y))},
                0, dy, is_touchpad))
        {
            request_repaint();
            on_pointer_move(pt.x, pt.y);
        }
    }

    bool on_key_down(const KeyEvent& event)
    {
        return dispatch_key_down(event);
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
        LPCWSTR name = IDC_ARROW;
        if (c == Cursor::Pointer) name = IDC_HAND;
        else if (c == Cursor::IBeam) name = IDC_IBEAM;
        HCURSOR newc = LoadCursorW(nullptr, name);
        if (newc == current_cursor_) return;
        current_cursor_ = newc;
        // Apply immediately so the change is visible before the next
        // WM_SETCURSOR. SetCursor only affects the visible cursor when the
        // pointer is over the calling thread's window, so this is a no-op
        // when the user has moved off the window entirely.
        SetCursor(newc);
    }

protected:
    Widget* input_root_() const override { return root_.get(); }

private:
    HWND hwnd_;
    const Theme* theme_;
    bool transparent_ = false;
    std::unique_ptr<d2d::Surface> d2d_surface_;
    std::unique_ptr<CanvasFactory> factory_;

    // Declared before root_ so it is destroyed *after* root_ (member
    // destruction runs in reverse declaration order). root_'s teardown
    // recursively destroys every BetterTextField/BetterTextArea still in
    // the tree, and each one erases its own entry here via the
    // set_on_destroyed() callback wired in make_text_field()/
    // make_text_area() — that erase would touch an already-destroyed map
    // if areas_by_id_ were declared (and thus destroyed) before root_.
    int next_ctrl_id_ = 0x4000;
    std::unordered_map<int, Win32TextAreaBase*> areas_by_id_;

    std::unique_ptr<Widget> root_;
    std::function<void()> on_layout_;
    HCURSOR current_cursor_ = LoadCursorW(nullptr, IDC_ARROW);
    const tk::AnimImageCache* anim_cache_ = nullptr;
    std::vector<tk::Rect> anim_damage_;

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
    void set_on_file_drop_error(FileDropErrorHandler cb)
    {
        on_file_drop_error_ = std::move(cb);
    }
    // Forwards the payload into the widget tree via the shared
    // Host::dispatch_file_drop. Returns true if some widget claimed it. A
    // window that isn't currently shown (e.g. the Settings window while a
    // different top-level has focus) shouldn't process a drop even if its
    // HWND is still a registered drop target.
    bool fire_file_drop(std::vector<std::uint8_t> bytes, std::string mime,
                        std::string filename, tk::Point pos)
    {
        if (!hwnd_ || !IsWindowVisible(hwnd_))
            return false;
        tk::FileDropPayload payload{std::move(bytes), std::move(mime),
                                    std::move(filename)};
        return dispatch_file_drop(pos, payload) != nullptr;
    }
    // Converts a drop's screen-space POINTL to the same client-area,
    // DPI-independent tk::Point space fire_right_click already builds —
    // mirrors the ScreenToClient + phys_to_dip pipeline the WM_MOUSEWHEEL
    // path uses (that message, unlike WM_LBUTTONDOWN/WM_MOUSEMOVE, also
    // delivers screen coordinates).
    tk::Point screen_to_tk_point(POINT screen_pt) const
    {
        POINT pt = screen_pt;
        ScreenToClient(hwnd_, &pt);
        return {phys_to_dip(static_cast<float>(pt.x)),
               phys_to_dip(static_cast<float>(pt.y))};
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
    // Drag-hover entry points for DropTarget::DragOver/DragLeave (a plain
    // COM object holding a Host*, not a Host member — need a public wrapper
    // around the protected shared dispatch, mirroring fire_file_drop above).
    // The per-widget highlight these drive replaces the old whole-surface
    // "Drop to attach" overlay.
    Widget* hover_file_drop(tk::Point pos)
    {
        if (!hwnd_ || !IsWindowVisible(hwnd_))
            return nullptr;
        return dispatch_drag_hover(pos);
    }
    void leave_file_drop()
    {
        dispatch_drag_leave();
    }

private:
    FileDropErrorHandler on_file_drop_error_;
    std::function<void(tk::Point)> on_right_click_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Surface — child HWND + window-class registration
// ─────────────────────────────────────────────────────────────────────────

namespace
{

constexpr const wchar_t* kSurfaceClass = L"tk_win32_Surface";

// Windows tags every touch/pen-injected input event (including a Precision
// Touchpad's translated scroll gesture) with this signature in
// GetMessageExtraInfo(), the same technique Chromium and Firefox use to
// distinguish a genuine physical mouse wheel notch from a touchpad's
// synthesized WM_MOUSEWHEEL — there's no other native signal for it, since
// both arrive as the same message.
constexpr LPARAM kTouchInjectedSignatureMask = 0xFFFFFF00;
constexpr LPARAM kTouchInjectedSignature = 0xFF515700; // MI_WP_SIGNATURE

bool wheel_event_is_touchpad()
{
    return (static_cast<LPARAM>(GetMessageExtraInfo()) & kTouchInjectedSignatureMask) ==
           kTouchInjectedSignature;
}

Key key_from_win32(WPARAM vk, bool shift)
{
    switch (vk)
    {
    case VK_ESCAPE: return Key::Escape;
    case VK_RETURN: return Key::Enter;
    case VK_SPACE: return Key::Space;
    case VK_TAB: return shift ? Key::Backtab : Key::Tab;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    default: return Key::Unknown;
    }
}

std::string character_text_from_win32(WPARAM vk, bool shift)
{
    if (vk >= 'A' && vk <= 'Z')
    {
        const char base = shift ? 'A' : 'a';
        return std::string(1, static_cast<char>(base + vk - 'A'));
    }
    if (vk >= '0' && vk <= '9')
    {
        return std::string(1, static_cast<char>('0' + vk - '0'));
    }
    return {};
}

KeyEvent translate_key_event(WPARAM vk, LPARAM lParam)
{
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    KeyEvent out{};
    out.key = key_from_win32(vk, shift);
    out.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    out.shift = shift;
    out.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    out.meta = (GetKeyState(VK_LWIN) & 0x8000) != 0 ||
               (GetKeyState(VK_RWIN) & 0x8000) != 0;
    out.repeat = (lParam & (1L << 30)) != 0;
    if (out.key == Key::Unknown)
    {
        out.text = character_text_from_win32(vk, shift);
        if (!out.text.empty())
        {
            out.key = Key::Character;
        }
    }
    return out;
}

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
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
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
            SetFocus(hwnd);
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
        // cursor is over a child HWND — e.g. a NativeTextField / NativeTextArea
        // EDIT control — wParam is the child's HWND; we must NOT return TRUE
        // there or the child's default WndProc never runs and the I-beam is
        // suppressed. Falling through to DefWindowProc lets the message
        // bubble back to the child.
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
            host->on_wheel(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), delta,
                           wheel_event_is_touchpad());
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        if (host)
        {
            KeyEvent event = translate_key_event(wParam, lParam);
            if (event.key != Key::Unknown && host->on_key_down(event))
            {
                return 0;
            }
        }
        break;
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
                                        DWORD /*grfKeyState*/, POINTL pt,
                                        DWORD* pdwEffect) override
    {
        if (!pdwEffect)
        {
            return E_POINTER;
        }
        accept_ = host_ && acceptable(data);
        *pdwEffect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accept_ && host_)
        {
            host_->hover_file_drop(host_->screen_to_tk_point(POINT{pt.x, pt.y}));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD /*grfKeyState*/, POINTL pt,
                                       DWORD* pdwEffect) override
    {
        if (!pdwEffect)
        {
            return E_POINTER;
        }
        *pdwEffect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accept_ && host_)
        {
            host_->hover_file_drop(host_->screen_to_tk_point(POINT{pt.x, pt.y}));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        accept_ = false;
        if (host_)
        {
            host_->leave_file_drop();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD /*grfKeyState*/,
                                   POINTL pt, DWORD* pdwEffect) override
    {
        if (pdwEffect)
        {
            *pdwEffect = DROPEFFECT_NONE;
        }
        if (host_)
        {
            host_->leave_file_drop();
        }
        if (!accept_ || !host_)
        {
            return S_OK;
        }

        // IDropTarget::Drop's POINTL is screen-space per the OLE contract
        // (unlike WM_LBUTTONDOWN/WM_MOUSEMOVE, which are already client-
        // relative) — convert once, same pipeline WM_MOUSEWHEEL uses.
        const tk::Point drop_pos =
            host_->screen_to_tk_point(POINT{pt.x, pt.y});

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
                if (try_dispatch_file(path, drop_pos))
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

    bool try_dispatch_file(const std::wstring& path, tk::Point pos)
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

        return host_->fire_file_drop(std::move(bytes), std::move(mime),
                                     wide_to_utf8(basename(path)), pos);
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

    // Register an OLE drop target. Routing is tree-dispatched automatically
    // (DropTarget::Drop -> Host::fire_file_drop -> Host::dispatch_file_drop);
    // nothing needs to be wired here. RegisterDragDrop fails silently when
    // the caller hasn't OleInitialize'd their thread; the shell is
    // responsible for that (main.cpp).
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
