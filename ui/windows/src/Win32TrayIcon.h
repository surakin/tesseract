#pragma once
#include <tesseract/tray_icon.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <functional>
#include <string>

namespace win32
{

// Custom Shell_NotifyIcon callback message. We route it to a hidden helper
// HWND owned by Win32TrayIcon so the main window's wnd_proc switch stays
// clean (and so the tray can outlive the main window during shutdown).
constexpr UINT WM_TESSERACT_TRAY = WM_APP + 20;

class Win32TrayIcon final : public tesseract::ITrayIcon
{
public:
    Win32TrayIcon(HINSTANCE hInst, std::function<void()> on_show,
                  std::function<void()> on_toggle,
                  std::function<void()> on_quit);
    ~Win32TrayIcon() override;

    bool is_available() const override
    {
        return added_;
    }
    void set_tooltip(const std::string& text) override;

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    void on_tray_message(WPARAM, LPARAM);
    void show_menu();

    static constexpr int kMenuShowId = 1001;
    static constexpr int kMenuQuitId = 1002;
    static constexpr UINT kIconId = 1;
    static constexpr const wchar_t* CLASS_NAME = L"TesseractTrayWnd";
    static bool class_registered_;

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON hIcon_ = nullptr;
    std::function<void()> on_show_;
    std::function<void()> on_toggle_;
    std::function<void()> on_quit_;
    bool added_ = false;
};

} // namespace win32
