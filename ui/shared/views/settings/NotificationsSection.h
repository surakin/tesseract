#pragma once

// Settings panel section: notification toggle.
// Renders a single labelled checkbox row: "Enable notifications on this
// device". Reads the initial state from Settings::instance() and fires
// on_notifications_changed when the user toggles it.

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

    // Fires with the new boolean state when the checkbox is toggled.
    std::function<void(bool)> on_notifications_changed;

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up  (tk::Point local, bool inside_self) override;
    void on_pointer_move(tk::Point local) override;
    void on_pointer_leave()               override;

private:
    // Tick/check mark drawn inside the checked box: a simple two-segment
    // polyline — "L"-shaped, rotated: bottom-left → mid-bottom → top-right.
    void draw_checkmark(tk::Canvas& canvas, tk::Rect box, tk::Color ink) const;

    // Returns true when `local` falls inside the hit target (box + label row).
    bool hit_row(tk::Point local) const;

    bool checked_;   // tracks live toggle state
    bool hovered_ = false;
    bool pressed_ = false;

    std::unique_ptr<tk::TextLayout> label_layout_;
};

} // namespace tesseract::views
