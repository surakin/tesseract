#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "tk/theme.h"

#include <cstring>
#include <vector>

using tk::AccentTheme;
using tk::Color;
using tk::HighContrastColors;
using tk::Palette;
using tk::Theme;
using tk::ThemeMode;

namespace
{

// Windows' "High Contrast Black" and "High Contrast White" system colours.
HighContrastColors hc_black()
{
    return {Color::rgb(0x000000), Color::rgb(0xFFFFFF), Color::rgb(0x1AEBFF),
            Color::rgb(0x000000), Color::rgb(0xFFFF00), Color::rgb(0x3FF23F),
            Color::rgb(0x000000), Color::rgb(0xFFFFFF)};
}

HighContrastColors hc_white()
{
    return {Color::rgb(0xFFFFFF), Color::rgb(0x000000), Color::rgb(0x37006E),
            Color::rgb(0xFFFFFF), Color::rgb(0x600000), Color::rgb(0x600000),
            Color::rgb(0xFFFFFF), Color::rgb(0x000000)};
}

// Palette is a plain struct of Color fields, so it can be walked as an array.
std::vector<Color> all_colours(const Palette& p)
{
    static_assert(sizeof(Palette) % sizeof(Color) == 0);
    std::vector<Color> out(sizeof(Palette) / sizeof(Color));
    std::memcpy(out.data(), &p, sizeof(Palette));
    return out;
}

} // namespace

TEST_CASE("make_high_contrast_theme picks the mode from the window colour",
          "[tk][theme][high_contrast]")
{
    CHECK(tk::make_high_contrast_theme(hc_black()).mode == ThemeMode::Dark);
    CHECK(tk::make_high_contrast_theme(hc_white()).mode == ThemeMode::Light);
}

TEST_CASE("make_high_contrast_theme maps the system colours",
          "[tk][theme][high_contrast]")
{
    const auto hc = GENERATE(hc_black(), hc_white());
    const Theme t = tk::make_high_contrast_theme(hc);
    const Palette& p = t.palette;

    CHECK(t.high_contrast);
    CHECK(p.bg == hc.window);
    CHECK(p.sidebar_bg == hc.window);
    CHECK(p.chrome_bg == hc.window);
    CHECK(p.text_primary == hc.window_text);
    CHECK(p.text_secondary == hc.window_text);
    CHECK(p.text_muted == hc.window_text);
    CHECK(p.border == hc.window_text);
    CHECK(p.border_strong == hc.window_text);
    CHECK(p.accent == hc.highlight);
    CHECK(p.text_on_accent == hc.highlight_text);
    CHECK(p.unread_bg == hc.highlight);
    CHECK(p.unread_text == hc.highlight_text);
    CHECK(p.selection == hc.highlight);
    CHECK(p.selection_text == hc.highlight_text);
    CHECK(p.link == hc.hotlight);
    CHECK(p.text_disabled == hc.gray_text);
    CHECK(p.text_error == hc.window_text);
    CHECK(p.notice_bg == hc.window);
    CHECK(p.notice_text == hc.window_text);
    CHECK(p.warning_bg == hc.window);
    CHECK(p.destructive == hc.highlight);
    CHECK(p.sidebar_selected == hc.highlight);
    CHECK(p.selected_text == hc.highlight_text);
    CHECK(tk::on_selected(t, Color::rgb(0x123456)) == hc.highlight_text);
}

TEST_CASE("make_high_contrast_theme builds an opaque palette",
          "[tk][theme][high_contrast]")
{
    const auto hc = GENERATE(hc_black(), hc_white());
    // Even when the OS colours carry alpha, nothing translucent comes out.
    HighContrastColors translucent = hc;
    translucent.window.a = 40;
    translucent.highlight.a = 40;

    for (const HighContrastColors& in : {hc, translucent})
    {
        const Theme t = tk::make_high_contrast_theme(in);
        for (const Color& c : all_colours(t.palette))
        {
            CHECK(c.a == 255);
        }
    }
}

TEST_CASE("Normal themes leave High Contrast off", "[tk][theme][high_contrast]")
{
    CHECK_FALSE(Theme::light().high_contrast);
    CHECK_FALSE(Theme::dark().high_contrast);
}

TEST_CASE("The new palette roles equal the colours their call sites used",
          "[tk][theme][high_contrast]")
{
    const auto mode = GENERATE(ThemeMode::Light, ThemeMode::Dark);
    const auto accent = GENERATE(AccentTheme::Blue, AccentTheme::Forest,
                                 AccentTheme::Sunset, AccentTheme::Violet,
                                 AccentTheme::System);
    Theme t = Theme::variant(mode, accent);
    if (accent == AccentTheme::System)
    {
        tk::apply_system_accent(t, Color::rgb(0xC2185B));
    }
    const Palette& p = t.palette;

    CHECK_FALSE(t.high_contrast);
    CHECK(p.link == p.accent);
    CHECK(p.text_disabled == p.text_muted);
    CHECK(p.selection_text == p.text_primary);
    CHECK(p.selected_text == p.text_primary);
    CHECK(tk::on_selected(t, Color::rgb(0x123456)) == Color::rgb(0x123456));
    CHECK(p.text_error == Color::rgb(0xCC3333));
    CHECK(p.notice_bg == Color::rgb(0xFFF4D6));
    CHECK(p.notice_border == Color::rgb(0xE0C97A));
    CHECK(p.notice_text == Color::rgb(0x5C4500));
    CHECK(p.warning_bg == Color::rgb(0xFFB300));
    CHECK(p.warning_text == Color::rgb(0x332600));
}

TEST_CASE("accent_foreground is white normally and the palette's in High Contrast",
          "[tk][theme][high_contrast]")
{
    CHECK(tk::accent_foreground(Theme::dark()) == Color{255, 255, 255, 255});
    const Theme hc = tk::make_high_contrast_theme(hc_black());
    CHECK(tk::accent_foreground(hc) == hc.palette.text_on_accent);
}

TEST_CASE("hot_row_fill is subtle_hover normally and Highlight in High Contrast",
          "[tk][theme][high_contrast]")
{
    const Theme& dark = Theme::dark();
    CHECK(tk::hot_row_fill(dark) == dark.palette.subtle_hover);

    const auto hc = GENERATE(hc_black(), hc_white());
    const Theme t = tk::make_high_contrast_theme(hc);
    CHECK(tk::hot_row_fill(t) == t.palette.sidebar_selected);
    CHECK(tk::hot_row_fill(t) != t.palette.bg);
}
