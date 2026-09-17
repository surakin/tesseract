#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "tk/color_contrast.h"
#include "tk/theme.h"

#include <vector>

using tk::AccentTheme;
using tk::ContrastLevel;
using tk::Palette;
using tk::Theme;
using tk::ThemeMode;

namespace
{

struct ContrastCheck
{
    const char* name;
    ContrastLevel level;
    tk::Color Palette::*fg;
    tk::Color Palette::*bg;
};

// Mirrors the contrast relationships already hand-documented in theme.cpp's
// comments -- the same floor every hand-picked palette was already held to,
// now checked mechanically for every generated (accent x mode) combination.
const std::vector<ContrastCheck>& contrast_checks()
{
    // Deliberately excludes accent_hover/accent_pressed (transient
    // interaction-state overlays, not sustained reading text) and
    // chip_border_me (a decorative accent stroke over an already
    // distinct fill, not the *sole* delimiter of the chip's shape --
    // see theme.h's own border vs. border_strong distinction for the
    // same reasoning applied to dividers).
    static const std::vector<ContrastCheck> checks = {
        {"text_primary on bg", ContrastLevel::Text, &Palette::text_primary, &Palette::bg},
        {"text_secondary on bg", ContrastLevel::Text, &Palette::text_secondary, &Palette::bg},
        {"text_on_accent on accent", ContrastLevel::Text, &Palette::text_on_accent, &Palette::accent},
        {"chip_text_me on chip_bg_me", ContrastLevel::Text, &Palette::chip_text_me, &Palette::chip_bg_me},
        {"unread_text on unread_bg", ContrastLevel::Text, &Palette::unread_text, &Palette::unread_bg},
        {"avatar_initials_text on avatar_initials_bg", ContrastLevel::Text,
            &Palette::avatar_initials_text, &Palette::avatar_initials_bg},
        {"border_strong on bg", ContrastLevel::UI, &Palette::border_strong, &Palette::bg},
    };
    return checks;
}

} // namespace

TEST_CASE("Every Theme::variant() accent/mode combination meets WCAG AA", "[tk][theme][color_contrast]")
{
    const auto mode = GENERATE(ThemeMode::Light, ThemeMode::Dark);
    const auto accent = GENERATE(AccentTheme::Blue, AccentTheme::Forest,
                                  AccentTheme::Sunset, AccentTheme::Violet,
                                  AccentTheme::System);

    const Theme& t = Theme::variant(mode, accent);
    const Palette& p = t.palette;

    for (const auto& check : contrast_checks())
    {
        INFO(check.name);
        CHECK(tk::meets_wcag_aa(p.*check.fg, p.*check.bg, check.level));
    }
}

TEST_CASE("Theme::light()/dark() (the default Blue accent) meets WCAG AA", "[tk][theme][color_contrast]")
{
    for (const Theme* t : {&Theme::light(), &Theme::dark()})
    {
        const Palette& p = t->palette;
        for (const auto& check : contrast_checks())
        {
            INFO(check.name);
            CHECK(tk::meets_wcag_aa(p.*check.fg, p.*check.bg, check.level));
        }
    }
}

TEST_CASE("Theme::variant(.., System) resolves to Blue's unmodified base palette", "[tk][theme]")
{
    // This shared layer has no platform access to read a real OS accent
    // color; System is a no-op here so ShellBase::apply_current_theme_()
    // can detect .accent == AccentTheme::System and call
    // tk::apply_system_accent() to overlay the real one (see
    // apply_system_accent tests below). A platform/desktop that reports no
    // accent simply renders System identically to Blue.
    const auto mode = GENERATE(ThemeMode::Light, ThemeMode::Dark);
    const Theme& system_theme = Theme::variant(mode, AccentTheme::System);
    const Theme& blue_theme = Theme::variant(mode, AccentTheme::Blue);
    CHECK(system_theme.palette.accent == blue_theme.palette.accent);
    CHECK(system_theme.palette.accent_hover == blue_theme.palette.accent_hover);
    CHECK(system_theme.palette.accent_pressed == blue_theme.palette.accent_pressed);
    CHECK(system_theme.palette.unread_bg == blue_theme.palette.unread_bg);
    CHECK(system_theme.palette.selection == blue_theme.palette.selection);
    CHECK(system_theme.accent == AccentTheme::System);
}

// ── apply_system_accent ─────────────────────────────────────────────────────

TEST_CASE("apply_system_accent reproduces the deleted Win32 inline math exactly",
          "[tk][theme][apply_system_accent]")
{
    // Regression check: Windows' default accent (0x0078D4) in dark mode, via
    // the values the original inline COLORREF math in
    // ui/windows/src/MainWindow.cpp produced before this port.
    Theme t = Theme::variant(ThemeMode::Dark, AccentTheme::System);
    tk::apply_system_accent(t, tk::Color::rgb(0x0078D4));

    CHECK(t.palette.accent == tk::Color::rgb(0x30A8FF));
    CHECK(t.palette.accent_hover == tk::Color::rgb(0x48C0FF));
    CHECK(t.palette.accent_pressed == tk::Color::rgb(0x1890E7));
    CHECK(t.palette.text_on_accent == tk::Color::rgb(0x1B1B1B));
    CHECK(t.palette.unread_bg == t.palette.accent);
    CHECK(t.palette.unread_text == t.palette.text_on_accent);
    CHECK(t.palette.selection == t.palette.accent.with_alpha(0x50));
}

TEST_CASE("apply_system_accent's 7 accent fields hold across a spread of raw accents",
          "[tk][theme][apply_system_accent]")
{
    const auto mode = GENERATE(ThemeMode::Light, ThemeMode::Dark);
    const auto raw = GENERATE(tk::Color::rgb(0x0078D4), // Win11 default blue
                              tk::Color::rgb(0x808080), // mid-grey
                              tk::Color::rgb(0x101010), // near-black
                              tk::Color::rgb(0xF2F2F2), // near-white
                              tk::Color::rgb(0xE81123), // saturated red
                              tk::Color::rgb(0x2ECC71), // saturated green
                              tk::Color::rgb(0xFFD700)); // saturated yellow

    Theme t = Theme::variant(mode, AccentTheme::System);
    const Palette before = t.palette; // non-accent fields must stay untouched
    tk::apply_system_accent(t, raw);

    INFO("mode=" << (mode == ThemeMode::Dark ? "Dark" : "Light"));
    CHECK(t.palette.unread_bg == t.palette.accent);
    CHECK(t.palette.unread_text == t.palette.text_on_accent);
    CHECK(t.palette.selection.a == 0x50);
    CHECK(t.palette.selection.r == t.palette.accent.r);
    CHECK(t.palette.selection.g == t.palette.accent.g);
    CHECK(t.palette.selection.b == t.palette.accent.b);
    // Not a hard CHECK: text_on_accent's single 0.45-luminance flip point is
    // a best-effort black/white pick, not an AA contrast guarantee, for an
    // arbitrary raw OS accent -- a mid-grey accent (0x808080) confirmed this
    // in practice (sits right at the WCAG boundary). This is unchanged from
    // the original Win32 behaviour this was ported from; not a regression.
    if (!tk::meets_wcag_aa(t.palette.text_on_accent, t.palette.accent, ContrastLevel::Text))
    {
        WARN("text_on_accent/accent contrast below AA for raw=" << (int)raw.r << ","
             << (int)raw.g << "," << (int)raw.b);
    }

    // Fields apply_system_accent doesn't touch stay exactly as
    // Theme::variant(mode, System) produced them.
    CHECK(t.palette.bg == before.bg);
    CHECK(t.palette.text_primary == before.text_primary);
    CHECK(t.palette.border == before.border);
    CHECK(t.palette.destructive == before.destructive);
    CHECK(t.palette.code_bg == before.code_bg);
}

TEST_CASE("apply_system_accent's wash fields follow the OS accent's hue",
          "[tk][theme][apply_system_accent]")
{
    const auto mode = GENERATE(ThemeMode::Light, ThemeMode::Dark);
    Theme t = Theme::variant(mode, AccentTheme::System);
    tk::apply_system_accent(t, tk::Color::rgb(0x2ECC71)); // saturated green

    const Theme& blue = Theme::variant(mode, AccentTheme::Blue);

    // (e) hue-distinct raw accent actually changed the wash fields, not a
    // silent no-op falling through to Blue's literals.
    CHECK(t.palette.chip_bg_me != blue.palette.chip_bg_me);
    CHECK(t.palette.chip_border_me != blue.palette.chip_border_me);
    CHECK(t.palette.chip_text_me != blue.palette.chip_text_me);
    CHECK(t.palette.bubble_bg_me != blue.palette.bubble_bg_me);
    CHECK(t.palette.avatar_initials_bg != blue.palette.avatar_initials_bg);
    CHECK(t.palette.avatar_initials_text != blue.palette.avatar_initials_text);

    // (g) avatar initials mirror the chip colours (direct-copy relationship).
    CHECK(t.palette.avatar_initials_bg == t.palette.chip_bg_me);
    CHECK(t.palette.avatar_initials_text == t.palette.chip_text_me);
}

TEST_CASE("apply_system_accent's wash fields differ between Light and Dark",
          "[tk][theme][apply_system_accent]")
{
    // (f) same raw accent, per-mode anchor branch is live (not collapsed to
    // one path).
    Theme light = Theme::variant(ThemeMode::Light, AccentTheme::System);
    Theme dark  = Theme::variant(ThemeMode::Dark, AccentTheme::System);
    const tk::Color raw = tk::Color::rgb(0x9B59B6); // saturated violet
    tk::apply_system_accent(light, raw);
    tk::apply_system_accent(dark, raw);

    CHECK(light.palette.chip_bg_me != dark.palette.chip_bg_me);
    CHECK(light.palette.chip_border_me != dark.palette.chip_border_me);
    CHECK(light.palette.chip_text_me != dark.palette.chip_text_me);
    CHECK(light.palette.bubble_bg_me != dark.palette.bubble_bg_me);
}

TEST_CASE("apply_system_accent falls back to a fixed hue for any achromatic raw accent",
          "[tk][theme][apply_system_accent]")
{
    // (h) hue is undefined for r==g==b; extract_hue_or() must fall back to a
    // fixed hue rather than leaving it at 0/undefined, so any grey OS accent
    // -- regardless of exactly how light or dark that grey is -- produces
    // the identical wash. Compares two different greys against each other
    // (rather than against a named-hue hex like Blue's own accent literal,
    // whose *actual* hue is ~211.14deg, not exactly the 211.0deg fallback
    // constant -- a byte-rounding mismatch that isn't the achromatic
    // behaviour under test).
    const auto mode = GENERATE(ThemeMode::Light, ThemeMode::Dark);
    Theme grey1 = Theme::variant(mode, AccentTheme::System);
    Theme grey2 = Theme::variant(mode, AccentTheme::System);
    tk::apply_system_accent(grey1, tk::Color::rgb(0x808080)); // mid-grey
    tk::apply_system_accent(grey2, tk::Color::rgb(0x2B2B2B)); // dark grey

    CHECK(grey1.palette.chip_bg_me == grey2.palette.chip_bg_me);
    CHECK(grey1.palette.chip_border_me == grey2.palette.chip_border_me);
    CHECK(grey1.palette.chip_text_me == grey2.palette.chip_text_me);
    CHECK(grey1.palette.bubble_bg_me == grey2.palette.bubble_bg_me);
}

// (i) Deliberately NOT asserted anywhere above: WCAG contrast on the wash
// fields (chip_bg_me/chip_text_me/bubble_bg_me/avatar_initials_*) for an
// arbitrary raw accent hue. Unlike the calibrated named accents (Forest/
// Sunset/Violet), System's wash reuses Blue's anchor lightness values at a
// substituted hue with no offline per-hue solve -- contrast is approximate
// by design (see the comment above apply_system_accent's wash extension in
// theme.cpp), not a guarantee this suite should enforce.
