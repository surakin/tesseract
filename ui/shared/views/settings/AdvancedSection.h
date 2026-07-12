#pragma once

// Settings panel section: hidden "Advanced" tab, revealed via the "Advanced"
// button on the About tab. Currently holds a single checkbox governing MSC2545
// image-pack historical compatibility (dual stable/unstable event-type
// reads+writes for room packs and the emote-rooms subscription list, plus
// whether the personal pack is loaded at all).

#include "SettingsPage.h"

#include "tk/controls.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class AdvancedSection : public SettingsPage
{
public:
    AdvancedSection();
    ~AdvancedSection() override = default;

    // Silently update checkbox state without firing on_msc2545_legacy_compat_changed.
    void set_msc2545_legacy_compat(bool enabled);

    // Fired with the new boolean state when the checkbox is toggled.
    std::function<void(bool)> on_msc2545_legacy_compat_changed;

    // Tooltip callbacks — bubbled up from the checkbox. Wire the same way
    // AboutSection's tooltip callbacks are wired.
    std::function<void(std::string text, tk::Rect anchor)> on_show_tooltip;
    std::function<void()> on_hide_tooltip;

private:
    tk::CheckButton* legacy_compat_cb_ = nullptr;
};

} // namespace tesseract::views
