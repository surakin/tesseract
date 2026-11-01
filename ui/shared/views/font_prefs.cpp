#include "views/font_prefs.h"

namespace tesseract
{

static_assert(Settings::FontPrefs::kCategoryCount == tk::kFontCategoryCount,
              "Settings::FontPrefs categories must mirror tk::FontCategory");

static_assert(Settings::FontPrefs::kBaseMinPt == tk::kFontBaseMinPt &&
                  Settings::FontPrefs::kBaseMaxPt == tk::kFontBaseMaxPt &&
                  Settings::FontPrefs::kDeltaMinPt == tk::kFontDeltaMinPt &&
                  Settings::FontPrefs::kDeltaMaxPt == tk::kFontDeltaMaxPt,
              "Settings::FontPrefs clamp ranges must match tk::FontConfig's");

tk::FontConfig to_font_config(const Settings::FontPrefs& prefs)
{
    tk::FontConfig cfg;
    cfg.base_pt = prefs.base_pt;
    cfg.ui_family = prefs.ui_family;
    cfg.mono_family = prefs.mono_family;
    for (std::size_t i = 0; i < tk::kFontCategoryCount; ++i)
    {
        cfg.delta_pt[i] = prefs.categories[i].delta_pt;
        cfg.family[i] = prefs.categories[i].family;
    }
    return cfg;
}

Settings::FontPrefs to_font_prefs(const tk::FontConfig& cfg)
{
    Settings::FontPrefs prefs;
    prefs.base_pt = cfg.base_pt;
    prefs.ui_family = cfg.ui_family;
    prefs.mono_family = cfg.mono_family;
    for (std::size_t i = 0; i < tk::kFontCategoryCount; ++i)
    {
        prefs.categories[i].delta_pt = cfg.delta_pt[i];
        prefs.categories[i].family = cfg.family[i];
    }
    return prefs;
}

bool apply_font_prefs(const Settings::FontPrefs& prefs)
{
    const auto before = tk::font_config_generation();
    tk::set_font_config(to_font_config(prefs));
    return tk::font_config_generation() != before;
}

} // namespace tesseract
