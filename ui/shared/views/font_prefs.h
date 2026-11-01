#pragma once

// Bridge between the persisted Settings::FontPrefs and the toolkit's active
// tk::FontConfig (tk can't see Settings). Shared by launch and live changes.

#include "tesseract/settings.h"
#include "tk/font_config.h"

namespace tesseract
{

tk::FontConfig to_font_config(const Settings::FontPrefs& prefs);
Settings::FontPrefs to_font_prefs(const tk::FontConfig& cfg);

// Pushes `prefs` into tk::set_font_config(). Returns true if the effective
// config changed (i.e. caches need flushing).
bool apply_font_prefs(const Settings::FontPrefs& prefs);

} // namespace tesseract
