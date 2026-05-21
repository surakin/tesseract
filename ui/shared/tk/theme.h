#pragma once

// Shared visual tokens. Hex values mirror docs/UI-PARITY.md. Size /
// padding / font-size constants stay in client/include/tesseract/visual.h
// (that header is also consumed by the SDK layer, so it can't pull in tk).

#include "canvas.h"

namespace tk
{

// Visual mode the palette was built for. Dark-mode parity is roadmap
// work — for now Theme::light() is the only built-in palette.
enum class ThemeMode
{
    Light,
    Dark
};

struct Palette
{
    // Surfaces & chrome
    Color bg; // chat area background
    Color sidebar_bg;
    Color sidebar_selected;
    Color sidebar_hover;
    Color chrome_bg; // headers, status, banner backgrounds
    Color compose_card_bg;
    Color border;
    Color separator;
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

    // Destructive (red) — used by confirm-and-leave style buttons.
    Color destructive;
    Color destructive_hover;
    Color destructive_pressed;

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

    // Default initials-avatar disc colours when the sender has no avatar.
    Color avatar_initials_bg;
    Color avatar_initials_text;

    // Text selection highlight — drawn behind glyphs so text remains readable.
    Color selection;
};

struct Theme
{
    ThemeMode mode;
    Palette palette;

    static const Theme& light();
    static const Theme& dark();
};

} // namespace tk
