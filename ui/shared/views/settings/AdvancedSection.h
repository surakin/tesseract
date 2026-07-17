#pragma once

// Settings panel section: hidden "Advanced" tab, revealed via the "Advanced"
// button on the About tab. Holds an "Advanced" group with a checkbox
// governing MSC2545 image-pack historical compatibility (dual
// stable/unstable event-type reads+writes for room packs and the
// emote-rooms subscription list, plus whether the personal pack is loaded
// at all), and a "Developer" group with a developer-mode checkbox (off by
// default; no behavior gated on it yet).

#include "SettingsPage.h"

#include "tk/controls.h"
#include "tk/host.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class AdvancedSection : public SettingsPage
{
public:
    AdvancedSection();
    ~AdvancedSection() override = default;

    void paint(tk::PaintCtx& ctx) override;

    // Silently update checkbox state without firing on_msc2545_legacy_compat_changed.
    void set_msc2545_legacy_compat(bool enabled);

    // Fired with the new boolean state when the checkbox is toggled.
    std::function<void(bool)> on_msc2545_legacy_compat_changed;

    // Silently update checkbox state without firing on_developer_mode_changed.
    void set_developer_mode(bool enabled);

    // Fired with the new boolean state when the checkbox is toggled.
    std::function<void(bool)> on_developer_mode_changed;

private:
    tk::CheckButton* legacy_compat_cb_ = nullptr;
    tk::CheckButton* developer_mode_cb_ = nullptr;
    // Cached from paint() so the checkbox's hover callbacks (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;
};

} // namespace tesseract::views
