#pragma once

// Settings panel section: General.
//
// Groups:
//   "Startup" — checkbox to launch Tesseract automatically when the user
//                logs into the OS
//
// Reads initial state from Settings::instance(), but the shell re-pushes the
// actual OS-queried state via set_launch_at_login() on settings-open (see
// tesseract::IAutostart) since the OS registration is the source of truth,
// not the persisted preference.

#include "SettingsPage.h"

#include "tk/controls.h"

#include <functional>

namespace tesseract::views
{

class GeneralSection : public SettingsPage
{
public:
    GeneralSection();
    ~GeneralSection() override = default;

    // Silently update the "launch at login" checkbox without firing the
    // callback. Called by the shell on settings-open (sourced from
    // IAutostart::is_enabled(), not the persisted Settings bool) and after a
    // failed toggle to reflect the actual OS state.
    void set_launch_at_login(bool enabled);

    // Fired with the new state when the "launch at login" checkbox is toggled.
    std::function<void(bool)> on_launch_at_login_changed;

private:
    tk::CheckButton* launch_at_login_cb_ = nullptr;
};

} // namespace tesseract::views
