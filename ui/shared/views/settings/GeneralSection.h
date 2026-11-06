#pragma once

// Settings panel section: General.
//
// Groups:
//   "Startup"     — checkbox to launch Tesseract automatically when the user
//                   logs into the OS, checkbox to start every launch minimized
//                   to the tray, and a dropdown for what closing the window does
//   "Composer"    — checkbox to auto-replace text emoticons (":-)") with emoji
//   "Power"       — Low power mode: Auto / On / Off
//
// Reads initial state from Settings::instance(), but the shell re-pushes the
// actual OS-queried state via set_launch_at_login() on settings-open (see
// tesseract::IAutostart) since the OS registration is the source of truth,
// not the persisted preference.

#include "SettingBinder.h"
#include "SettingsPage.h"

#include "tesseract/settings.h"
#include "tk/controls.h"

#include <functional>

namespace tk
{
class ComboBox;
}

namespace tesseract::views
{
class SettingsGroup;
}

namespace tesseract::views
{

class GeneralSection : public SettingsPage
{
public:
    explicit GeneralSection(SettingBinder& binder);
    ~GeneralSection() override = default;

    // Silently update the "launch at login" checkbox without firing the
    // callback. Called by the shell on settings-open (sourced from
    // IAutostart::is_enabled(), not the persisted Settings bool) and after a
    // failed toggle to reflect the actual OS state.
    void set_launch_at_login(bool enabled);

    // Report whether a system tray icon actually exists on this machine, and
    // rebuild the close-action dropdown accordingly. Without a tray icon,
    // CloseAction::HideToTray would be a trap — the window would hide with no
    // way to bring it back — so that option is dropped from the list and the
    // setting silently degrades to a real quit, matching what the shells do
    // at runtime (ITrayIcon::is_available()).
    void set_tray_available(bool available);

    // Show / hide the whole "Power" group. Hidden by default — the shell shows
    // it only on a machine that has a battery (IPowerMonitor::has_battery()).
    void set_low_power_available(bool available);

private:
    // Rebuild close_action_combo_'s options for the current tray availability.
    void rebuild_close_action_options_();
    // Resolve an unoffered "tray" selection to "quit" and refresh the
    // description under the dropdown from the current selection.
    void refresh_close_action_();

    tk::CheckButton* launch_at_login_cb_ = nullptr;
    tk::ComboBox* close_action_combo_ = nullptr;
    tk::Label* close_action_desc_ = nullptr;
    bool tray_available_ = true;
    SettingsGroup* power_group_ = nullptr;
    tk::ComboBox* low_power_combo_ = nullptr;
    tk::Label* low_power_desc_ = nullptr;
};

} // namespace tesseract::views
