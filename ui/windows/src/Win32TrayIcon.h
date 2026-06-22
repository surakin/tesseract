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
#include <vector>

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
    void set_unread(bool has_unread, bool has_highlight) override;

    // Rebuild the per-window items shown before the Quit action.
    // `window_items` is a list of (UTF-8 label, callback) pairs.
    void rebuild_menu(
        std::vector<std::pair<std::string, std::function<void()>>> window_items);

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    void on_tray_message(WPARAM, LPARAM);
    void show_menu();
    // Returns an HICON copy of base_icon_ with a coloured dot overlay in the
    // bottom-right; nullptr on failure. Caller owns and must DestroyIcon().
    // dot_color_argb: 0xAARRGGBB; pass 0 to skip the overlay (returns a plain
    // copy of base_icon_).
    HICON make_overlay_icon_(UINT32 dot_color_argb) const;

    static constexpr int kMenuShowId = 1001;
    static constexpr int kMenuQuitId = 1002;
    // Window-item command IDs start here (one per entry in window_items_).
    static constexpr int kMenuWinBase = 2000;
    static constexpr UINT kIconId = 1;
    static constexpr const wchar_t* CLASS_NAME = L"TesseractTrayWnd";
    static bool class_registered_;

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    // Plain application icon owned for the lifetime of the tray. Held
    // separately from displayed_overlay_ so set_unread() can rebuild the
    // overlay from the source without re-loading the resource.
    HICON hIcon_ = nullptr;
    // The currently displayed overlay HICON (composited base + dot), or
    // nullptr when the plain hIcon_ is showing. set_unread() destroys this
    // before installing a new one to avoid leaks.
    HICON displayed_overlay_ = nullptr;
    std::function<void()> on_show_;
    std::function<void()> on_toggle_;
    std::function<void()> on_quit_;
    bool added_ = false;

    // Per-window menu items, rebuilt by rebuild_menu().
    std::vector<std::pair<std::string, std::function<void()>>> window_items_;
};

} // namespace win32
