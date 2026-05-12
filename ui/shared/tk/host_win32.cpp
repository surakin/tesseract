#include "host_win32.h"
#include "canvas_d2d.h"
#include "controls.h"

#include <commctrl.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

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
            WS_EX_CLIENTEDGE,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 100, 24,
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_)),
            reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
            nullptr);
        if (!hwnd_) return;
        SetWindowSubclass(hwnd_, &Win32NativeTextField::subclass_proc,
                           1, reinterpret_cast<DWORD_PTR>(this));
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
    bool suppress_changed_ = false;
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
    Win32NativeTextArea(HWND parent, int ctrl_id)
        : parent_(parent), id_(ctrl_id) {
        hwnd_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                ES_MULTILINE | ES_AUTOVSCROLL | ES_LEFT | ES_WANTRETURN,
            0, 0, 200, 40,
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_)),
            reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
            nullptr);
        if (!hwnd_) return;
        SetWindowSubclass(hwnd_, &Win32NativeTextArea::subclass_proc,
                           1, reinterpret_cast<DWORD_PTR>(this));
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
        if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!shift) {
                if (self->on_submit_) self->on_submit_();
                return 0;
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
    std::function<void(const std::string&)>  on_changed_;
    std::function<void()>                    on_submit_;
    std::function<void(float)>               on_height_changed_;
};

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
        auto area = std::make_unique<Win32NativeTextArea>(hwnd_, id);
        areas_by_id_.emplace(id, area.get());
        return area;
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

Surface::Surface(HINSTANCE inst, HWND parent, const Theme& theme) {
    if (!ensure_class_registered(inst)) return;

    HWND hwnd = CreateWindowExW(
        0,
        kSurfaceClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 100, 100,
        parent, nullptr, inst,
        /*lpCreateParams=*/nullptr);
    if (!hwnd) return;

    host_ = std::make_unique<Host>(hwnd, theme);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                       reinterpret_cast<LONG_PTR>(host_.get()));
}

Surface::~Surface() {
    if (host_ && host_->hwnd()) {
        HWND hwnd = host_->hwnd();
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

} // namespace tk::win32
