#pragma once

// Settings panel section: two labelled checkbox rows —
//   1. "Enable notifications on this device"
//   2. "Show image & sticker previews in notifications"
// Reads initial state from Settings::instance() and fires the matching
// callback when a row is toggled. (The lock-screen privacy gate is always
// on regardless of row 2 — see ShellBase::notification_image_allowed_.)

#include "tk/controls.h"

#include <functional>

namespace tesseract::views
{

class NotificationsSection : public tk::Widget
{
public:
    NotificationsSection();
    ~NotificationsSection() override = default;

    // Silently update checkbox state without firing callbacks.
    void set_checked(bool enabled);                // row 1
    void set_image_previews_checked(bool enabled); // row 2

    // Fire with the new boolean state when the matching row is toggled.
    std::function<void(bool)> on_notifications_changed;
    std::function<void(bool)> on_image_previews_changed;

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    tk::CheckButton* notif_cb_    = nullptr; // row 1
    tk::CheckButton* previews_cb_ = nullptr; // row 2
};

} // namespace tesseract::views
