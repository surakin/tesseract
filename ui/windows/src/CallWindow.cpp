#ifdef TESSERACT_CALLS_ENABLED
#include "CallWindow.h"
#include "MainWindow.h"
#include "resource.h"

#include "views/CallOverlayWidget.h"

namespace win32
{

bool CallWindow::class_registered_ = false;

// ---------------------------------------------------------------------------

CallWindow::CallWindow(MainWindow* parent_shell)
    : tesseract::CallWindowBase(parent_shell)
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
        surface_->set_theme(t);
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
    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && surface_)
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (HWND sh = surface_->hwnd())
            {
                SetWindowPos(sh, nullptr, 0, 0, rc.right, rc.bottom,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            surface_->relayout();
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
#endif // TESSERACT_CALLS_ENABLED
