#pragma once

// Settings panel section: two labelled checkbox rows —
//   1. "Enable notifications on this device"
//   2. "Show image & sticker previews in notifications"
// Reads initial state from Settings::instance() and fires the matching
// callback when a row is toggled. (The lock-screen privacy gate is always
// on regardless of row 2 — see ShellBase::notification_image_allowed_.)

#include "tk/widget.h"

#include <functional>
#include <memory>

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

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    void draw_checkmark(tk::Canvas& canvas, tk::Rect box, tk::Color ink) const;

    // 0 = notifications row, 1 = image-previews row, -1 = none.
    int row_at(tk::Point local) const;
    void paint_row(tk::PaintCtx& ctx, int row, float y, bool checked,
                   const std::string& label, std::unique_ptr<tk::TextLayout>& cache);

    bool checked_;          // row 1 — notifications_enabled
    bool previews_checked_; // row 2 — notification_image_previews
    int hovered_row_ = -1;
    int pressed_row_ = -1;

    std::unique_ptr<tk::TextLayout> label_layout_;          // row 1
    std::unique_ptr<tk::TextLayout> previews_label_layout_; // row 2
};

} // namespace tesseract::views
