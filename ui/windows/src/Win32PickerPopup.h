#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "tk/canvas.h" // tk::Rect
#include "tk/host_win32.h"
#include "tk/theme.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace win32
{

// A reusable WS_POPUP window hosting a tk::win32::Surface that paints a shared
// picker widget (tesseract::views::EmojiPicker / StickerPicker), which owns
// its own search field. The main window has its own bespoke picker plumbing;
// this exists so pop-out room windows can own their own pickers (routing
// selections to their own room / composer) without duplicating the HWND +
// surface glue.
//
// The caller supplies a factory that builds the picker widget given a live
// Host& (available only after the Surface exists, hence the factory instead
// of a pre-built widget — construction order is bottom-up, so the widget
// can't be built before the Surface/Host it needs to construct its own
// search field). The popup owns the window and surface, and drives
// positioning / show / hide.
class Win32PickerPopup
{
public:
    using RootFactory = std::function<std::unique_ptr<tk::Widget>(tk::Host&)>;

    struct Config
    {
        HINSTANCE inst = nullptr;
        HWND owner = nullptr;          // parent HWND for CreateWindowExW
        tk::Theme theme;               // initial theme
        const wchar_t* class_name = nullptr; // unique window class per kind
        float width_dip = 0.f;
        float height_dip = 0.f;
        // called just before each show: refresh frequents/packs + clear filter.
        std::function<void()> on_before_show;
        // called after each show (post-relayout): caller focuses its own
        // picker's search field.
        std::function<void()> on_shown;
        // called on every hide() — toggle-close, an explicit hide() after a
        // selection, etc. Caller uses this to return focus to the compose
        // box now that nothing else claims it.
        std::function<void()> on_hide;
    };

    Win32PickerPopup(RootFactory make_root, Config cfg);
    ~Win32PickerPopup();

    Win32PickerPopup(const Win32PickerPopup&) = delete;
    Win32PickerPopup& operator=(const Win32PickerPopup&) = delete;

    // Show centered above `anchor_rect` (surface-local DIP coords in
    // `anchor_hwnd`), falling back below and clamping to the monitor work area.
    void show_at(HWND anchor_hwnd, tk::Rect anchor_rect);
    // Show if currently hidden, otherwise hide (compose-button toggle).
    void toggle_at(HWND anchor_hwnd, tk::Rect anchor_rect);
    void hide();
    bool visible() const;
    // Invalidate the popup surface so it repaints (e.g. animation tick). No-op
    // when hidden.
    void repaint();
    void set_theme(const tk::Theme& t);

private:
    LONG dip_to_phys_(float dip) const;
    void register_class_once_();

    Config cfg_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> surface_;
};

} // namespace win32
