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

    // Fires with the newly selected language code when the user picks an option.
    std::function<void(std::string)> on_language_changed;

    // Constrain the combobox dropdown popup to the page bounds.
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;

private:
    tk::ComboBox* combo_ = nullptr;
};

} // namespace tesseract::views
