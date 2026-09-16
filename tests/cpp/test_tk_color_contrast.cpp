#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/color_contrast.h"

#include <cstdlib>

using tk::Color;
using Catch::Approx;

TEST_CASE("relative_luminance matches known WCAG reference values", "[tk][color_contrast]")
{
    CHECK(tk::relative_luminance(Color::rgb(0x000000)) == Approx(0.0).margin(0.001));
    CHECK(tk::relative_luminance(Color::rgb(0xFFFFFF)) == Approx(1.0).margin(0.001));
    // #767676 is the WCAG worked example for a color at exactly 4.5:1 against
    // white -- verified independently against the formula, not hardcoded from
    // a remembered figure (relative_luminance(#767676) == ~0.1812).
    CHECK(tk::relative_luminance(Color::rgb(0x767676)) == Approx(0.1812).margin(0.001));
}

TEST_CASE("contrast_ratio is symmetric and matches known pairs", "[tk][color_contrast]")
{
    const Color black = Color::rgb(0x000000);
    const Color white = Color::rgb(0xFFFFFF);
    CHECK(tk::contrast_ratio(black, white) == Approx(21.0).margin(0.01));
    CHECK(tk::contrast_ratio(black, white) == Approx(tk::contrast_ratio(white, black)));
    CHECK(tk::contrast_ratio(white, white) == Approx(1.0).margin(0.001));
}

TEST_CASE("composite_over blends a translucent color over its backdrop", "[tk][color_contrast]")
{
    const Color half_black = Color::rgba(0, 0, 0, 128);
    const Color white_bg = Color::rgb(0xFFFFFF);
    const Color result = tk::composite_over(half_black, white_bg);
    CHECK(result.r < 140);
    CHECK(result.r > 110);
    CHECK(result.a == 255);

    const Color opaque_red = Color::rgb(0xFF0000);
    CHECK(tk::composite_over(opaque_red, white_bg) == opaque_red);
}

TEST_CASE("meets_wcag_aa applies the correct threshold per level", "[tk][color_contrast]")
{
    const Color black = Color::rgb(0x000000);
    const Color white = Color::rgb(0xFFFFFF);
    CHECK(tk::meets_wcag_aa(black, white, tk::ContrastLevel::Text));
    CHECK(tk::meets_wcag_aa(black, white, tk::ContrastLevel::UI));

    // 0x777777 on white is ~4.47:1 -- just under the 4.5:1 text floor but
    // comfortably clears the 3:1 UI floor.
    const Color borderline = Color::rgb(0x777777);
    CHECK_FALSE(tk::meets_wcag_aa(borderline, white, tk::ContrastLevel::Text));
    CHECK(tk::meets_wcag_aa(borderline, white, tk::ContrastLevel::UI));
}

TEST_CASE("Color::from_hsl reproduces the calibrated Blue accent literals", "[tk][color_contrast]")
{
    struct Case
    {
        float h, s, l;
        std::uint32_t expected_hex;
    };
    // Reverse-engineered from the pre-existing hand-picked hex literals in
    // theme.cpp's light/dark palettes (hue 211 == the "Blue" accent).
    const Case cases[] = {
        {211.0f, 1.00f, 0.465f, 0x0072ED}, // light accent
        {211.0f, 1.00f, 0.651f, 0x4DA3FF}, // dark accent
        {211.0f, 1.00f, 0.906f, 0xCFE3FF}, // light chip_bg_me
        {211.0f, 1.00f, 0.947f, 0xE4F0FF}, // light bubble_bg_me
    };
    for (const auto& c : cases)
    {
        const Color got = Color::from_hsl(c.h, c.s, c.l);
        const Color want = Color::rgb(c.expected_hex);
        // Allow a couple of RGB units of rounding slop per channel.
        CHECK(std::abs(int(got.r) - int(want.r)) <= 3);
        CHECK(std::abs(int(got.g) - int(want.g)) <= 3);
        CHECK(std::abs(int(got.b) - int(want.b)) <= 3);
    }
}

TEST_CASE("Color::from_hsl round-trips primary/secondary hues", "[tk][color_contrast]")
{
    CHECK(Color::from_hsl(0.0f, 1.0f, 0.5f) == Color::rgb(0xFF0000));
    CHECK(Color::from_hsl(120.0f, 1.0f, 0.5f) == Color::rgb(0x00FF00));
    CHECK(Color::from_hsl(240.0f, 1.0f, 0.5f) == Color::rgb(0x0000FF));
}
