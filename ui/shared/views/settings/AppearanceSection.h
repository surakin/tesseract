#pragma once

// Settings panel section: theme selection + room-list grouping options.
// A SettingsPage with a "Theme" group (three-radio ThemePicker) and a
// "Room list" group (checkbox + period combobox).

#include "SettingsPage.h"

#include "tesseract/settings.h"
#include "tk/combobox.h"
#include "tk/controls.h"

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

    // ----- Room list group -----
    // Silently update the controls without firing the callbacks below.
    void set_group_inactive(bool enabled);
    void set_inactive_period(int days);

    // Fired when the user toggles grouping / changes the period.
    std::function<void(bool)> on_group_inactive_changed;
    std::function<void(int)>  on_inactive_period_changed;

    // Constrain the period combobox dropdown popup to the page bounds.
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;

private:
    class ThemePicker; // defined in AppearanceSection.cpp
    ThemePicker*     picker_           = nullptr;
    tk::CheckButton* group_inactive_cb_ = nullptr;
    tk::ComboBox*    period_combo_      = nullptr;
};

} // namespace tesseract::views
