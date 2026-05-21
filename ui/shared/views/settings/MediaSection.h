#pragma once

// Settings panel section: one labelled checkbox row under a "Media" header —
//   "Pre-load full images while scrolling"
// Reads initial state from Settings::instance() and fires on_prefetch_changed
// when the row is toggled.

#include "SettingsPage.h"

#include "tk/controls.h"

#include <functional>

namespace tesseract::views
{

class MediaSection : public SettingsPage
{
public:
    MediaSection();
    ~MediaSection() override = default;

    // Silently update checkbox state without firing the callback.
    void set_prefetch_checked(bool enabled);

    // Fired with the new boolean state when the row is toggled.
    std::function<void(bool)> on_prefetch_changed;

private:
    tk::CheckButton* prefetch_cb_ = nullptr;
};

} // namespace tesseract::views
