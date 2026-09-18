#include "AuxWindow.h"
#include "MainWindow.h"
#include "Theme.h"
#include "resource.h"

namespace win32
{

bool AuxWindow::class_registered_ = false;

AuxWindow::AuxWindow(MainWindow* /*parent_shell*/, const std::string& title,
                     std::unique_ptr<tk::Widget> root, int width, int height,
                     const tk::Theme& theme)
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

    const std::wstring wtitle = tk::win32::utf8_to_wide(title);
    hwnd_ = CreateWindowExW(
        0, kClassName, wtitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        width, height,
        nullptr, nullptr, hInst, this);

    if (!hwnd_)
        return;

    surface_ = std::make_unique<tk::win32::Surface>(hInst, hwnd_, theme);
    surface_->set_root(std::move(root));

    ShowWindow(hwnd_, SW_SHOW);
}

AuxWindow::~AuxWindow()
{
    if (hwnd_)
        DestroyWindow(hwnd_);
}

void AuxWindow::bring_to_front()
{
    if (hwnd_)
    {
        if (IsIconic(hwnd_))
            ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
    }
}

void AuxWindow::close_window()
{
    if (hwnd_)
        DestroyWindow(hwnd_);
}

void AuxWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->root()->apply_theme(t);
    }
}

void AuxWindow::apply_scale_change(float scale)
{
    if (surface_)
        surface_->apply_scale_change(scale);
}

void AuxWindow::request_relayout()
{
    if (surface_)
        surface_->relayout();
}

void AuxWindow::request_repaint()
{
    if (surface_)
        surface_->host().request_repaint();
}

// static
LRESULT CALLBACK AuxWindow::wnd_proc_(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AuxWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self     = static_cast<AuxWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<AuxWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handle_msg_(hwnd, msg, wParam, lParam);
}

LRESULT AuxWindow::handle_msg_(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DPICHANGED:
    {
        // Independent top-level window: it gets its own WM_DPICHANGED (see
        // CallWindow::handle_msg_).
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
                // Same guard as CallWindow: SetWindowPos already relayouts via
                // the child's own WM_SIZE when the size actually changes.
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
