#pragma once

// Shared visual tokens. Hex values mirror docs/UI-PARITY.md. Size /
// padding / font-size constants stay in client/include/tesseract/visual.h
// (that header is also consumed by the SDK layer, so it can't pull in tk).

#include "canvas.h"

#include <array>

namespace tk
{

// Visual mode the palette was built for. Both Theme::light() and
// Theme::dark() are shipped; the active one is resolved per platform
// from Settings::ThemePreference (see ShellBase::apply_current_theme_()).
enum class ThemeMode
{
    Light,
    Dark
};

// Named accent-color variant. Blue is the original, unchanged default;
// Forest/Sunset/Violet are generated from a seed hue (see theme.cpp) and
// only override the accent-dependent Palette fields — every other field
// (surfaces, text, borders, presence dots, ...) is shared across accents
// for a given ThemeMode, unchanged from the original hand-tuned palette.
// System resolves to the same unmodified palette as Blue at this shared
// layer; ShellBase::apply_current_theme_() overlays the real OS accent color
// on top (via apply_system_accent() below) when it sees AccentTheme::System,
// using whatever platform shell's os_accent_color_() override reports one.
// A platform/desktop that exposes no such concept (os_accent_color_()
// returns nullopt) simply renders System identically to Blue.
enum class AccentTheme
{
    Blue,
    Forest,
    Sunset,
    Violet,
    System,
};

struct AccentThemeInfo
{
    AccentTheme id;
    const char* name; // untranslated key, e.g. "Blue" — wrap in tk::tr() at the UI call site
    float seed_hue_deg; // 0..360; unused for System (no generation happens)
};

// Stable order == UI listing order. Iterated by both theme.cpp (generation)
// and AppearanceSection (populating its accent picker).
const std::array<AccentThemeInfo, 5>& accent_theme_infos();

struct Palette
{
    // Surfaces & chrome
    Color bg; // chat area background
    Color sidebar_bg;
    Color sidebar_selected;
    Color sidebar_hover;
    Color section_header_bg;    // room-list section header (idle)
    Color section_header_hover; // room-list section header (hovered)
    Color chrome_bg; // headers, status, banner backgrounds
    Color compose_card_bg;
    Color border;
    Color separator;
    // Higher-contrast stroke (WCAG 1.4.11 3:1) for the handful of places
    // where a border is the *sole* delimiter of a component's shape (input
    // field edges whose fill matches the surrounding card) rather than a
    // decorative divider layered over an already-distinct fill.
    Color border_strong;
    Color popup_border; // outer frame of floating pickers

    // Text
    Color text_primary;
    Color text_secondary;
    Color text_muted; // timestamp, hint text
    Color text_on_accent;

    // Accent + interactive
    Color accent;
    Color accent_hover;
    Color accent_pressed;
    Color subtle_hover;
    Color subtle_pressed;

    // Message bubbles (Settings::message_layout == Bubbles). Very low contrast,
    // opaque, no border — a barely-there wash behind each message body.
    Color bubble_bg;    // other users' messages
    Color bubble_bg_me; // own (right-aligned) messages — faint accent tint

    // Destructive (red) — used by confirm-and-leave style buttons.
    Color destructive;
    Color destructive_hover;
    Color destructive_pressed;

    // Success (green) — status text for a positive outcome (e.g. homeserver
    // discovery resolved).
    Color success;

    // Reaction chips
    Color chip_bg;
    Color chip_bg_me;
    Color chip_border;
    Color chip_border_me;
    Color chip_text;
    Color chip_text_me;

    // Unread badge
    Color unread_bg;
    Color unread_text;

    // Presence dots (DM avatar badge + member rows)
    Color presence_online;      // green
    Color presence_unavailable; // amber
    Color presence_offline;     // muted grey

    // Default initials-avatar disc colours when the sender has no avatar.
    Color avatar_initials_bg;
    Color avatar_initials_text;

    // Text selection highlight — drawn behind glyphs so text remains readable.
    Color selection;

    // Tint drawn behind inline code / fenced code-block runs in message bodies.
    Color code_bg;
};

struct Theme
{
    ThemeMode mode;
    Palette palette;
    // Appended after `palette` (not inserted earlier) so the field stays
    // out of the way of any positional aggregate-init of {mode, palette}.
    AccentTheme accent = AccentTheme::Blue;

    static const Theme& light();
    static const Theme& dark();
    static const Theme& variant(ThemeMode mode, AccentTheme accent);
};

// Overlay a raw OS accent colour onto an already-resolved AccentTheme::System
// theme, in place. `raw_accent` is the colour the platform reports (opaque
// sRGB); `theme.mode` selects the light/dark derivation. Overwrites the
// accent-dependent Palette fields (accent, accent_hover, accent_pressed,
// text_on_accent, unread_bg, unread_text, selection, chip_bg_me,
// chip_border_me, chip_text_me, bubble_bg_me, avatar_initials_bg,
// avatar_initials_text) and leaves every other field untouched. Callers
// gate on AccentTheme::System (see ShellBase::apply_current_theme_()). Not
// constexpr (runtime input), unlike the rest of this header.
void apply_system_accent(Theme& theme, Color raw_accent);

} // namespace tk
