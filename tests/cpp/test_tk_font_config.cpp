#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/font_config.h"

using namespace tk;

namespace
{

// Restores the default config so other tests see stock sizes.
struct ConfigGuard
{
    ConfigGuard() { set_font_config({}); }
    ~ConfigGuard() { set_font_config({}); }
};

constexpr std::size_t cat(FontCategory c)
{
    return static_cast<std::size_t>(c);
}

} // namespace

TEST_CASE("font_role_pt defaults reproduce the stock offsets", "[font]")
{
    ConfigGuard g;
    CHECK(font_role_pt(FontRole::Body, 11) == 11);
    CHECK(font_role_pt(FontRole::Caption, 11) == 9);
    CHECK(font_role_pt(FontRole::Title, 11) == 13);
    CHECK(font_role_pt(FontRole::BigEmoji, 11) == 22);
    CHECK(font_role_pt(FontRole::InlineCustomEmoji, 11) == 15);
    CHECK(font_role_pt(FontRole::ReactionEmoji, 11) == 13);
    CHECK(font_role_pt(FontRole::ReactionText, 11) == 10);
    CHECK(font_role_pt(FontRole::Small, 6) == 6); // floor
}

TEST_CASE("base size override replaces the system base, keeping offsets",
          "[font]")
{
    ConfigGuard g;
    FontConfig c;
    c.base_pt = 16;
    set_font_config(c);
    CHECK(font_role_pt(FontRole::Body, 11) == 16);
    CHECK(font_role_pt(FontRole::Caption, 11) == 14);
    CHECK(font_role_pt(FontRole::Title, 11) == 18);
    CHECK(effective_base_pt(11) == 16);
}

TEST_CASE("category deltas apply to their roles only", "[font]")
{
    ConfigGuard g;
    FontConfig c;
    c.delta_pt[cat(FontCategory::Secondary)] = 2;
    set_font_config(c);
    CHECK(font_role_pt(FontRole::Timestamp, 11) == 10);
    CHECK(font_role_pt(FontRole::Small, 11) == 9);
    CHECK(font_role_pt(FontRole::Body, 11) == 11);
    CHECK(font_role_pt(FontRole::Title, 11) == 13);
}

TEST_CASE("inline emoji follows Messages and Emoji deltas", "[font]")
{
    ConfigGuard g;
    FontConfig c;
    c.delta_pt[cat(FontCategory::Messages)] = 3;
    c.delta_pt[cat(FontCategory::Emoji)] = 1;
    set_font_config(c);
    CHECK(font_role_pt(FontRole::InlineEmoji, 11) == 15);
    CHECK(font_role_pt(FontRole::BigEmoji, 11) == 30);
    // Picker cells are independent of the Messages delta.
    CHECK(font_role_pt(FontRole::EmojiPickerCell, 11) == 21);
}

TEST_CASE("set_font_config clamps and bumps the generation only on change",
          "[font]")
{
    ConfigGuard g;
    const auto gen0 = font_config_generation();
    set_font_config({});
    CHECK(font_config_generation() == gen0);

    FontConfig c;
    c.base_pt = 500;
    c.delta_pt[cat(FontCategory::Headings)] = -99;
    set_font_config(c);
    CHECK(font_config_generation() == gen0 + 1);
    CHECK(font_config().base_pt == kFontBaseMaxPt);
    CHECK(font_config().delta_pt[cat(FontCategory::Headings)] ==
          kFontDeltaMinPt);
}

TEST_CASE("font_role_family resolves override, global, and emoji", "[font]")
{
    ConfigGuard g;
    CHECK(font_role_family(FontRole::Body).empty());

    FontConfig c;
    c.ui_family = "Inter";
    c.mono_family = "Fira Code";
    c.family[cat(FontCategory::Headings)] = "Georgia";
    c.family[cat(FontCategory::Emoji)] = "Ignored";
    set_font_config(c);
    CHECK(font_role_family(FontRole::Body) == "Inter");
    CHECK(font_role_family(FontRole::Title) == "Georgia");
    CHECK(font_role_family(FontRole::BigEmoji).empty());
    CHECK(font_mono_family() == "Fira Code");
    CHECK_FALSE(font_config().is_default());
}

TEST_CASE("CachedTextLayout empties itself when the config changes", "[font]")
{
    ConfigGuard g;
    CachedTextLayout slot;
    CHECK_FALSE(slot);
    FontConfig c;
    c.base_pt = 14;
    set_font_config(c);
    CHECK_FALSE(slot);
}

#include "views/font_prefs.h"

TEST_CASE("apply_font_prefs converts and reports changes", "[font]")
{
    ConfigGuard g;
    tesseract::Settings::FontPrefs p;
    CHECK_FALSE(tesseract::apply_font_prefs(p)); // default → default
    p.base_pt = 15;
    p.ui_family = "Inter";
    p.categories[cat(FontCategory::Buttons)].delta_pt = 1;
    CHECK(tesseract::apply_font_prefs(p));
    CHECK_FALSE(tesseract::apply_font_prefs(p));
    CHECK(font_role_pt(FontRole::UiSemibold, 11) == 14);
    CHECK(tesseract::to_font_prefs(font_config()) == p);
}

TEST_CASE("font_height_scale grows with larger fonts and never shrinks",
          "[font]")
{
    ConfigGuard g;
    (void)font_role_pt(FontRole::Body, 11); // teaches the system base
    CHECK(font_height_scale(FontRole::SenderName) == 1.0f);

    FontConfig c;
    c.base_pt = 22;
    set_font_config(c);
    CHECK(font_height_scale(FontRole::SenderName) > 1.5f);

    c.base_pt = 8;
    set_font_config(c);
    CHECK(font_height_scale(FontRole::SenderName) == 1.0f);
}

#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"
#include "views/MainAppWidget.h"
#include "views/UserInfo.h"

TEST_CASE("user strip height is 64 at default fonts and grows with them",
          "[font]")
{
    ConfigGuard g;
    (void)font_role_pt(FontRole::Body, 11); // teaches the system base
    auto surface = TestSurface::create(1000, 700);
    auto app = tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);
    const auto strip_h = [&]
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        app->measure(lc, {1000.0f, 700.0f});
        app->arrange(lc, {0, 0, 1000.0f, 700.0f});
        return app->user_info()->bounds().h;
    };
    CHECK(strip_h() == 64.0f);

    FontConfig c;
    c.base_pt = 22;
    set_font_config(c);
    CHECK(strip_h() > 64.0f);
}
