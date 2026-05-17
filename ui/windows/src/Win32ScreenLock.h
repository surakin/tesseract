#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tesseract/screen_lock.h>
#include <atomic>

namespace win32 {

// IScreenLock impl backed by WTS session notifications. Owns a hidden
// message-only window; WTSRegisterSessionNotification routes
// WM_WTSSESSION_CHANGE (WTS_SESSION_LOCK / WTS_SESSION_UNLOCK) to it and
// the locked state is cached in an atomic so is_locked() is a cheap read
// on the notification path. Best-effort: if registration fails the app is
// clearly in interactive use, so it reports "unlocked".
class Win32ScreenLock final : public tesseract::IScreenLock {
public:
    explicit Win32ScreenLock(HINSTANCE inst);
    ~Win32ScreenLock() override;

    Win32ScreenLock(const Win32ScreenLock&)            = delete;
    Win32ScreenLock& operator=(const Win32ScreenLock&) = delete;

    bool is_locked() const override {
        return locked_.load(std::memory_order_relaxed);
    }

private:
    static LRESULT CALLBACK wnd_proc_(HWND, UINT, WPARAM, LPARAM);

    HWND              hwnd_       = nullptr;
    bool              registered_ = false;
    std::atomic<bool> locked_{ false };
};

} // namespace win32
