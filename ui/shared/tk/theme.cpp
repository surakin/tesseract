#include "theme.h"

namespace tk
{

namespace
{

constexpr Palette light_palette()
{
    Palette p{};
    // Surfaces (docs/UI-PARITY.md "Colour token table")
    p.bg = Color::rgb(0xFFFFFF);
    p.sidebar_bg = Color::rgb(0xF0F2F5);
    p.sidebar_selected = Color::rgb(0xE4E6EB);
    p.sidebar_hover = Color::rgb(0xEBEDF0);
    p.chrome_bg = Color::rgb(0xF8F9FA);
    p.compose_card_bg = Color::rgb(0xF0F2F5);
    p.border = Color::rgb(0xD0D3D8);
    p.separator = Color::rgb(0xD0D3D8);
    p.popup_border = Color::rgb(0xBBBEC4);

    // Text
    p.text_primary = Color::rgb(0x111111);
    p.text_secondary = Color::rgb(0x8E8E93);
    p.text_muted = Color::rgb(0xA0A0A6);
    p.text_on_accent = Color::rgb(0xFFFFFF);

    // Accent
    p.accent = Color::rgb(0x0084FF);
    p.accent_hover = Color::rgb(0x1A92FF);
    p.accent_pressed = Color::rgb(0x006BD1);
    p.subtle_hover = Color::rgba(0x00, 0x00, 0x00, 0x0F);
    p.subtle_pressed = Color::rgba(0x00, 0x00, 0x00, 0x1F);

    // Reaction chips
    p.chip_bg = Color::rgb(0xEBEDF0);
    p.chip_bg_me = Color::rgb(0xCFE3FF);
    p.chip_border = Color::rgb(0xD0D3D8);
    p.chip_border_me = Color::rgb(0x9CC4FF);
    p.chip_text = Color::rgb(0x111111);
    p.chip_text_me = Color::rgb(0x004A9E);

    // Unread badge — same accent as buttons + focus ring.
    p.unread_bg = Color::rgb(0x0084FF);
    p.unread_text = Color::rgb(0xFFFFFF);

    // Initials disc
    p.avatar_initials_bg = Color::rgb(0xCFE3FF);
    p.avatar_initials_text = Color::rgb(0x004A9E);
    return p;
}

constexpr Palette dark_palette()
{
    Palette p{};
    // First-pass dark-mode palette. Will be refined once Step-5 dark
    // mode parity work begins; values here are conservative.
    p.bg = Color::rgb(0x1B1D21);
    p.sidebar_bg = Color::rgb(0x16181C);
    p.sidebar_selected = Color::rgb(0x2A2D33);
    p.sidebar_hover = Color::rgb(0x23262B);
    p.chrome_bg = Color::rgb(0x202327);
    p.compose_card_bg = Color::rgb(0x202327);
    p.border = Color::rgb(0x33363B);
    p.separator = Color::rgb(0x33363B);
    p.popup_border = Color::rgb(0x50535A);

    p.text_primary = Color::rgb(0xF0F0F2);
    p.text_secondary = Color::rgb(0xA0A0A8);
    p.text_muted = Color::rgb(0x808088);
    p.text_on_accent = Color::rgb(0xFFFFFF);

    p.accent = Color::rgb(0x4DA3FF);
    p.accent_hover = Color::rgb(0x66B3FF);
    p.accent_pressed = Color::rgb(0x3388E0);
    p.subtle_hover = Color::rgba(0xFF, 0xFF, 0xFF, 0x14);
    p.subtle_pressed = Color::rgba(0xFF, 0xFF, 0xFF, 0x28);

    p.chip_bg = Color::rgb(0x2A2D33);
    p.chip_bg_me = Color::rgb(0x1F3A66);
    p.chip_border = Color::rgb(0x33363B);
    p.chip_border_me = Color::rgb(0x2D55A0);
    p.chip_text = Color::rgb(0xE6E6E8);
    p.chip_text_me = Color::rgb(0xBFD8FF);

    p.unread_bg = Color::rgb(0x4DA3FF);
    p.unread_text = Color::rgb(0x0B1320);

    p.avatar_initials_bg = Color::rgb(0x1F3A66);
    p.avatar_initials_text = Color::rgb(0xBFD8FF);
    return p;
}

const Theme g_light{ThemeMode::Light, light_palette()};
const Theme g_dark{ThemeMode::Dark, dark_palette()};

} // namespace

const Theme& Theme::light()
{
    return g_light;
}
const Theme& Theme::dark()
{
    return g_dark;
}

} // namespace tk
