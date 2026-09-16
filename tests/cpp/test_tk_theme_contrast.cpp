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
                                  AccentTheme::Sunset, AccentTheme::Violet);

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
