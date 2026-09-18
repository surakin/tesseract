#pragma once

// The "Bridge" tab of RoomSettingsView — only shown for a room MSC2346
// auto-detected as bridged (see RoomSettingsView::open()'s
// tabs_->set_tab_visible(kBridgeTabIdx, info.is_bridged) call). Holds the
// "this room isn't actually bridged" override: a local-only account-data
// preference (im.gnomos.tesseract), not a room state event, so it applies
// immediately on toggle rather than being staged for Accept/Cancel like
// every other field in this dialog — see RoomSettingsView::
// on_bridge_override_changed's doc comment.

#include "SettingsPage.h"

#include "tk/controls.h"

#include <functional>

namespace tesseract::views
{

class RoomBridgeSection : public SettingsPage
{
public:
    RoomBridgeSection();
    ~RoomBridgeSection() override = default;

    void set_override(bool not_bridged); // checked state, silent seed

    // Fired immediately on toggle (not staged) — see the class comment above.
    std::function<void(bool)> on_override_changed;

    tk::CheckButton* override_checkbox() const { return override_check_; }

private:
    tk::CheckButton* override_check_ = nullptr;
};

} // namespace tesseract::views
