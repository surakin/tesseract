#pragma once

// Settings panel section: Privacy.
//
// Groups:
//   "Privacy"      — checkboxes to send/receive presence and to show avatars
//                    in invites (MSC4278 invite_avatars)
//   "Data sharing" — "Check for updates automatically" (TESSERACT_UPDATE_CHECKS
//                    builds, unless disabled at runtime) and the local crash
//                    report checkbox (TESSERACT_CRASH_HANDLER_ENABLED builds).
//                    The group is omitted when neither is available.
//
// Reads initial state from Settings::instance(); every checkbox is bound
// straight to its field via the SettingBinder.

#include "SettingBinder.h"
#include "SettingsPage.h"

#include "tk/controls.h"
#include "tk/host.h"

namespace tesseract::views
{

class PrivacySection : public SettingsPage
{
public:
    explicit PrivacySection(SettingBinder& binder);
    ~PrivacySection() override = default;

    void paint_before_children(tk::PaintCtx& ctx) override;

private:
    tk::CheckButton* invite_avatars_cb_ = nullptr;
#ifdef TESSERACT_CRASH_HANDLER_ENABLED
    tk::CheckButton* crash_reporting_cb_ = nullptr;
#endif

    // Cached from paint() so the checkbox's hover callbacks (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;
};

} // namespace tesseract::views
