#pragma once

// Settings panel section: theme selection. A SettingsPage with one "Theme"
// group whose body is the three-radio ThemePicker widget. The picker handles
// hit-testing and rendering; the section forwards its public API.

#include "SettingsPage.h"

#include "tesseract/settings.h"

#include <functional>

namespace tesseract::views
{

class AppearanceSection : public SettingsPage
{
public:
    AppearanceSection();
    ~AppearanceSection() override;

    // Silently update the displayed selection without firing on_theme_changed.
    void set_selected(tesseract::Settings::ThemePreference pref);

    // Fires with the newly selected preference when the user picks a button.
    std::function<void(tesseract::Settings::ThemePreference)> on_theme_changed;

private:
    class ThemePicker; // defined in AppearanceSection.cpp
    ThemePicker* picker_ = nullptr;
};

} // namespace tesseract::views
