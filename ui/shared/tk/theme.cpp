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
    // Section headers sit distinctly darker than the room list (sidebar_bg
    // 0xF0F2F5); the hover state steps darker again for clear feedback.
    p.section_header_bg    = Color::rgb(0xDADDE5);
    p.section_header_hover = Color::rgb(0xC6CAD4);
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

    p.destructive         = Color::rgb(0xD93636);
    p.destructive_hover   = Color::rgb(0xE04848);
    p.destructive_pressed = Color::rgb(0xB52A2A);

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

    // Selection highlight
    p.selection = Color::rgba(0x00, 0x84, 0xFF, 0x50);

    // Code-run tint — distinctly darker than the grey message area
    // (sidebar_bg 0xF0F2F5) so inline/fenced code reads as a tinted panel.
    p.code_bg = Color::rgb(0xD9DCE3);

    // Presence dots
    p.presence_online      = Color::rgb(0x2ECC40); // green
    p.presence_unavailable = Color::rgb(0xFF851B); // amber
    p.presence_offline     = Color::rgb(0xB0B3BA); // muted grey
    return p;
}

constexpr Palette dark_palette()
{
    Palette p{};
    // Dark-mode palette, mirrored in docs/UI-PARITY.md's colour table.
    p.bg = Color::rgb(0x1B1D21);
    p.sidebar_bg = Color::rgb(0x16181C);
    p.sidebar_selected = Color::rgb(0x2A2D33);
    p.sidebar_hover = Color::rgb(0x23262B);
    // Section headers sit distinctly lighter than the dark room list
    // (sidebar_bg 0x16181C); the hover state steps lighter again.
    p.section_header_bg    = Color::rgb(0x2D3138);
    p.section_header_hover = Color::rgb(0x3C414A);
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

    p.destructive         = Color::rgb(0xE45656);
    p.destructive_hover   = Color::rgb(0xEC6868);
    p.destructive_pressed = Color::rgb(0xC44343);

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

    p.selection = Color::rgba(0x4D, 0xA3, 0xFF, 0x50);

    // Code-run tint (lighter than the dark message area, sidebar_bg 0x16181C).
    p.code_bg = Color::rgb(0x2E3138);

    // Presence dots
    p.presence_online      = Color::rgb(0x23B064); // green
    p.presence_unavailable = Color::rgb(0xE08C1A); // amber
    p.presence_offline     = Color::rgb(0x606470); // muted grey
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
