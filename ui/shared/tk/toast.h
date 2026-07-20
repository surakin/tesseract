#pragma once

// A brief, self-dismissing "toast" pill (e.g. "Copied to clipboard") drawn
// above the entire widget tree, bottom-centered within the whole window.
// Owned exclusively by Host: widgets never construct or paint one directly,
// they request one via Host::show_toast() (see host.h). Mirrors tk::Tooltip
// in shape — pure paint, no hit-testing, no widget-tree membership.

#include "animator.h"
#include "canvas.h"
#include "widget.h" // PaintCtx

#include <tesseract/visual.h>

#include <memory>
#include <string>

namespace tk
{

class Toast
{
public:
    // Sets the message to display.
    void set_message(std::string message);

    // Draws the pill bottom-centered within `surface_bounds` (the whole
    // paintable surface, not any one widget's bounds — same role as
    // Tooltip::paint_overlay's parameter). No-op if no message has been set.
    void paint_overlay(PaintCtx& ctx, Rect surface_bounds);

    // True while the entrance reveal (see paint_overlay) is still easing —
    // Host checks this after painting to decide whether to self-schedule
    // another repaint, since Toast has no Host handle of its own.
    bool still_revealing() const
    {
        return reveal_.still_animating();
    }

private:
    std::string message_;
    std::unique_ptr<TextLayout> layout_;
    // layout_ is rebuilt only when the message text changes.
    std::string layout_text_cache_;
    // Grow-from-centre entrance reveal, restarted whenever set_message()
    // sets a genuinely new message (see its early-return guard).
    FloatTween reveal_{1.0f};

    static constexpr float kPadX         = 16.0f;
    static constexpr float kPadY         = 10.0f;
    static constexpr float kBottomMargin = 24.0f;
    static constexpr float kRadius       = tesseract::visual::kRadiusMD;
};

} // namespace tk
