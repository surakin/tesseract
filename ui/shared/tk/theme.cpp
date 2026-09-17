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

constexpr std::array<AccentThemeInfo, 5> kAccentInfos{{
    {AccentTheme::Blue,   "Blue",   211.0f},
    {AccentTheme::Forest, "Forest", 146.0f},
    {AccentTheme::Sunset, "Sunset",  18.0f},
    {AccentTheme::Violet, "Violet", 268.0f},
    {AccentTheme::System, "System",   0.0f}, // hue unused; see make_variant
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

// ── AccentTheme::System overlay ────────────────────────────────────────────
//
// Unlike Blue/Forest/Sunset/Violet (calibrated offline for one specific
// hue), System's hue is whatever the OS reports at runtime, so there's no
// per-hue solve for it. apply_system_accent() (declared in theme.h, called
// by ShellBase::apply_current_theme_() once a platform's os_accent_color_()
// override reports a real colour) instead:
//  - ports Win32's original channel-shift derivation verbatim for the 7
//    fields it has patched since adf4f808 (accent/hover/pressed,
//    text_on_accent, unread_bg/text, selection) — kept byte-identical to
//    avoid regressing shipped Windows appearance;
//  - extends that to the wash fields (chip_bg_me/chip_border_me/
//    chip_text_me/bubble_bg_me/avatar_initials_bg/avatar_initials_text) by
//    reusing Blue's own literal lightness values as generic per-mode anchors
//    and substituting in the OS accent's extracted *hue* only, at the same
//    fixed wash saturations apply_accent() already uses above. Contrast on
//    these wash fields is therefore approximate for an arbitrary hue, not
//    calibrated the way the named accents are — an accepted limitation of
//    an OS-accent wash, not a defect.

constexpr std::uint8_t shift_ch(std::uint8_t v, int delta)
{
    const int shifted = static_cast<int>(v) + delta;
    return static_cast<std::uint8_t>(shifted < 0 ? 0 : (shifted > 255 ? 255 : shifted));
}

constexpr Color shift_rgb(Color c, int delta)
{
    return Color::rgba(shift_ch(c.r, delta), shift_ch(c.g, delta), shift_ch(c.b, delta), c.a);
}

// Non-linearised perceived-luminance approximation — the exact expression
// ui/windows/src/MainWindow.cpp used inline before this was shared. Keep it
// as-is: "fixing" it to WCAG relative luminance moves the black/white
// text_on_accent flip point and changes shipped Windows appearance.
constexpr float perceived_luma(Color c)
{
    return (0.2126f * static_cast<float>(c.r) + 0.7152f * static_cast<float>(c.g) +
            0.0722f * static_cast<float>(c.b)) /
           255.0f;
}

// Blue's own hue (kAccentInfos' Blue entry, 211 degrees) — used as the
// achromatic-input fallback below so a grey OS accent tints the wash a
// neutral blue rather than an undefined/arbitrary hue.
constexpr float kBlueHueDeg = 211.0f;

// Fixed wash saturations apply_accent() already uses for the named accents
// (see kWashSaturationLight above and the 0.53f/0.56f/0.34f dark literals in
// apply_accent()) — reused verbatim here so System's wash follows the exact
// same "pastel, not neon" shape.
constexpr float kWashSaturationDarkChipBg     = 0.53f;
constexpr float kWashSaturationDarkChipBorder = 0.56f;
constexpr float kWashSaturationDarkBubble     = 0.34f;

// Lightness anchors extracted (by hand, via standard RGB->HSL) from Blue's
// own literals in light_palette()/dark_palette() above — see the constants'
// source hex in the comment beside each.
constexpr float kSystemWashLightChipBgL   = 0.9059f; // chip_bg_me light 0xCFE3FF
constexpr float kSystemWashLightChipTextL = 0.3098f; // chip_text_me light 0x004A9E
constexpr float kSystemWashLightBubbleL   = 0.9471f; // bubble_bg_me light 0xE4F0FF
constexpr float kSystemWashDarkChipBgL    = 0.2608f; // chip_bg_me dark 0x1F3A66
constexpr float kSystemWashDarkChipTextL  = 0.8745f; // chip_text_me dark 0xBFD8FF
// No dark bubble anchor: dark bubble_bg_me is derived as
// (kSystemWashDarkChipBgL - 0.083f), the same offset apply_accent() already
// uses for the named accents' dark bubble — it reproduces Blue's own dark
// bubble_bg_me literal (0x1E2A3D) from kSystemWashDarkChipBgL.

// RGB -> hue only (S and L are never extracted: the wash always uses the
// fixed per-field/per-mode saturations and Blue's own anchor lightness
// values above — only the *hue* comes from the OS). Returns `fallback_hue`
// for an achromatic input (delta below a small epsilon), since hue is
// undefined at r==g==b.
constexpr float extract_hue_or(Color c, float fallback_hue)
{
    const float r = static_cast<float>(c.r) / 255.0f;
    const float g = static_cast<float>(c.g) / 255.0f;
    const float b = static_cast<float>(c.b) / 255.0f;
    const float mx = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    const float mn = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    const float delta = mx - mn;
    if (delta < (2.0f / 255.0f)) return fallback_hue; // achromatic
    float h;
    if (mx == r)      h = 60.0f * ((g - b) / delta);
    else if (mx == g) h = 60.0f * ((b - r) / delta + 2.0f);
    else              h = 60.0f * ((r - g) / delta + 4.0f);
    if (h < 0.0f) h += 360.0f;
    return h;
}

} // namespace

void apply_system_accent(Theme& theme, Color raw_accent)
{
    raw_accent = raw_accent.with_alpha(255);
    Palette& p = theme.palette;

    // 7 fields — behavior-preserving port of the deleted Win32 inline math.
    const Color a = (theme.mode == ThemeMode::Dark) ? shift_rgb(raw_accent, 0x30) : raw_accent;
    p.accent = a;
    p.accent_hover   = shift_rgb(a, (theme.mode == ThemeMode::Dark) ? 0x18 : -0x1A);
    p.accent_pressed = shift_rgb(a, (theme.mode == ThemeMode::Dark) ? -0x18 : -0x30);
    p.text_on_accent = (perceived_luma(a) > 0.45f) ? Color::rgb(0x1B1B1B) : Color::rgb(0xFFFFFF);
    p.unread_bg = a;
    p.unread_text = p.text_on_accent;
    p.selection = a.with_alpha(0x50);

    // Wash fields — Blue's own anchor lightness, OS accent's hue.
    const float hue = extract_hue_or(raw_accent, kBlueHueDeg);
    if (theme.mode == ThemeMode::Light)
    {
        p.chip_bg_me     = Color::from_hsl(hue, kWashSaturationLight, kSystemWashLightChipBgL);
        p.chip_border_me = Color::from_hsl(hue, kWashSaturationLight, kSystemWashLightChipBgL - 0.12f);
        p.chip_text_me   = Color::from_hsl(hue, 1.00f, kSystemWashLightChipTextL);
        p.bubble_bg_me   = Color::from_hsl(hue, kWashSaturationLight, kSystemWashLightBubbleL);
    }
    else
    {
        p.chip_bg_me     = Color::from_hsl(hue, kWashSaturationDarkChipBg, kSystemWashDarkChipBgL);
        p.chip_border_me = Color::from_hsl(hue, kWashSaturationDarkChipBorder, kSystemWashDarkChipBgL + 0.141f);
        p.chip_text_me   = Color::from_hsl(hue, 1.00f, kSystemWashDarkChipTextL);
        p.bubble_bg_me   = Color::from_hsl(hue, kWashSaturationDarkBubble, kSystemWashDarkChipBgL - 0.083f);
    }
    p.avatar_initials_bg   = p.chip_bg_me;
    p.avatar_initials_text = p.chip_text_me;
}

namespace
{

constexpr Theme make_variant(ThemeMode mode, AccentTheme accent)
{
    Palette base = (mode == ThemeMode::Light) ? light_palette() : dark_palette();
    // System resolves to the same unmodified palette as Blue here;
    // ShellBase::apply_current_theme_() overlays the real OS accent color on
    // top (via apply_system_accent(), above) when it sees
    // AccentTheme::System — this shared layer has no platform access, so it
    // can't do that overlay itself.
    if (accent == AccentTheme::Blue || accent == AccentTheme::System)
        return Theme{mode, base, accent}; // unchanged original literals
    for (const auto& spec : kAccentSpecs)
        if (spec.id == accent)
            return Theme{mode, apply_accent(base, mode, spec), accent};
    return Theme{mode, base, accent}; // unreachable
}

const Theme g_light = make_variant(ThemeMode::Light, AccentTheme::Blue);
const Theme g_dark  = make_variant(ThemeMode::Dark, AccentTheme::Blue);

const std::array<Theme, 10> g_variants{{
    make_variant(ThemeMode::Light, AccentTheme::Blue),
    make_variant(ThemeMode::Light, AccentTheme::Forest),
    make_variant(ThemeMode::Light, AccentTheme::Sunset),
    make_variant(ThemeMode::Light, AccentTheme::Violet),
    make_variant(ThemeMode::Light, AccentTheme::System),
    make_variant(ThemeMode::Dark, AccentTheme::Blue),
    make_variant(ThemeMode::Dark, AccentTheme::Forest),
    make_variant(ThemeMode::Dark, AccentTheme::Sunset),
    make_variant(ThemeMode::Dark, AccentTheme::Violet),
    make_variant(ThemeMode::Dark, AccentTheme::System),
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

const std::array<AccentThemeInfo, 5>& accent_theme_infos()
{
    return kAccentInfos;
}

} // namespace tk
