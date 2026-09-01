#include "CallWindow.h"
#include "MainWindow.h"
#include "Theme.h"
#include "Win32Taskbar.h"
#include "resource.h"

#include "views/CallOverlayWidget.h"

#include <cmath>

namespace win32
{

bool CallWindow::class_registered_ = false;

// ---------------------------------------------------------------------------

CallWindow::CallWindow(MainWindow* parent_shell)
    : tesseract::CallWindowBase(parent_shell)
    , parent_shell_(parent_shell)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!class_registered_)
    {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = wnd_proc_;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        wc.hIcon = static_cast<HICON>(
            LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                       LR_DEFAULTCOLOR | LR_SHARED));
        wc.hIconSm = static_cast<HICON>(
            LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                       LR_DEFAULTCOLOR | LR_SHARED));
        if (!wc.hIcon)
            wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        if (!wc.hIconSm)
            wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);
        class_registered_ = true;
    }

    hwnd_ = CreateWindowExW(
        0, kClassName,
        L"Call",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        640, 480,
        nullptr, nullptr, hInst, this);

    if (!hwnd_)
        return;

    surface_ = std::make_unique<tk::win32::Surface>(hInst, hwnd_, tk::Theme::light());

    auto overlay = std::make_unique<tesseract::views::CallOverlayWidget>();
    call_overlay_widget_ = overlay.get();
    surface_->set_root(std::move(overlay));

    auto update_taskbar = [this]
    {
        if (!parent_shell_ || !call_overlay_widget_ || !hwnd_) return;
        const auto state = call_overlay_widget_->snapshot();
        parent_shell_->taskbar().set_call_state(
            hwnd_, state.audio_muted, state.video_muted,
            state.show_video_button);
    };
    call_overlay_widget_->on_controls_changed = update_taskbar;
    parent_shell_->taskbar().register_call_window(
        hwnd_,
        [this] { if (call_overlay_widget_) call_overlay_widget_->toggle_audio(); },
        [this] { if (call_overlay_widget_) call_overlay_widget_->toggle_video(); },
        [this] { if (call_overlay_widget_) call_overlay_widget_->hang_up(); });
    update_taskbar();

    ShowWindow(hwnd_, SW_SHOW);
}

CallWindow::~CallWindow()
{
    if (hwnd_)
        DestroyWindow(hwnd_);
}

// ---------------------------------------------------------------------------

void CallWindow::bring_to_front()
{
    if (hwnd_)
    {
        if (IsIconic(hwnd_))
            ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
    }
}

void CallWindow::close_window()
{
    if (hwnd_)
        DestroyWindow(hwnd_);
}

void CallWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->root()->apply_theme(t);
    }
}

void CallWindow::apply_scale_change(float scale)
{
    if (surface_)
        surface_->apply_scale_change(scale);
}

void CallWindow::request_relayout()
{
    if (surface_)
        surface_->relayout();
}

void CallWindow::request_repaint()
{
    if (surface_)
        surface_->host().request_repaint();
}

// ---------------------------------------------------------------------------
// Window procedure

// static
LRESULT CALLBACK CallWindow::wnd_proc_(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam)
{
    if (msg == WM_GETMINMAXINFO)
    {
        // Enforce the call window's own floor — can fire before
        // WM_NCCREATE sets GWLP_USERDATA (see RoomWindow::wnd_proc_).
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hwnd);
        const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.f : 1.f;
        mmi->ptMinTrackSize.x = static_cast<LONG>(
            std::round(tesseract::visual::kMinCallWindowWidth * scale));
        mmi->ptMinTrackSize.y = static_cast<LONG>(
            std::round(tesseract::visual::kMinCallWindowHeight * scale));
        return 0;
    }

    CallWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self     = static_cast<CallWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<CallWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handle_msg_(hwnd, msg, wParam, lParam);
}

LRESULT CallWindow::handle_msg_(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (parent_shell_ &&
        msg == parent_shell_->taskbar().taskbar_button_created_message())
    {
        parent_shell_->taskbar().on_taskbar_button_created(hwnd);
        return 0;
    }
    switch (msg)
    {
    case WM_COMMAND:
        if (parent_shell_ &&
            parent_shell_->taskbar().handle_thumbnail_command(hwnd, wParam))
            return 0;
        break;

    case WM_DPICHANGED:
    {
        // Mirrors RoomWindow::handle_msg_'s identical case — this is its
        // own independent top-level window (see the class-registration
        // block above), so it gets its own WM_DPICHANGED and must react to
        // it directly rather than relying on the main shell to propagate
        // one (which only knows its own monitor's scale). No case existed
        // here before — this window previously didn't react to a DPI
        // change at all.
        theme::on_dpi_changed();
        const RECT* rc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        apply_scale_change(static_cast<float>(LOWORD(wParam)) / 96.0f);
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && surface_)
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (HWND sh = surface_->hwnd())
            {
                // See RoomWindow's WM_SIZE handler for why this is guarded:
                // SetWindowPos() already triggers a full relayout via the
                // child's own WM_SIZE whenever its size actually changes,
                // so an unconditional relayout() here would redo that work
                // a second time on every tick of a live resize drag.
                RECT before{};
                GetClientRect(sh, &before);
                const bool needs_relayout =
                    before.right == rc.right && before.bottom == rc.bottom;
                SetWindowPos(sh, nullptr, 0, 0, rc.right, rc.bottom,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                if (needs_relayout) surface_->relayout();
            }
        }
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // surface child covers the entire client area

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (parent_shell_) parent_shell_->taskbar().unregister_window(hwnd);
        hwnd_ = nullptr;
        if (on_window_closed)
            on_window_closed();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace win32
