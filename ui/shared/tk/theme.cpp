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
    // Was 0xEBEDF0 — only 5 RGB units from sidebar_bg (0xF0F2F5), nearly
    // imperceptible. Widened to a clearly visible step while staying
    // lighter than sidebar_selected, so hover and selection stay distinct.
    p.sidebar_hover = Color::rgb(0xE7E9EE);
    // Section headers sit distinctly darker than the room list (sidebar_bg
    // 0xF0F2F5); the hover state steps darker again for clear feedback.
    p.section_header_bg    = Color::rgb(0xDADDE5);
    p.section_header_hover = Color::rgb(0xC6CAD4);
    p.chrome_bg = Color::rgb(0xF8F9FA);
    // Matches sidebar_bg — a clearly distinct "input well" tone against
    // chrome_bg (the compose bar's own background). See the dark palette
    // below, which had the analogous bug (identical to chrome_bg) fixed to
    // mirror this relationship.
    p.compose_card_bg = Color::rgb(0xF0F2F5);
    p.border = Color::rgb(0xD0D3D8);
    p.separator = Color::rgb(0xD0D3D8);
    // Input-field edges (login card, sidebar/picker search boxes) fill with
    // a color matching their surrounding card, so the stroke is the *only*
    // thing that reads as their shape — needs the WCAG 1.4.11 3:1 floor,
    // unlike `border`'s other, purely decorative divider uses.
    p.border_strong = Color::rgb(0x92959A);
    p.popup_border = Color::rgb(0xBBBEC4);

    // Text
    p.text_primary = Color::rgb(0x111111);
    p.text_secondary = Color::rgb(0x76767B);
    p.text_muted = Color::rgb(0x76767C);
    p.text_on_accent = Color::rgb(0xFFFFFF);

    // Accent
    p.accent = Color::rgb(0x0072ED);
    p.accent_hover = Color::rgb(0x1A92FF);
    p.accent_pressed = Color::rgb(0x006BD1);
    p.subtle_hover = Color::rgba(0x00, 0x00, 0x00, 0x0F);
    p.subtle_pressed = Color::rgba(0x00, 0x00, 0x00, 0x1F);

    // Message bubbles — a faint step off the white chat area (bg 0xFFFFFF);
    // the "me" tint stays well below chip_bg_me (0xCFE3FF).
    p.bubble_bg    = Color::rgb(0xF0F2F5);
    p.bubble_bg_me = Color::rgb(0xE4F0FF);

    p.destructive         = Color::rgb(0xD93636);
    p.destructive_hover   = Color::rgb(0xE04848);
    p.destructive_pressed = Color::rgb(0xB52A2A);

    p.success = Color::rgb(0x008A00);

    // Reaction chips
    p.chip_bg = Color::rgb(0xEBEDF0);
    p.chip_bg_me = Color::rgb(0xCFE3FF);
    p.chip_border = Color::rgb(0xD0D3D8);
    p.chip_border_me = Color::rgb(0x9CC4FF);
    p.chip_text = Color::rgb(0x111111);
    p.chip_text_me = Color::rgb(0x004A9E);

    // Unread badge — same accent as buttons + focus ring.
    p.unread_bg = Color::rgb(0x0072ED);
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
    p.presence_online      = Color::rgb(0x0EAC20); // green
    p.presence_unavailable = Color::rgb(0xEB7107); // amber
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
    // Was 0x202327 — identical to chrome_bg, so the compose input card had
    // zero fill contrast against its own bar in dark mode (only the 1px
    // border defined it). Mirrors the light palette, where compose_card_bg
    // already equals sidebar_bg for a clearly distinct "input well" tone.
    p.compose_card_bg = Color::rgb(0x16181C);
    p.border = Color::rgb(0x33363B);
    p.separator = Color::rgb(0x33363B);
    p.border_strong = Color::rgb(0x65686D);
    p.popup_border = Color::rgb(0x50535A);

    p.text_primary = Color::rgb(0xF0F0F2);
    p.text_secondary = Color::rgb(0xA0A0A8);
    p.text_muted = Color::rgb(0x84848C);
    // Dark text on the light accent blue, not white — same navy already
    // used for unread_text below, since white-on-accent only hits 2.63:1.
    p.text_on_accent = Color::rgb(0x0B1320);

    p.accent = Color::rgb(0x4DA3FF);
    p.accent_hover = Color::rgb(0x66B3FF);
    p.accent_pressed = Color::rgb(0x3388E0);
    p.subtle_hover = Color::rgba(0xFF, 0xFF, 0xFF, 0x14);
    p.subtle_pressed = Color::rgba(0xFF, 0xFF, 0xFF, 0x28);

    // Message bubbles — a faint step off the dark chat area (bg 0x1B1D21);
    // the "me" tint is a muted navy, far less saturated than chip_bg_me.
    p.bubble_bg    = Color::rgb(0x24272C);
    p.bubble_bg_me = Color::rgb(0x1E2A3D);

    p.destructive         = Color::rgb(0xE45656);
    p.destructive_hover   = Color::rgb(0xEC6868);
    p.destructive_pressed = Color::rgb(0xC44343);

    p.success = Color::rgb(0x23B064);

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

constexpr std::array<AccentThemeInfo, 4> kAccentInfos{{
    {AccentTheme::Blue,   "Blue",   211.0f},
    {AccentTheme::Forest, "Forest", 146.0f},
    {AccentTheme::Sunset, "Sunset",  18.0f},
    {AccentTheme::Violet, "Violet", 268.0f},
}};

// Per-accent, per-mode lightness constants for the ~11 accent-dependent
// Palette fields, for every accent *except* Blue. Blue keeps the exact
// original literals from light_palette()/dark_palette() untouched (see
// make_variant below) — it isn't run through this table at all, so the
// shipped default theme carries zero regression risk from this feature.
//
// A flat HSL lightness can't simply be reused across hues: HSL is not
// perceptually uniform, so e.g. green at the same (S, L) as blue reads as
// far brighter (its luminance weight, 0.7152, dwarfs blue's 0.0722). Each
// constant below was solved offline (see the calibration script referenced
// in the accent-themes PR) so every accent hits comparable WCAG contrast
// margins to Blue's, not just a shared lightness number:
//  - *_accent_l: chosen so the resting `accent` swatch clears text_on_accent
//    contrast with a safety margin over the 4.5:1 AA floor (light targets
//    ~4.7:1 against white, dark targets ~5.0:1 against navy).
//  - *_chip_bg_l / *_chip_text_l: chosen so chip_bg_me's luminance matches
//    Blue's own pale-wash / muted-wash target, and chip_text_me then clears
//    a ~6:1 margin against that resolved background.
//  - light_bubble_bg_l: matched to Blue's own faint-wash luminance target.
// Hover/pressed/border/bubble derive from these via the same fixed offsets
// Blue's own hand-picked values used (see apply_accent) — those aren't
// independently contrast-gated (transient interaction states, or decorative
// dividers rather than a shape's sole delimiter), so reusing Blue's spacing
// keeps them visually consistent without needing their own solve.
struct AccentSpec
{
    AccentTheme id;
    float hue;
    float light_accent_l;
    float light_chip_bg_l;
    float light_chip_text_l;
    float light_bubble_bg_l;
    float dark_accent_l;
    float dark_chip_bg_l;
    float dark_chip_text_l;
};

constexpr std::array<AccentSpec, 3> kAccentSpecs{{
    {AccentTheme::Forest, 146.0f, 0.2618f, 0.8582f, 0.1873f, 0.9230f, 0.3010f, 0.1704f, 0.4245f},
    {AccentTheme::Sunset,  18.0f, 0.4127f, 0.8882f, 0.2951f, 0.9398f, 0.4716f, 0.2314f, 0.7521f},
    {AccentTheme::Violet, 268.0f, 0.6206f, 0.9056f, 0.4422f, 0.9483f, 0.6814f, 0.3110f, 0.8382f},
}};

// Light-mode "wash" fields (chip_bg_me, chip_border_me, bubble_bg_me) use a
// low, fixed saturation across every accent rather than the hue's full
// strength — at S=1.0 a hue like green stays visually vivid even at a very
// high lightness (again, HSL non-uniformity), which reads as a neon swatch
// instead of the faint pastel wash these fields are meant to be.
constexpr float kWashSaturationLight = 0.35f;

// Overwrites only the accent-dependent subset of an already-built Palette
// using `spec`'s calibrated constants. Every other field (surfaces, text,
// borders, presence dots, ...) is left untouched, so this is applied on top
// of a copy of light_palette()/dark_palette(), never in place of it.
constexpr Palette apply_accent(Palette p, ThemeMode mode, const AccentSpec& spec)
{
    const float hue = spec.hue;
    if (mode == ThemeMode::Light)
    {
        p.accent         = Color::from_hsl(hue, 1.00f, spec.light_accent_l);
        p.accent_hover   = Color::from_hsl(hue, 1.00f, spec.light_accent_l + 0.085f);
        p.accent_pressed = Color::from_hsl(hue, 1.00f, spec.light_accent_l - 0.055f);
        p.chip_bg_me     = Color::from_hsl(hue, kWashSaturationLight, spec.light_chip_bg_l);
        p.chip_border_me = Color::from_hsl(hue, kWashSaturationLight, spec.light_chip_bg_l - 0.12f);
        p.chip_text_me   = Color::from_hsl(hue, 1.00f, spec.light_chip_text_l);
        p.bubble_bg_me   = Color::from_hsl(hue, kWashSaturationLight, spec.light_bubble_bg_l);
        p.unread_bg            = p.accent;
        p.avatar_initials_bg   = p.chip_bg_me;
        p.avatar_initials_text = p.chip_text_me;
        p.selection = Color::from_hsl(hue, 1.00f, 0.50f).with_alpha(0x50);
    }
    else // Dark
    {
        p.accent         = Color::from_hsl(hue, 1.00f, spec.dark_accent_l);
        p.accent_hover   = Color::from_hsl(hue, 1.00f, spec.dark_accent_l + 0.049f);
        p.accent_pressed = Color::from_hsl(hue, 1.00f, spec.dark_accent_l - 0.081f);
        p.chip_bg_me     = Color::from_hsl(hue, 0.53f, spec.dark_chip_bg_l);
        p.chip_border_me = Color::from_hsl(hue, 0.56f, spec.dark_chip_bg_l + 0.141f);
        p.chip_text_me   = Color::from_hsl(hue, 1.00f, spec.dark_chip_text_l);
        p.bubble_bg_me   = Color::from_hsl(hue, 0.34f, spec.dark_chip_bg_l - 0.083f);
        p.unread_bg            = p.accent;
        p.avatar_initials_bg   = p.chip_bg_me;
        p.avatar_initials_text = p.chip_text_me;
        p.selection = p.accent.with_alpha(0x50);
    }
    return p;
}

constexpr Theme make_variant(ThemeMode mode, AccentTheme accent)
{
    Palette base = (mode == ThemeMode::Light) ? light_palette() : dark_palette();
    if (accent == AccentTheme::Blue)
        return Theme{mode, base, accent}; // unchanged original literals
    for (const auto& spec : kAccentSpecs)
        if (spec.id == accent)
            return Theme{mode, apply_accent(base, mode, spec), accent};
    return Theme{mode, base, accent}; // unreachable
}

const Theme g_light = make_variant(ThemeMode::Light, AccentTheme::Blue);
const Theme g_dark  = make_variant(ThemeMode::Dark, AccentTheme::Blue);

const std::array<Theme, 8> g_variants{{
    make_variant(ThemeMode::Light, AccentTheme::Blue),
    make_variant(ThemeMode::Light, AccentTheme::Forest),
    make_variant(ThemeMode::Light, AccentTheme::Sunset),
    make_variant(ThemeMode::Light, AccentTheme::Violet),
    make_variant(ThemeMode::Dark, AccentTheme::Blue),
    make_variant(ThemeMode::Dark, AccentTheme::Forest),
    make_variant(ThemeMode::Dark, AccentTheme::Sunset),
    make_variant(ThemeMode::Dark, AccentTheme::Violet),
}};

} // namespace

const Theme& Theme::light()
{
    return g_light;
}
const Theme& Theme::dark()
{
    return g_dark;
}

const Theme& Theme::variant(ThemeMode mode, AccentTheme accent)
{
    for (const auto& t : g_variants)
        if (t.mode == mode && t.accent == accent) return t;
    return (mode == ThemeMode::Dark) ? g_dark : g_light; // unreachable
}

const std::array<AccentThemeInfo, 4>& accent_theme_infos()
{
    return kAccentInfos;
}

} // namespace tk
