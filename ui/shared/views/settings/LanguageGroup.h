#pragma once

// Settings group: language selection via a combobox, with a "Restart now"
// row shown while the saved language differs from the running one. Hosted by
// GeneralSection.

#include "RestartNotice.h"
#include "SettingBinder.h"
#include "SettingsGroup.h"
#include "tk/combobox.h"

#include <functional>

namespace tesseract::views
{

class LanguageGroup : public SettingsGroup
{
public:
    explicit LanguageGroup(SettingBinder& binder);
    ~LanguageGroup() override = default;

    // Show the "Restart now" button — set while the saved language differs
    // from the one this process started with.
    void set_restart_pending(bool pending);

    // Fires when the user presses "Restart now".
    std::function<void()> on_restart_requested;

private:
    tk::ComboBox* combo_ = nullptr;
    RestartNotice* notice_ = nullptr;
};

} // namespace tesseract::views
