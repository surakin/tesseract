#include "Win32PickerPopup.h"

#include <algorithm>
#include <cmath>

namespace win32
{

Win32PickerPopup::Win32PickerPopup(RootFactory make_root, Config cfg)
    : cfg_(std::move(cfg))
{
    register_class_once_();

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cfg_.class_name,
                            L"", WS_POPUP, 0, 0, dip_to_phys_(cfg_.width_dip),
                            dip_to_phys_(cfg_.height_dip), cfg_.owner, nullptr,
                            cfg_.inst, nullptr);
    if (!hwnd_)
    {
        return;
    }

    surface_ = std::make_unique<tk::win32::Surface>(cfg_.inst, hwnd_, cfg_.theme);
    // The picker builds its own self-owned search field, which needs a live
    // Host& — only available now that surface_ exists.
    surface_->set_root(make_root(surface_->host()));

    if (HWND s = surface_->hwnd())
    {
        SetWindowPos(s, nullptr, 0, 0, dip_to_phys_(cfg_.width_dip),
                     dip_to_phys_(cfg_.height_dip),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

Win32PickerPopup::~Win32PickerPopup()
{
    surface_.reset();
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Win32PickerPopup::register_class_once_()
{
    WNDCLASSEXW wc{};
    if (GetClassInfoExW(cfg_.inst, cfg_.class_name, &wc))
    {
        return; // already registered (shared across all instances of this kind)
    }
    wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = cfg_.inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // tk::win32::Surface paints the body
    wc.lpszClassName = cfg_.class_name;
    RegisterClassExW(&wc);
}

LONG Win32PickerPopup::dip_to_phys_(float dip) const
{
    const float dpi =
        static_cast<float>(GetDpiForWindow(hwnd_ ? hwnd_ : cfg_.owner));
    const float scale = dpi > 0.f ? dpi / 96.f : 1.f;
    return static_cast<LONG>(std::round(dip * scale));
}

bool Win32PickerPopup::visible() const
{
    return hwnd_ && IsWindowVisible(hwnd_);
}

void Win32PickerPopup::hide()
{
    if (hwnd_)
    {
        ShowWindow(hwnd_, SW_HIDE);
    }
    if (cfg_.on_hide)
    {
        cfg_.on_hide();
    }
}

void Win32PickerPopup::repaint()
{
    if (visible() && surface_)
    {
        InvalidateRect(surface_->hwnd(), nullptr, FALSE);
    }
}

void Win32PickerPopup::set_theme(const tk::Theme& t)
{
    cfg_.theme = t;
    if (surface_)
    {
        surface_->set_theme(t);
    }
}

void Win32PickerPopup::toggle_at(HWND anchor_hwnd, tk::Rect anchor_rect)
{
    if (visible())
    {
        hide();
        return;
    }
    show_at(anchor_hwnd, anchor_rect);
}

void Win32PickerPopup::show_at(HWND anchor_hwnd, tk::Rect anchor_rect)
{
    if (!hwnd_ || !anchor_hwnd)
    {
        return;
    }

    // Map the local DIP rect into screen pixels.
    POINT pt{dip_to_phys_(anchor_rect.x), dip_to_phys_(anchor_rect.y)};
    ClientToScreen(anchor_hwnd, &pt);
    const LONG rectW = dip_to_phys_(anchor_rect.w);
    const LONG rectH = dip_to_phys_(anchor_rect.h);

    const int pickerW = dip_to_phys_(cfg_.width_dip);
    const int pickerH = dip_to_phys_(cfg_.height_dip);

    // Prefer above, centered on the rect; fall back below; clamp to work area.
    int x = pt.x + rectW / 2 - pickerW / 2;
    int y = pt.y - pickerH - 4;
    POINT ptCenter{pt.x + rectW / 2, pt.y + rectH / 2};
    HMONITOR mon = MonitorFromPoint(ptCenter, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi))
    {
        if (y < mi.rcWork.top)
        {
            y = pt.y + rectH + 4;
        }
        if (x + pickerW > mi.rcWork.right)
        {
            x = mi.rcWork.right - pickerW - 4;
        }
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + 4;
        }
        if (y + pickerH > mi.rcWork.bottom)
        {
            y = mi.rcWork.bottom - pickerH - 4;
        }
    }

    if (cfg_.on_before_show)
    {
        cfg_.on_before_show();
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, pickerW, pickerH, SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (surface_)
    {
        surface_->relayout();
    }
    if (cfg_.on_shown)
    {
        cfg_.on_shown();
    }
}

} // namespace win32
