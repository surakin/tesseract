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
// picker widget (tesseract::views::EmojiPicker / StickerPicker) plus a native
// search field overlay. The main window has its own bespoke picker plumbing;
// this exists so pop-out room windows can own their own pickers (routing
// selections to their own room / composer) without duplicating the HWND +
// surface + search-field glue.
//
// The caller builds the picker widget (wiring its on_selected / image provider
// to its own state) and hands it to the popup as the root; the popup owns the
// window, surface, and search field, and drives positioning / show / hide.
class Win32PickerPopup
{
public:
    struct Config
    {
        HINSTANCE inst = nullptr;
        HWND owner = nullptr;          // parent HWND for CreateWindowExW
        tk::Theme theme;               // initial theme
        const wchar_t* class_name = nullptr; // unique window class per kind
        float width_dip = 0.f;
        float height_dip = 0.f;
        std::string search_placeholder;
        // query → caller updates the picker's search filter + relayouts.
        std::function<void(const std::string&)> on_search;
        // returns the picker's desired search-field rect (DIP, surface-local).
        std::function<tk::Rect()> search_rect;
        // called just before each show: refresh frequents/packs + clear filter.
        std::function<void()> on_before_show;
    };

    Win32PickerPopup(std::unique_ptr<tk::Widget> root, Config cfg);
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
    void set_theme(const tk::Theme& t);

private:
    LONG dip_to_phys_(float dip) const;
    void register_class_once_();

    Config cfg_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> surface_;
    std::unique_ptr<tk::NativeTextField> search_field_;
};

} // namespace win32
