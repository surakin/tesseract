#include "host_win32.h"
#include "canvas_d2d.h"
#include "controls.h"

#include <tesseract/settings.h>

#include <commctrl.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <wincodec.h>
#include <objidl.h>
#include <ole2.h>       // RegisterDragDrop / IDropTarget
#include <shellapi.h>   // CF_HDROP / DragQueryFileW
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <atomic>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tk::win32 {

// ─────────────────────────────────────────────────────────────────────────
//  Process-wide D2D backend + post-to-UI message
// ─────────────────────────────────────────────────────────────────────────

namespace {

d2d::Backend& backend_singleton() {
    static d2d::Backend instance;
    return instance;
}

// One registered window message per process for the post_to_ui channel.
// The lParam is a heap-allocated std::function<void()>* that the
// receiving WndProc invokes and frees.
UINT post_to_ui_message() {
    static UINT msg = RegisterWindowMessageW(L"tk_post_to_ui");
    return msg;
}

// Process-wide font for native EDIT overlays — matches FontRole::Body
// ("Segoe UI Variable Text", Settings::font_body pt, regular weight) so
// text input fields render in the same face and size as message body text.
// On systems without "Segoe UI Variable Text" (pre-Win11) GDI silently
// substitutes "Segoe UI" at the same size, which is visually identical.
HFONT body_font() {
    static HFONT cached = []() -> HFONT {
        const int pt = tesseract::Settings::instance().font_body;
        HDC hdc = GetDC(nullptr);
        int h = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        ReleaseDC(nullptr, hdc);
        LOGFONTW lf{};
        lf.lfHeight         = h;
        lf.lfWeight         = FW_REGULAR;
        lf.lfCharSet        = DEFAULT_CHARSET;
        lf.lfQuality        = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
        HFONT font = CreateFontIndirectW(&lf);
        return font ? font
                    : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }();
    return cached;
}

// Converters.
inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

inline std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                 static_cast<int>(s.size()),
                                 nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(),
                         static_cast<int>(s.size()),
                         out.data(), n, nullptr, nullptr);
    return out;
}

// ── Clipboard image extraction ────────────────────────────────────────────
//
// Read CF_DIBV5 or CF_DIB from the Windows clipboard, decode through WIC,
// and re-encode as PNG so the shared layer doesn't need to understand DIB
// memory layouts. The output mime is always "image/png" because we lose
// the source identity when transcoding from a DIB.
inline bool clipboard_image_to_png(IWICImagingFactory* wic,
                                    HWND owner,
                                    std::vector<std::uint8_t>& out) {
    if (!OpenClipboard(owner)) return false;
    struct CloseGuard { ~CloseGuard() { CloseClipboard(); } } guard;

    UINT fmt = 0;
    if (IsClipboardFormatAvailable(CF_DIBV5)) fmt = CF_DIBV5;
    else if (IsClipboardFormatAvailable(CF_DIB)) fmt = CF_DIB;
    else return false;

    HGLOBAL hg = GetClipboardData(fmt);
    if (!hg) return false;
    SIZE_T sz = GlobalSize(hg);
    void*  data = GlobalLock(hg);
    if (!data || sz == 0) { if (data) GlobalUnlock(hg); return false; }

    // A CF_DIB/CF_DIBV5 payload starts with a BITMAPINFOHEADER (or V5
    // header) followed by colour table + pixel data. WIC's
    // CreateDecoderFromStream needs a full BMP file (with file header).
    // Synthesize a 14-byte BITMAPFILEHEADER in front of the DIB.
    std::vector<std::uint8_t> bmp;
    bmp.resize(sizeof(BITMAPFILEHEADER) + sz);
    BITMAPFILEHEADER* bfh = reinterpret_cast<BITMAPFILEHEADER*>(bmp.data());
    bfh->bfType    = 0x4D42;   // 'BM'
    bfh->bfSize    = static_cast<DWORD>(bmp.size());
    bfh->bfReserved1 = 0;
    bfh->bfReserved2 = 0;

    const BITMAPINFOHEADER* bih =
        reinterpret_cast<const BITMAPINFOHEADER*>(data);
    DWORD header_size = bih->biSize;
    // Colour table for paletted / bitfields formats.
    DWORD palette_bytes = 0;
    if (bih->biBitCount <= 8) {
        DWORD entries = bih->biClrUsed ? bih->biClrUsed
                                       : (1u << bih->biBitCount);
        palette_bytes = entries * sizeof(RGBQUAD);
    } else if (bih->biCompression == BI_BITFIELDS) {
        palette_bytes = 3 * sizeof(DWORD);
    }
    bfh->bfOffBits = sizeof(BITMAPFILEHEADER) + header_size + palette_bytes;
    std::memcpy(bmp.data() + sizeof(BITMAPFILEHEADER), data, sz);
    GlobalUnlock(hg);

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromMemory(
            bmp.data(), static_cast<DWORD>(bmp.size())))) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(
            stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad,
            decoder.GetAddressOf()))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;

    // Encode to PNG into an in-memory IStream.
    ComPtr<IStream> mem_stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE,
                                       mem_stream.GetAddressOf())))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    encoder.GetAddressOf()))) return false;
    if (FAILED(encoder->Initialize(mem_stream.Get(),
                                     WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> out_frame;
    ComPtr<IPropertyBag2>         props;
    if (FAILED(encoder->CreateNewFrame(out_frame.GetAddressOf(),
                                         props.GetAddressOf()))) return false;
    if (FAILED(out_frame->Initialize(nullptr))) return false;
    if (FAILED(out_frame->WriteSource(frame.Get(), nullptr))) return false;
    if (FAILED(out_frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;

    // Read the encoded bytes back from the stream.
    HGLOBAL h_out = nullptr;
    if (FAILED(GetHGlobalFromStream(mem_stream.Get(), &h_out)) || !h_out)
        return false;
    SIZE_T n = GlobalSize(h_out);
    void* p = GlobalLock(h_out);
    if (!p || n == 0) { if (p) GlobalUnlock(h_out); return false; }
    out.assign(static_cast<const std::uint8_t*>(p),
                static_cast<const std::uint8_t*>(p) + n);
    GlobalUnlock(h_out);
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Win32NativeTextField — EDIT-control NativeTextField
// ─────────────────────────────────────────────────────────────────────────
//
// One EDIT child window per make_text_field(). The EDIT is subclassed so
// VK_RETURN raises on_submit_ rather than getting eaten silently.
// EN_CHANGE notifications arrive at the parent surface as WM_COMMAND;
// the surface forwards them to the right NativeTextField by control ID.

class Win32NativeTextField : public NativeTextField {
public:
    Win32NativeTextField(HWND parent, int ctrl_id)
        : parent_(parent), id_(ctrl_id) {
        hwnd_ = CreateWindowExW(
            0,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 100, 24,
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_)),
            reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
            nullptr);
        if (!hwnd_) return;
        SendMessageW(hwnd_, WM_SETFONT,
                      reinterpret_cast<WPARAM>(body_font()), FALSE);
        SetWindowSubclass(hwnd_, &Win32NativeTextField::subclass_proc,
                           1, reinterpret_cast<DWORD_PTR>(this));
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

    ~Win32NativeTextField() override {
        if (hwnd_) {
            RemoveWindowSubclass(hwnd_, &Win32NativeTextField::subclass_proc, 1);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void set_rect(Rect r) override {
        if (!hwnd_) return;
        if (r.x == last_rect_.x && r.y == last_rect_.y &&
            r.w == last_rect_.w && r.h == last_rect_.h) return;
        last_rect_ = r;
        int x = static_cast<int>(std::floor(r.x));
        int w = static_cast<int>(std::round(r.w));
        // Single-line EDIT controls draw text top-aligned within their HWND.
        // Size the HWND to the measured line height and centre it vertically
        // within the rect the caller allocated so text appears centred.
        int h = (line_h_ > 0) ? line_h_ : static_cast<int>(std::round(r.h));
        int y = static_cast<int>(std::floor(r.y)) +
                (static_cast<int>(std::round(r.h)) - h) / 2;
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void set_text(std::string text) override {
        if (!hwnd_) return;
        suppress_changed_ = true;
        std::wstring w = utf8_to_wide(text);
        SetWindowTextW(hwnd_, w.c_str());
        suppress_changed_ = false;
    }
    std::string text() const override {
        if (!hwnd_) return {};
        int len = GetWindowTextLengthW(hwnd_);
        if (len <= 0) return {};
        std::wstring w(len, L'\0');
        GetWindowTextW(hwnd_, w.data(), len + 1);
        return wide_to_utf8(w);
    }
    void set_placeholder(std::string text) override {
        if (!hwnd_) return;
        std::wstring w = utf8_to_wide(text);
        // EM_SETCUEBANNER lives in commctrl.h; available on XP SP1+.
        SendMessageW(hwnd_, EM_SETCUEBANNER, TRUE,
                      reinterpret_cast<LPARAM>(w.c_str()));
    }
    void set_focused(bool focused) override {
        if (hwnd_ && focused) SetFocus(hwnd_);
    }
    void set_visible(bool visible) override {
        if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
    void set_enabled(bool enabled) override {
        if (hwnd_) EnableWindow(hwnd_, enabled ? TRUE : FALSE);
    }
    void set_password(bool password) override {
        if (!hwnd_) return;
        // EM_SETPASSWORDCHAR is the universal toggle: setting '*' masks
        // the buffer, 0 clears the mask and shows plaintext.
        SendMessageW(hwnd_, EM_SETPASSWORDCHAR,
                      static_cast<WPARAM>(password ? L'•' : 0), 0);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override {
        on_submit_ = std::move(cb);
    }

    // Called by Surface's WndProc on WM_COMMAND with EN_CHANGE.
    void notify_changed() {
        if (suppress_changed_ || !on_changed_) return;
        on_changed_(text());
    }
    int  ctrl_id() const { return id_; }
    HWND hwnd()    const { return hwnd_; }

private:
    static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam,
                                           UINT_PTR /*id*/,
                                           DWORD_PTR ref) {
        auto* self = reinterpret_cast<Win32NativeTextField*>(ref);
        if (msg == WM_NCPAINT)
            return 0;
        if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
            if (self->on_submit_) self->on_submit_();
            return 0;
        }
        // Tell the parent we want all char input forwarded for Enter, so
        // the default beep on VK_RETURN doesn't fire.
        if (msg == WM_GETDLGCODE) {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            return r | DLGC_WANTALLKEYS;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    HWND parent_     = nullptr;
    HWND hwnd_       = nullptr;
    int  id_         = 0;
    int  line_h_     = 0;
    bool suppress_changed_ = false;
    Rect last_rect_  = {-1.f, -1.f, -1.f, -1.f};
    std::function<void(const std::string&)> on_changed_;
    std::function<void()>                   on_submit_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Win32NativeTextArea — multi-line EDIT control overlay
// ─────────────────────────────────────────────────────────────────────────
//
// Multi-line EDIT (ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL). The
// subclass swallows Enter (without Shift) and raises on_submit; Shift+
// Enter falls through and inserts a newline. Natural height comes from a
// CalcTextSize-ish measurement: line count × DT_HEIGHT_OF_FIRST_LINE.

class Win32NativeTextArea : public NativeTextArea {
public:
    Win32NativeTextArea(HWND parent, int ctrl_id, IWICImagingFactory* wic = nullptr)
        : parent_(parent), id_(ctrl_id), wic_(wic) {
        hwnd_ = CreateWindowExW(
            0,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE |
                ES_MULTILINE | ES_AUTOVSCROLL | ES_LEFT | ES_WANTRETURN,
            0, 0, 200, 40,
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_)),
            reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
            nullptr);
        if (!hwnd_) return;
        SendMessageW(hwnd_, WM_SETFONT,
                      reinterpret_cast<WPARAM>(body_font()), FALSE);
        SetWindowSubclass(hwnd_, &Win32NativeTextArea::subclass_proc,
                           1, reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(hwnd_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                      MAKELONG(8, 8));
    }

    ~Win32NativeTextArea() override {
        if (hwnd_) {
            RemoveWindowSubclass(hwnd_, &Win32NativeTextArea::subclass_proc, 1);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void set_rect(Rect r) override {
        if (!hwnd_) return;
        if (r.x == last_rect_.x && r.y == last_rect_.y &&
            r.w == last_rect_.w && r.h == last_rect_.h) return;
        last_rect_ = r;
        SetWindowPos(hwnd_, nullptr,
                      static_cast<int>(std::floor(r.x)),
                      static_cast<int>(std::floor(r.y)),
                      static_cast<int>(std::round(r.w)),
                      static_cast<int>(std::round(r.h)),
                      SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void set_text(std::string text) override {
        if (!hwnd_) return;
        suppress_changed_ = true;
        std::wstring w = utf8_to_wide(text);
        SetWindowTextW(hwnd_, w.c_str());
        suppress_changed_ = false;
        float h = natural_height();
        if (h != last_height_ && on_height_changed_) {
            last_height_ = h;
            on_height_changed_(h);
        }
    }
    std::string text() const override {
        if (!hwnd_) return {};
        int len = GetWindowTextLengthW(hwnd_);
        if (len <= 0) return {};
        std::wstring w(len, L'\0');
        GetWindowTextW(hwnd_, w.data(), len + 1);
        return wide_to_utf8(w);
    }
    void set_placeholder(std::string text) override {
        if (!hwnd_) return;
        std::wstring w = utf8_to_wide(text);
        SendMessageW(hwnd_, EM_SETCUEBANNER, TRUE,
                      reinterpret_cast<LPARAM>(w.c_str()));
    }
    void set_focused(bool focused) override {
        if (hwnd_ && focused) SetFocus(hwnd_);
    }
    void set_visible(bool visible) override {
        if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
    void set_enabled(bool enabled) override {
        if (hwnd_) EnableWindow(hwnd_, enabled ? TRUE : FALSE);
    }
    float natural_height() const override {
        if (!hwnd_) return 0.f;
        int line_count = static_cast<int>(SendMessageW(hwnd_,
                                            EM_GETLINECOUNT, 0, 0));
        if (line_count < 1) line_count = 1;
        // Measure one line through the EDIT control's own font.
        HDC hdc = GetDC(hwnd_);
        HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(hwnd_, WM_GETFONT, 0, 0));
        HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        if (old) SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        int per_line = tm.tmHeight + tm.tmExternalLeading;
        return static_cast<float>(per_line * line_count + 8);
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override {
        on_submit_ = std::move(cb);
    }
    void set_on_height_changed(std::function<void(float)> cb) override {
        on_height_changed_ = std::move(cb);
    }
    void set_on_image_paste(ImagePasteHandler cb) override {
        on_image_paste_ = std::move(cb);
    }

    void notify_changed() {
        if (suppress_changed_) return;
        std::string t = text();
        if (on_changed_) on_changed_(t);
        float h = natural_height();
        if (h != last_height_ && on_height_changed_) {
            last_height_ = h;
            on_height_changed_(h);
        }
    }
    int  ctrl_id() const { return id_; }
    HWND hwnd()    const { return hwnd_; }

private:
    static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam,
                                           UINT_PTR /*id*/,
                                           DWORD_PTR ref) {
        auto* self = reinterpret_cast<Win32NativeTextArea*>(ref);
        if (msg == WM_NCPAINT)
            return 0;
        if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!shift) {
                if (self->on_submit_) self->on_submit_();
                return 0;
            }
        }
        // Intercept Ctrl+V / Shift+Insert / right-click "Paste" before
        // EDIT inserts text. If clipboard holds a DIB and we have an
        // image-paste handler, route to it and skip the default.
        if (msg == WM_PASTE && self->on_image_paste_ && self->wic_) {
            if (IsClipboardFormatAvailable(CF_DIBV5) ||
                IsClipboardFormatAvailable(CF_DIB)) {
                std::vector<std::uint8_t> bytes;
                if (clipboard_image_to_png(self->wic_, hwnd, bytes)) {
                    self->on_image_paste_(std::move(bytes), "image/png");
                    return 0;
                }
            }
        }
        if (msg == WM_GETDLGCODE) {
            LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
            return r | DLGC_WANTALLKEYS;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    HWND parent_     = nullptr;
    HWND hwnd_       = nullptr;
    int  id_         = 0;
    bool  suppress_changed_ = false;
    float last_height_      = 0.f;
    Rect  last_rect_        = {-1.f, -1.f, -1.f, -1.f};
    IWICImagingFactory* wic_ = nullptr;
    std::function<void(const std::string&)>  on_changed_;
    std::function<void()>                    on_submit_;
    std::function<void(float)>               on_height_changed_;
    ImagePasteHandler                        on_image_paste_;
};

// Defined in audio_win32.cpp — wired here so Host::make_audio_player() can
// call it without a separate header (mirrors the qt6 / gtk / macos pattern).
std::unique_ptr<tk::AudioPlayer>
make_audio_player_win32(std::function<void(std::function<void()>)> post);

// Defined in video_win32.cpp.
std::unique_ptr<tk::VideoPlayer>
make_video_player_win32(std::function<void(std::function<void()>)> post,
                        tk::d2d::Backend* backend);

// ─────────────────────────────────────────────────────────────────────────
//  Host — owns the tree, paints into the d2d::Surface, dispatches input
// ─────────────────────────────────────────────────────────────────────────

class Host : public tk::Host {
public:
    Host(HWND hwnd, const Theme& theme)
        : hwnd_(hwnd),
          theme_(&theme),
          d2d_surface_(std::make_unique<d2d::Surface>(backend_singleton(), hwnd)),
          factory_(d2d::make_factory(backend_singleton())) {}

    void request_repaint() override {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void post_to_ui(std::function<void()> task) override {
        if (!hwnd_) return;
        auto* heap = new std::function<void()>(std::move(task));
        if (!PostMessageW(hwnd_, post_to_ui_message(), 0,
                           reinterpret_cast<LPARAM>(heap))) {
            // PostMessage failed; reclaim the heap copy so we don't leak.
            delete heap;
        }
    }

    std::unique_ptr<NativeTextField> make_text_field() override {
        int id = next_ctrl_id_++;
        auto field = std::make_unique<Win32NativeTextField>(hwnd_, id);
        fields_by_id_.emplace(id, field.get());
        return field;
    }

    std::unique_ptr<NativeTextArea> make_text_area() override {
        int id = next_ctrl_id_++;
        auto area = std::make_unique<Win32NativeTextArea>(
            hwnd_, id, d2d::factories(backend_singleton()).wic);
        areas_by_id_.emplace(id, area.get());
        return area;
    }

    std::unique_ptr<AudioPlayer> make_audio_player() override {
        return make_audio_player_win32(
            [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
    }
    std::unique_ptr<VideoPlayer> make_video_player() override {
        return make_video_player_win32(
            [this](std::function<void()> fn) { post_to_ui(std::move(fn)); },
            &backend_singleton());
    }

    EncodedImage encode_for_send(const std::uint8_t* data,
                                 std::size_t         len,
                                 bool                compress) override {
        EncodedImage out{};
        if (!data || len == 0) return out;

        using Microsoft::WRL::ComPtr;
        IWICImagingFactory* wic = d2d::factories(backend_singleton()).wic;
        if (!wic) return out;

        // Decode to inspect dimensions + source format.
        ComPtr<IWICStream> stream;
        if (FAILED(wic->CreateStream(stream.GetAddressOf()))) return out;
        if (FAILED(stream->InitializeFromMemory(
                const_cast<BYTE*>(data),
                static_cast<DWORD>(len)))) return out;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic->CreateDecoderFromStream(
                stream.Get(), nullptr,
                WICDecodeMetadataCacheOnLoad,
                decoder.GetAddressOf()))) return out;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return out;

        UINT src_w = 0, src_h = 0;
        frame->GetSize(&src_w, &src_h);

        if (!compress) {
            out.bytes.assign(data, data + len);
            GUID container = {};
            decoder->GetContainerFormat(&container);
            if (container == GUID_ContainerFormatPng)       out.mime = "image/png";
            else if (container == GUID_ContainerFormatJpeg) out.mime = "image/jpeg";
            else if (container == GUID_ContainerFormatGif)  out.mime = "image/gif";
            else if (container == GUID_ContainerFormatBmp)  out.mime = "image/bmp";
            else                                            out.mime = "image/png";
            out.width  = src_w;
            out.height = src_h;
            return out;
        }

        constexpr UINT kMaxW = 1600;
        constexpr UINT kMaxH = 1200;
        UINT dst_w = src_w, dst_h = src_h;
        if (src_w > kMaxW || src_h > kMaxH) {
            double s = std::min({1.0,
                                  static_cast<double>(kMaxW) / src_w,
                                  static_cast<double>(kMaxH) / src_h});
            dst_w = std::max<UINT>(1, static_cast<UINT>(std::round(src_w * s)));
            dst_h = std::max<UINT>(1, static_cast<UINT>(std::round(src_h * s)));
        }

        ComPtr<IWICBitmapSource> source;
        if (dst_w != src_w || dst_h != src_h) {
            ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())))
                return EncodedImage{};
            if (FAILED(scaler->Initialize(frame.Get(), dst_w, dst_h,
                                            WICBitmapInterpolationModeFant)))
                return EncodedImage{};
            source = scaler;
        } else {
            source = frame;
        }

        // Encode JPEG into an in-memory IStream.
        ComPtr<IStream> mem;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, mem.GetAddressOf())))
            return EncodedImage{};
        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                                        encoder.GetAddressOf())))
            return EncodedImage{};
        if (FAILED(encoder->Initialize(mem.Get(), WICBitmapEncoderNoCache)))
            return EncodedImage{};
        ComPtr<IWICBitmapFrameEncode> out_frame;
        ComPtr<IPropertyBag2>         props;
        if (FAILED(encoder->CreateNewFrame(out_frame.GetAddressOf(),
                                             props.GetAddressOf())))
            return EncodedImage{};
        // Quality 0.75.
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;  VariantInit(&v);
        v.vt = VT_R4; v.fltVal = 0.75f;
        props->Write(1, &opt, &v);
        VariantClear(&v);
        if (FAILED(out_frame->Initialize(props.Get()))) return EncodedImage{};
        if (FAILED(out_frame->SetSize(dst_w, dst_h)))   return EncodedImage{};
        if (FAILED(out_frame->WriteSource(source.Get(), nullptr)))
            return EncodedImage{};
        if (FAILED(out_frame->Commit())) return EncodedImage{};
        if (FAILED(encoder->Commit()))   return EncodedImage{};

        HGLOBAL h_out = nullptr;
        if (FAILED(GetHGlobalFromStream(mem.Get(), &h_out)) || !h_out)
            return EncodedImage{};
        SIZE_T n = GlobalSize(h_out);
        void* p = GlobalLock(h_out);
        if (!p || n == 0) { if (p) GlobalUnlock(h_out); return EncodedImage{}; }
        out.bytes.assign(static_cast<const std::uint8_t*>(p),
                          static_cast<const std::uint8_t*>(p) + n);
        GlobalUnlock(h_out);
        out.mime   = "image/jpeg";
        out.width  = dst_w;
        out.height = dst_h;
        return out;
    }

    // Look up the NativeTextField owning a child EDIT control by ID.
    Win32NativeTextField* field_by_id(int id) {
        auto it = fields_by_id_.find(id);
        return it == fields_by_id_.end() ? nullptr : it->second;
    }
    Win32NativeTextArea* area_by_id(int id) {
        auto it = areas_by_id_.find(id);
        return it == areas_by_id_.end() ? nullptr : it->second;
    }

    // ── Internal ──────────────────────────────────────────────────────
    void set_root(std::unique_ptr<Widget> root) {
        root_ = std::move(root);
        relayout();
    }
    Widget* root() const { return root_.get(); }
    const Theme& theme() const { return *theme_; }
    CanvasFactory& factory() { return *factory_; }
    HWND hwnd() const { return hwnd_; }

    void relayout() {
        if (!root_ || !hwnd_) return;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        LayoutCtx ctx{ *factory_, *theme_ };
        Rect bounds{ 0, 0,
                      static_cast<float>(rc.right  - rc.left),
                      static_cast<float>(rc.bottom - rc.top) };
        root_->measure(ctx, { bounds.w, bounds.h });
        root_->arrange(ctx, bounds);
        if (on_layout_) on_layout_();
        request_repaint();
    }

    void set_on_layout(std::function<void()> cb) {
        on_layout_ = std::move(cb);
    }

    void on_resize() {
        if (!hwnd_) return;
        RECT rc; GetClientRect(hwnd_, &rc);
        d2d_surface_->resize(rc.right - rc.left, rc.bottom - rc.top);
        relayout();
    }

    void on_paint() {
        if (!hwnd_) return;
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);

        Canvas& canvas = d2d_surface_->begin_paint();
        canvas.clear(theme_->palette.bg);
        if (root_) {
            PaintCtx ctx{ canvas, *factory_, *theme_ };
            root_->paint(ctx);
        }
        if (drag_active_) {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            float w = static_cast<float>(rc.right - rc.left);
            float h = static_cast<float>(rc.bottom - rc.top);
            const float inset = 8.0f;
            Rect area{ inset, inset,
                       std::max(0.0f, w - inset * 2),
                       std::max(0.0f, h - inset * 2) };
            if (area.w > 0 && area.h > 0) {
                Color accent = theme_->palette.accent;
                Color fill   = accent;  fill.a = 28;
                Color stroke = accent;  stroke.a = 192;
                canvas.fill_rounded_rect(area, 12.0f, fill);
                canvas.stroke_rounded_rect(area, 12.0f, stroke, 2.0f);
                TextStyle st{};
                st.role = FontRole::Title;
                auto layout = factory_->build_text("Drop to attach", st);
                if (layout) {
                    Size sz = layout->measure();
                    canvas.draw_text(*layout,
                        { area.x + (area.w - sz.w) * 0.5f,
                          area.y + (area.h - sz.h) * 0.5f },
                        accent);
                }
            }
        }
        bool lost = d2d_surface_->end_paint();
        EndPaint(hwnd_, &ps);
        if (lost) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void on_pointer_down(int x, int y) {
        if (!root_) return;
        SetCapture(hwnd_);
        Point local{ static_cast<float>(x), static_cast<float>(y) };
        pressed_widget_ = root_->dispatch_pointer_down(local);
        if (pressed_widget_) request_repaint();
    }

    void on_pointer_up(int x, int y) {
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (!pressed_widget_) return;
        Point world{ static_cast<float>(x), static_cast<float>(y) };
        Point ws = pressed_widget_->world_to_local(world);
        bool inside = (ws.x >= 0 && ws.y >= 0 &&
                        ws.x < pressed_widget_->bounds().w &&
                        ws.y < pressed_widget_->bounds().h);
        pressed_widget_->on_pointer_up(ws, inside);
        pressed_widget_ = nullptr;
        request_repaint();
    }

    void on_pointer_move(int x, int y) {
        if (!root_) return;
        Point local{ static_cast<float>(x), static_cast<float>(y) };
        if (pressed_widget_) {
            Point ws = pressed_widget_->world_to_local(local);
            pressed_widget_->on_pointer_drag(ws);
            request_repaint();
            return;
        }
        Widget* hit = root_->hit_test(local);
        Button* hovered = dynamic_cast<Button*>(hit);
        if (hovered != hovered_btn_) {
            if (hovered_btn_) hovered_btn_->set_hovered(false);
            hovered_btn_ = hovered;
            if (hovered_btn_) hovered_btn_->set_hovered(true);
        }
        Widget* moved = root_->dispatch_pointer_move(local);
        if (moved != hovered_widget_) {
            if (hovered_widget_) hovered_widget_->on_pointer_leave();
            hovered_widget_ = moved;
        }
        request_repaint();
    }

    void on_pointer_leave() {
        if (hovered_btn_) { hovered_btn_->set_hovered(false); hovered_btn_ = nullptr; }
        if (hovered_widget_) { hovered_widget_->on_pointer_leave(); hovered_widget_ = nullptr; }
        if (pressed_widget_) {
            pressed_widget_->on_pointer_up({-1, -1}, false);
            pressed_widget_ = nullptr;
        }
        request_repaint();
    }

    void on_wheel(int screen_x, int screen_y, int delta_steps) {
        if (!root_ || !hwnd_) return;
        POINT pt{ screen_x, screen_y };
        ScreenToClient(hwnd_, &pt);
        // WM_MOUSEWHEEL: positive WHEEL_DELTA = forward away from user.
        // The toolkit convention is positive dy = scroll content down,
        // so invert. One notch (120) maps to ~3 toolkit pixels per step.
        float dy = static_cast<float>(-delta_steps) * (3.0f / 120.0f) * 30.0f;
        if (root_->dispatch_wheel({ static_cast<float>(pt.x),
                                       static_cast<float>(pt.y) }, 0, dy)) {
            request_repaint();
        }
    }

    void detach() { hwnd_ = nullptr; }

private:
    HWND                                 hwnd_;
    const Theme*                         theme_;
    std::unique_ptr<d2d::Surface>        d2d_surface_;
    std::unique_ptr<CanvasFactory>       factory_;
    std::unique_ptr<Widget>              root_;
    std::function<void()>                on_layout_;
    Widget*                              pressed_widget_ = nullptr;
    Button*                              hovered_btn_    = nullptr;
    Widget*                              hovered_widget_ = nullptr;

    int                                  next_ctrl_id_ = 0x4000;
    std::unordered_map<int, Win32NativeTextField*> fields_by_id_;
    std::unordered_map<int, Win32NativeTextArea*>  areas_by_id_;

public:
    void set_on_file_drop(FileDropHandler cb) {
        on_file_drop_ = std::move(cb);
    }
    bool has_file_drop_handler() const {
        return static_cast<bool>(on_file_drop_);
    }
    void fire_file_drop(std::vector<std::uint8_t> bytes,
                         std::string               mime,
                         std::string               filename) {
        if (on_file_drop_)
            on_file_drop_(std::move(bytes), std::move(mime),
                            std::move(filename));
    }
    void set_on_right_click(std::function<void(tk::Point)> cb) {
        on_right_click_ = std::move(cb);
    }
    void fire_right_click(int x, int y) {
        if (on_right_click_)
            on_right_click_(tk::Point{ static_cast<float>(x),
                                       static_cast<float>(y) });
    }
    void set_drag_active(bool active) {
        if (drag_active_ == active) return;
        drag_active_ = active;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    bool drag_active() const { return drag_active_; }

private:
    FileDropHandler                      on_file_drop_;
    std::function<void(tk::Point)>       on_right_click_;
    bool                                 drag_active_ = false;
};

// ─────────────────────────────────────────────────────────────────────────
//  Surface — child HWND + window-class registration
// ─────────────────────────────────────────────────────────────────────────

namespace {

constexpr const wchar_t* kSurfaceClass = L"tk_win32_Surface";

LRESULT CALLBACK surface_wnd_proc(HWND hwnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam) {
    Host* host = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // The runtime-registered post_to_ui message — handled before any
    // standard switch so it isn't confused with normal Win32 messages.
    if (host && msg == post_to_ui_message()) {
        auto* fn = reinterpret_cast<std::function<void()>*>(lParam);
        if (fn) {
            if (*fn) (*fn)();
            delete fn;
        }
        return 0;
    }

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                               reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_MOUSEACTIVATE:
            // Prevent the Surface from stealing keyboard focus from native
            // overlays (NativeTextArea, NativeTextField) when buttons are
            // clicked. All input handling is mouse-driven; no WM_KEYDOWN
            // handling lives in this proc.
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;   // we paint the full client area in WM_PAINT
        case WM_PAINT:
            if (host) host->on_paint();
            else      ValidateRect(hwnd, nullptr);
            return 0;
        case WM_SIZE:
            if (host) host->on_resize();
            return 0;
        case WM_LBUTTONDOWN:
            if (host) host->on_pointer_down(GET_X_LPARAM(lParam),
                                              GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            if (host) host->on_pointer_up(GET_X_LPARAM(lParam),
                                            GET_Y_LPARAM(lParam));
            return 0;
        case WM_RBUTTONUP:
            if (host) host->fire_right_click(GET_X_LPARAM(lParam),
                                              GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE: {
            if (host) host->on_pointer_move(GET_X_LPARAM(lParam),
                                              GET_Y_LPARAM(lParam));
            // Subscribe to WM_MOUSELEAVE for hover-out tracking.
            TRACKMOUSEEVENT tme{};
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (host) host->on_pointer_leave();
            return 0;
        case WM_MOUSEWHEEL: {
            if (host) {
                short delta = static_cast<short>(HIWORD(wParam));
                // WM_MOUSEWHEEL coordinates are in screen pixels; the
                // host converts via ScreenToClient.
                host->on_wheel(GET_X_LPARAM(lParam),
                                GET_Y_LPARAM(lParam),
                                delta);
            }
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            // Paint EDIT-control backgrounds with the theme's input-card colour
            // instead of the system default white.
            if (host) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                const auto& pal = host->theme().palette;
                COLORREF bg = RGB(pal.compose_card_bg.r,
                                  pal.compose_card_bg.g,
                                  pal.compose_card_bg.b);
                SetBkColor(dc, bg);
                SetTextColor(dc, RGB(pal.text_primary.r,
                                     pal.text_primary.g,
                                     pal.text_primary.b));
                SetDCBrushColor(dc, bg);
                return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
            }
            break;
        }
        case WM_COMMAND: {
            // EN_CHANGE from a child EDIT belonging to one of our
            // NativeTextField overlays. wParam HIWORD = notification,
            // LOWORD = control id.
            int  id   = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            if (host && code == EN_CHANGE) {
                if (auto* f = host->field_by_id(id)) f->notify_changed();
                else if (auto* a = host->area_by_id(id)) a->notify_changed();
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

class DropTarget final : public IDropTarget {
public:
    explicit DropTarget(Host* host) : host_(host) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropTarget)) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,
                                         DWORD /*grfKeyState*/,
                                         POINTL /*pt*/,
                                         DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        accept_ = host_ && host_->has_file_drop_handler()
                && acceptable(data);
        *pdwEffect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accept_ && host_) host_->set_drag_active(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD /*grfKeyState*/,
                                        POINTL /*pt*/,
                                        DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        accept_ = false;
        if (host_) host_->set_drag_active(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,
                                    DWORD /*grfKeyState*/,
                                    POINTL /*pt*/,
                                    DWORD* pdwEffect) override {
        if (pdwEffect) *pdwEffect = DROPEFFECT_NONE;
        if (host_) host_->set_drag_active(false);
        if (!accept_ || !host_) return S_OK;

        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg{};
        if (FAILED(data->GetData(&fe, &stg))) return S_OK;
        HDROP hdrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
        bool dispatched = false;
        if (hdrop) {
            UINT n_files = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n_files; ++i) {
                UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring path(len, L'\0');
                if (DragQueryFileW(hdrop, i, path.data(), len + 1) == 0)
                    continue;
                if (try_dispatch_file(path)) dispatched = true;
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);

        if (dispatched && pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

    void detach_host() { host_ = nullptr; }

private:
    // Extension → MIME table used as a fallback when content-sniffing
    // via FindMimeFromData isn't conclusive. Covers the common chat
    // payloads; the rest fall back to application/octet-stream.
    static const char* mime_from_ext(const std::wstring& ext_lower) {
        if (ext_lower == L"png")                       return "image/png";
        if (ext_lower == L"jpg" || ext_lower == L"jpeg") return "image/jpeg";
        if (ext_lower == L"webp")                      return "image/webp";
        if (ext_lower == L"bmp")                       return "image/bmp";
        if (ext_lower == L"gif")                       return "image/gif";
        if (ext_lower == L"pdf")                       return "application/pdf";
        if (ext_lower == L"zip")                       return "application/zip";
        if (ext_lower == L"txt")                       return "text/plain";
        if (ext_lower == L"json")                      return "application/json";
        return nullptr;
    }

    static std::wstring path_extension_lower(const std::wstring& p) {
        size_t slash = p.find_last_of(L"\\/");
        size_t dot   = p.find_last_of(L'.');
        if (dot == std::wstring::npos ||
            (slash != std::wstring::npos && dot < slash)) return {};
        std::wstring ext = p.substr(dot + 1);
        for (wchar_t& c : ext) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
        }
        return ext;
    }

    static std::wstring basename(const std::wstring& p) {
        size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? p : p.substr(slash + 1);
    }

    // Returns true when the drop carries at least one local file.
    static bool acceptable(IDataObject* data) {
        if (!data) return false;
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return data->QueryGetData(&fe) == S_OK;
    }

    bool try_dispatch_file(const std::wstring& path) {
        if (!host_) return false;

        // Size guard via GetFileAttributesEx — single syscall, no open.
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
            return false;
        ULARGE_INTEGER sz{};
        sz.LowPart  = fa.nFileSizeLow;
        sz.HighPart = fa.nFileSizeHigh;
        if (sz.QuadPart == 0 || sz.QuadPart > kMaxDroppedFileBytes)
            return false;

        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        std::vector<std::uint8_t> bytes(static_cast<size_t>(sz.QuadPart));
        DWORD read_total = 0;
        while (read_total < bytes.size()) {
            DWORD got = 0;
            BOOL  ok  = ReadFile(h, bytes.data() + read_total,
                                  static_cast<DWORD>(bytes.size() - read_total),
                                  &got, nullptr);
            if (!ok || got == 0) break;
            read_total += got;
        }
        CloseHandle(h);
        if (read_total != bytes.size()) return false;

        // Mime: extension table first (cheap), default to
        // application/octet-stream when unknown. FindMimeFromData would
        // need urlmon.lib; the table covers the common chat payloads.
        std::string mime = "application/octet-stream";
        if (const char* m = mime_from_ext(path_extension_lower(path))) {
            mime = m;
        }

        host_->fire_file_drop(std::move(bytes), std::move(mime),
                              wide_to_utf8(basename(path)));
        return true;
    }

    Host*               host_;
    std::atomic<ULONG>  refs_{1};
    bool                accept_ = false;
};

bool ensure_class_registered(HINSTANCE inst) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &surface_wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;       // we paint everything
    wc.lpszClassName = kSurfaceClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    registered = true;
    return true;
}

} // namespace

// One IDropTarget per Surface, indexed by hwnd so the dtor can find it
// at teardown. Keeping it out of Host avoids forward-declaration churn
// (DropTarget is defined later in this TU than Host).
namespace {
std::unordered_map<HWND, DropTarget*>& drop_targets_by_hwnd() {
    static std::unordered_map<HWND, DropTarget*> instance;
    return instance;
}
} // namespace

Surface::Surface(HINSTANCE inst, HWND parent, const Theme& theme) {
    if (!ensure_class_registered(inst)) return;

    HWND hwnd = CreateWindowExW(
        0,
        kSurfaceClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        parent, nullptr, inst,
        /*lpCreateParams=*/nullptr);
    if (!hwnd) return;

    host_ = std::make_unique<Host>(hwnd, theme);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                       reinterpret_cast<LONG_PTR>(host_.get()));

    // Register an OLE drop target. The handler isn't wired yet — the
    // target stays a no-op until the shell calls set_on_image_drop.
    // RegisterDragDrop fails silently when the caller hasn't OleInitialize'd
    // their thread; the shell is responsible for that (main.cpp).
    auto* dt = new DropTarget(host_.get());
    if (SUCCEEDED(RegisterDragDrop(hwnd, dt))) {
        drop_targets_by_hwnd().emplace(hwnd, dt);
    } else {
        dt->Release();
    }
}

Surface::~Surface() {
    if (host_ && host_->hwnd()) {
        HWND hwnd = host_->hwnd();
        auto& map = drop_targets_by_hwnd();
        auto it = map.find(hwnd);
        if (it != map.end()) {
            RevokeDragDrop(hwnd);
            it->second->detach_host();
            it->second->Release();
            map.erase(it);
        }
        host_->detach();
        DestroyWindow(hwnd);
    }
}

HWND Surface::hwnd() const {
    return host_ ? host_->hwnd() : nullptr;
}

tk::Host& Surface::host() { return *host_; }
const Theme& Surface::theme() const { return host_->theme(); }

void Surface::set_root(std::unique_ptr<Widget> root) {
    host_->set_root(std::move(root));
}

Widget* Surface::root() const { return host_->root(); }

void Surface::relayout() { host_->relayout(); }

void Surface::set_on_layout(std::function<void()> cb) {
    host_->set_on_layout(std::move(cb));
}

CanvasFactory& Surface::factory() { return host_->factory(); }

void Surface::set_on_file_drop(FileDropHandler cb) {
    host_->set_on_file_drop(std::move(cb));
}

void Surface::set_on_right_click(std::function<void(tk::Point)> cb) {
    host_->set_on_right_click(std::move(cb));
}

std::vector<tk::d2d::AnimatedFrame> decode_animation(
    std::span<const std::uint8_t> bytes)
{
    return tk::d2d::decode_animation(backend_singleton(), bytes);
}

} // namespace tk::win32
