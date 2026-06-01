#pragma once
#include <algorithm>
#include <string>

namespace tesseract
{

// ── Tray icon badge geometry ───────────────────────────────────────────────
// Shared across all four platform tray implementations (Win32, Qt6, GTK4, macOS).
// Colors mirror the light palette in ui/shared/tk/theme.cpp.
inline constexpr unsigned int kBadgeColorMention = 0xD93636u; // destructive/red
inline constexpr unsigned int kBadgeColorUnread  = 0x0084FFu; // accent/blue

// Dot diameter in pixels: ~38% of the shorter icon edge, minimum 6px.
inline int badge_dot_px(int side) { return std::max(6, side * 38 / 100); }
// Inset from the bottom-right edge so the outline isn't clipped: 1/32 of side, min 1px.
inline int badge_inset_px(int side) { return std::max(1, side / 32); }

class ITrayIcon
{
public:
    virtual ~ITrayIcon() = default;

    // Returns true if the tray icon was successfully created and is currently
    // visible to the user. Linux without a StatusNotifierItem host or XEmbed
    // tray, or any platform whose underlying API rejected creation, returns
    // false. Callers use this to gate minimize-to-tray behaviour: if false,
    // fall back to a real quit on window close.
    virtual bool is_available() const = 0;

    virtual void set_tooltip(const std::string& text) = 0;

    // Update the unread/mention indicator overlay on the tray icon. Called by
    // ShellBase whenever the aggregate across all signed-in accounts changes.
    // has_highlight implies has_unread (a highlight is a notification) but
    // shells should not assume that — render the dot when either flag is set,
    // preferring the highlight color when both are true. With both false the
    // shell must restore the plain base icon. Default no-op so test fakes and
    // shells without an implementation yet keep working.
    virtual void set_unread(bool /*has_unread*/, bool /*has_highlight*/) {}
};

} // namespace tesseract
