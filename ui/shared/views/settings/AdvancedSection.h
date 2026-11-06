#pragma once

// Settings panel section: hidden "Advanced" tab, revealed via the "Advanced"
// button on the About tab. Holds a single "Developer" group: a developer-mode
// checkbox (off by default; gates Copy event source), a dependent "Show
// unhandled events in the timeline" checkbox (greyed out while developer mode
// is off) and the "Open Activity Monitor" button.

#include "SettingBinder.h"
#include "SettingsPage.h"

#include "tk/controls.h"

#include <functional>

namespace tesseract::views
{

class AdvancedSection : public SettingsPage
{
public:
    explicit AdvancedSection(SettingBinder& binder);
    ~AdvancedSection() override = default;

    // Fired by the "Open Activity Monitor" button.
    std::function<void()> on_open_activity_monitor;

private:
    tk::CheckButton* developer_mode_cb_ = nullptr;
    tk::CheckButton* show_unhandled_events_cb_ = nullptr;
};

} // namespace tesseract::views
