#include "Win32ScreenLock.h"

#include <wtsapi32.h>

namespace win32 {

namespace {
constexpr const wchar_t* kClass = L"TesseractScreenLockSink";
}

LRESULT CALLBACK Win32ScreenLock::wnd_proc_(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<Win32ScreenLock*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == WM_WTSSESSION_CHANGE) {
        if (wParam == WTS_SESSION_LOCK)
            self->locked_.store(true,  std::memory_order_relaxed);
        else if (wParam == WTS_SESSION_UNLOCK)
            self->locked_.store(false, std::memory_order_relaxed);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

Win32ScreenLock::Win32ScreenLock(HINSTANCE inst)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &Win32ScreenLock::wnd_proc_;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // harmless if already registered

    hwnd_ = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, inst, this);
    if (hwnd_)
        registered_ = WTSRegisterSessionNotification(
                          hwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE;
}

Win32ScreenLock::~Win32ScreenLock()
{
    if (hwnd_) {
        if (registered_) WTSUnRegisterSessionNotification(hwnd_);
        DestroyWindow(hwnd_);
    }
}

} // namespace win32
