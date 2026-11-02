#pragma once

// Settings panel section: hidden "Advanced" tab, revealed via the "Advanced"
// button on the About tab. Holds an "Advanced" group with a checkbox
// governing MSC2545 image-pack historical compatibility (dual
// stable/unstable event-type reads+writes for room packs and the
// emote-rooms subscription list, plus whether the personal pack is loaded
// at all), a "Developer" group with a developer-mode checkbox (off by
// default; gates Copy event source) and a dependent "Show unhandled events in
// the timeline" checkbox (greyed out while developer mode is off), and — only in builds compiled with
// TESSERACT_CRASH_HANDLER_ENABLED (experimental, off by default) — a
// "Diagnostics" group with a crash-reporting checkbox.

#include "SettingBinder.h"
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
    explicit AdvancedSection(SettingBinder& binder);
    ~AdvancedSection() override = default;

    void paint_before_children(tk::PaintCtx& ctx) override;

    // Fired by the "Open Activity Monitor" button.
    std::function<void()> on_open_activity_monitor;

private:
    tk::CheckButton* legacy_compat_cb_ = nullptr;
    tk::CheckButton* developer_mode_cb_ = nullptr;
    tk::CheckButton* show_unhandled_events_cb_ = nullptr;
#ifdef TESSERACT_CRASH_HANDLER_ENABLED
    tk::CheckButton* crash_reporting_cb_ = nullptr;
#endif
    // Cached from paint() so the checkbox's hover callbacks (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;
};

} // namespace tesseract::views
