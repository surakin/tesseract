#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
// GDI+ headers reference IUnknown / IStream / PROPID / byte / MIDL_INTERFACE,
// which WIN32_LEAN_AND_MEAN strips out of <windows.h>. Pull them back in.
#include <objidl.h>
#include <propidl.h>
// GdiplusTypes.h calls unqualified min()/max(); with NOMINMAX the Win32
// macros are gone, so std::min/std::max must be brought into scope (a bare
// <algorithm> is not enough — the names there are std-qualified) before it.
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

namespace win32::theme {

enum class Mode { Light, Dark };

// All colour fields are COLORREF (0x00BBGGRR) for use with GDI/Win32 APIs.
// Convert with gpc() when handing to GDI+.
struct Palette {
    COLORREF window_bg;             // chat area / message list background
    COLORREF sidebar_bg;
    COLORREF sidebar_sel_bg;
    COLORREF sidebar_hover_bg;
    COLORREF chrome_bg;             // room header / user strip / status / banner
    COLORREF compose_card_bg;
    COLORREF border;
    COLORREF separator;
    COLORREF text_primary;
    COLORREF text_secondary;
    COLORREF text_muted;            // even dimmer (timestamp, hint)
    COLORREF text_on_accent;
    COLORREF accent;
    COLORREF accent_hover;
    COLORREF accent_pressed;
    COLORREF subtle_hover;
    COLORREF subtle_pressed;
    COLORREF reaction_chip_bg;
    COLORREF reaction_chip_bg_me;
    COLORREF reaction_chip_border;
    COLORREF reaction_chip_border_me;
    COLORREF reaction_chip_text;
    COLORREF reaction_chip_text_me;
    COLORREF unread_badge_bg;
    COLORREF unread_badge_text;
};

// Read AppsUseLightTheme from the user's Personalize key. Cached.
Mode current_mode();
const Palette& palette();

// Pull the latest mode from the registry. Returns true if the mode changed.
bool refresh_from_system();

// Register the main window so apply_window_attributes() can be re-applied
// when the system theme flips at runtime (WM_SETTINGCHANGE).
void register_main_window(HWND);

// DwmSetWindowAttribute calls for dark caption / Mica backdrop / rounded
// corners. Silently no-ops on older Windows builds (E_INVALIDARG ignored).
void apply_window_attributes(HWND);

// Push the per-control "DarkMode_Explorer" / "Explorer" UxTheme on/off,
// so scrollbars and edit borders track the theme. Safe to call on any
// recent uxtheme.dll; old versions ignore unknown class lists.
void apply_control_theme(HWND);

enum class FontRole {
    Small,       //  9pt regular (preview, status, hint)
    Body,        // 10pt regular (message body)
    Ui,          // 11pt regular (default control label)
    UiSemibold,  // 11pt semibold (button label, sender name)
    Title,       // 15pt semibold (room header)
};

// Owned by the theme module; do not DeleteObject. Lazily created with
// Segoe UI Variable Display/Text where available, falling back to Segoe UI.
HFONT font(FontRole);

// Cached HBRUSH for a given COLORREF, used by WM_CTLCOLOR* handlers.
// Lifetime managed by the theme module.
HBRUSH brush(COLORREF);

// Drop all cached HFONTs so they are rebuilt at the new DPI on next use.
// Call from WM_DPICHANGED before any repaint.
void on_dpi_changed();

// Release all cached HFONT/HBRUSH; called from MainWindow::~MainWindow.
void shutdown();

// ── Fluent buttons (BS_OWNERDRAW) ─────────────────────────────────────────
// Subclass a BS_OWNERDRAW button so it paints as a flat Win11 Fluent
// button with hover / pressed / disabled tracking. Pair with
// theme::draw_button() in your WM_DRAWITEM handler.
enum class ButtonStyle { Primary, Subtle, Icon };
void register_button(HWND, ButtonStyle);
void draw_button(DRAWITEMSTRUCT*);   // dispatches by registered style

inline Gdiplus::Color gpc(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}
inline Gdiplus::Color gpc(COLORREF c, BYTE alpha) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

} // namespace win32::theme
