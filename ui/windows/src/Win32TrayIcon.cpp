#include "Win32TrayIcon.h"
#include "resource.h"

#include <shellapi.h>
#include <ole2.h>   // IStream + DEFINE_GUID — required before gdiplus.h
#include <algorithm>
#include <cwchar>
#include <utility>
#include <vector>

// GdiplusTypes.h calls unqualified min()/max(); with NOMINMAX the Win32 macros
// are gone, so std::min/std::max must be brought into scope before the include.
// Mirrors the trick in MainWindow.h.
using std::max;
using std::min;
#include <gdiplus.h>

namespace win32
{

bool Win32TrayIcon::class_registered_ = false;

namespace
{

// Convert a UTF-8 string to a wide string capped at `max_chars` wchar_t units.
std::wstring widen_capped(const std::string& s, std::size_t max_chars)
{
    if (s.empty())
    {
        return {};
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                     static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0)
    {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), needed);
    if (out.size() > max_chars)
    {
        out.resize(max_chars);
    }
    return out;
}

} // namespace

LRESULT CALLBACK Win32TrayIcon::wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam)
{
    auto* self = reinterpret_cast<Win32TrayIcon*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_TESSERACT_TRAY:
        if (self)
        {
            self->on_tray_message(wParam, lParam);
        }
        return 0;

    case WM_COMMAND:
        if (!self)
        {
            break;
        }
        switch (LOWORD(wParam))
        {
        case kMenuShowId:
            if (self->on_show_)
            {
                self->on_show_();
            }
            return 0;
        case kMenuQuitId:
            if (self->on_quit_)
            {
                self->on_quit_();
            }
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

Win32TrayIcon::Win32TrayIcon(HINSTANCE hInst, std::function<void()> on_show,
                             std::function<void()> on_toggle,
                             std::function<void()> on_quit)
    : hInst_(hInst), on_show_(std::move(on_show)),
      on_toggle_(std::move(on_toggle)), on_quit_(std::move(on_quit))
{
    if (!class_registered_)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Win32TrayIcon::wnd_proc;
        wc.hInstance = hInst_;
        wc.lpszClassName = CLASS_NAME;
        if (!RegisterClassExW(&wc) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return;
        }
        class_registered_ = true;
    }

    // Message-only window — invisible, no taskbar entry, just a wnd_proc to
    // receive Shell_NotifyIcon callbacks and the popup menu's WM_COMMAND.
    hwnd_ = CreateWindowExW(0, CLASS_NAME, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, hInst_, nullptr);
    if (!hwnd_)
    {
        return;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TESSERACT_TRAY;
    hIcon_ = static_cast<HICON>(
        LoadImageW(hInst_, MAKEINTRESOURCEW(IDI_TESSERACT), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                   LR_DEFAULTCOLOR));
    if (!hIcon_)
    {
        hIcon_ = LoadIconW(nullptr, IDI_APPLICATION);
    }
    nid.hIcon = hIcon_;
    // szTip is a fixed wchar_t[128]. Manual bounded copy keeps us portable
    // across MSVC and MinGW (the wcsncpy_s array overload is MSVC-only).
    {
        const wchar_t* t = L"Tesseract";
        std::size_t n = wcslen(t);
        if (n >= sizeof(nid.szTip) / sizeof(nid.szTip[0]))
        {
            n = sizeof(nid.szTip) / sizeof(nid.szTip[0]) - 1;
        }
        std::copy(t, t + n, nid.szTip);
        nid.szTip[n] = L'\0';
    }

    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

Win32TrayIcon::~Win32TrayIcon()
{
    if (added_ && hwnd_)
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (displayed_overlay_)
    {
        DestroyIcon(displayed_overlay_);
        displayed_overlay_ = nullptr;
    }
    if (hIcon_)
    {
        DestroyIcon(hIcon_);
        hIcon_ = nullptr;
    }
}

void Win32TrayIcon::set_tooltip(const std::string& text)
{
    if (!added_)
    {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_TIP;
    // szTip is wchar_t[128]; leave room for NUL.
    constexpr std::size_t kTipMax = sizeof(nid.szTip) / sizeof(nid.szTip[0]);
    std::wstring w = widen_capped(text, kTipMax - 1);
    std::copy(w.begin(), w.end(), nid.szTip);
    nid.szTip[w.size()] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Win32TrayIcon::on_tray_message(WPARAM /*icon_id*/, LPARAM lParam)
{
    const UINT event = LOWORD(lParam);
    switch (event)
    {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        if (on_toggle_)
        {
            on_toggle_();
        }
        return;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        show_menu();
        return;
    }
}

HICON Win32TrayIcon::make_overlay_icon_(UINT32 dot_color_argb) const
{
    if (!hIcon_)
    {
        return nullptr;
    }
    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);
    if (cx <= 0 || cy <= 0)
    {
        return nullptr;
    }

    // 32-bpp top-down DIB so the colour bitmap carries premultiplied alpha
    // straight into CreateIconIndirect.
    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = cx;
    bi.bV5Height      = -cy; // negative → top-down
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    HDC screen_dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen_dc,
                                     reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen_dc);
    if (!color || !bits)
    {
        if (color)
        {
            DeleteObject(color);
        }
        return nullptr;
    }

    HDC mem_dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old_bmp = SelectObject(mem_dc, color);

    // Draw the base icon into the DIB. DrawIconEx honours per-pixel alpha
    // for 32-bpp icon resources.
    DrawIconEx(mem_dc, 0, 0, hIcon_, cx, cy, 0, nullptr, DI_NORMAL);

    if (dot_color_argb != 0)
    {
        // Antialiased ellipse via GDI+ on top of the DIB. GDI+ is initialised
        // by MainWindow (which owns the tray), so it's safe to use here.
        Gdiplus::Graphics g(mem_dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        const int side  = std::min(cx, cy);
        const int dot   = std::max(6, side * 38 / 100);
        const int inset = std::max(1, side / 32);
        const int x     = cx - dot - inset;
        const int y     = cy - dot - inset;

        Gdiplus::Color fill(dot_color_argb);
        Gdiplus::SolidBrush brush(fill);
        g.FillEllipse(&brush, x, y, dot, dot);

        // White outline so the dot reads on both light and dark trays.
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255),
                         static_cast<Gdiplus::REAL>(std::max(1, side / 32)));
        g.DrawEllipse(&pen, x, y, dot, dot);
    }

    SelectObject(mem_dc, old_bmp);
    DeleteDC(mem_dc);

    // CreateIconIndirect requires a mask bitmap even for ARGB icons; a 1bpp
    // all-black mask preserves the colour bitmap's per-pixel alpha. Pass an
    // explicitly zero-initialised bit buffer instead of nullptr — MSDN leaves
    // the contents undefined when `bits` is null, and modern Windows ignores
    // the mask on 32-bpp colour bitmaps but legacy / non-standard compositors
    // may not.
    const SIZE_T mask_stride = ((cx + 15) / 16) * 2; // 1bpp scanlines are WORD-aligned
    std::vector<BYTE> mask_bits(mask_stride * cy, 0);
    HBITMAP mask =
        CreateBitmap(cx, cy, 1, 1, mask_bits.data());
    if (!mask)
    {
        DeleteObject(color);
        return nullptr;
    }

    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = color;
    HICON ico = CreateIconIndirect(&ii);

    DeleteObject(mask);
    DeleteObject(color);
    return ico;
}

void Win32TrayIcon::set_unread(bool has_unread, bool has_highlight)
{
    if (!added_)
    {
        return;
    }

    UINT32 dot = 0;
    if (has_highlight)
    {
        // 0xFFD93636 — destructive/red from the light palette.
        dot = 0xFFD93636;
    }
    else if (has_unread)
    {
        // 0xFF0084FF — accent/blue from the light palette.
        dot = 0xFF0084FF;
    }

    HICON new_icon = nullptr;
    if (dot != 0)
    {
        new_icon = make_overlay_icon_(dot);
        if (!new_icon)
        {
            return; // leave the previous icon untouched on failure
        }
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = kIconId;
    nid.uFlags = NIF_ICON;
    nid.hIcon  = new_icon ? new_icon : hIcon_;
    if (Shell_NotifyIconW(NIM_MODIFY, &nid) == FALSE)
    {
        if (new_icon)
        {
            DestroyIcon(new_icon);
        }
        return;
    }

    // Replace any previously-installed overlay only after a successful swap.
    if (displayed_overlay_)
    {
        DestroyIcon(displayed_overlay_);
    }
    displayed_overlay_ = new_icon; // may be nullptr (back to plain base)
}

void Win32TrayIcon::show_menu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }
    AppendMenuW(menu, MF_STRING, kMenuShowId, L"Show App");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuitId, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    // Shell_NotifyIcon footgun: the menu sticks around after a click outside
    // unless the window is foregrounded first.
    SetForegroundWindow(hwnd_);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                     pt.x, pt.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0); // dismiss-fix per MSDN
    DestroyMenu(menu);
}

} // namespace win32
