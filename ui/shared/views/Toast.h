#pragma once

// A small, non-modal, self-dismissing notification pill — e.g. "Copied to
// clipboard". Unlike ConfirmDialog it has no backdrop and no buttons, so it
// never intercepts pointer input to whatever it's drawn over (no
// on_pointer_down override — Widget's default no-op dispatch lets clicks
// pass straight through). The owner is responsible for scheduling hide()
// after a delay (see RoomSettingsView's on_room_id_clicked wiring for the
// canonical use).

#include "tk/canvas.h"
#include "tk/widget.h"

#include <memory>
#include <string>

namespace tesseract::views
{

class Toast : public tk::Widget
{
public:
    Toast();
    ~Toast() override = default;

    // Sets the message and shows the pill. Replaces any message currently
    // showing (no queueing — a second show() while one is up just restarts
    // the display with the new text).
    void show(std::string message);
    void hide();

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

private:
    std::string message_;
    std::unique_ptr<tk::TextLayout> message_layout_;

    static constexpr float kPadX         = 16.0f;
    static constexpr float kPadY         = 10.0f;
    static constexpr float kBottomMargin = 24.0f;
    static constexpr float kRadius       = 8.0f;
};

} // namespace tesseract::views
