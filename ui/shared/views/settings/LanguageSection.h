#pragma once

// Settings panel section: language selection via a combobox.

#include "SettingsPage.h"
#include "tk/combobox.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class LanguageSection : public SettingsPage
{
public:
    LanguageSection();
    ~LanguageSection() override = default;

    // Silently update the displayed selection without firing on_language_changed.
    void set_selected(const std::string& lang);

    // Show the "Restart now" button — set while the saved language differs
    // from the one this process started with.
    void set_restart_pending(bool pending);

    // Fires with the newly selected language code when the user picks an option.
    std::function<void(std::string)> on_language_changed;

    // Fires when the user presses "Restart now".
    std::function<void()> on_restart_requested;

private:
    tk::ComboBox* combo_ = nullptr;
    tk::Widget* restart_row_ = nullptr;
};

} // namespace tesseract::views
