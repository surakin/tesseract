#pragma once

// Thin banner shown above the message list when a room has pinned events.
// Displays the currently-selected pin (sender + body preview), with chevrons
// + an "i/n" counter to step through multiple pins. Click on the body fires
// on_jump_to(event_id) so RoomView can scroll the main list to that event.
//
// Patterned after ThreadListView — same tk::Widget conventions, no
// request_repaint_/request_relayout_ plumbing (the toolkit doesn't expose
// those on Widget; the parent re-arranges when set_pins() changes the
// measured height from 0 → kBannerH or vice-versa via the next layout pass).

#include "tk/canvas.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class PinnedBanner : public tk::Widget
{
public:
    PinnedBanner();
    ~PinnedBanner() override = default;

    /// Replace the pin set. Clamps current_index_ to a valid range (or 0
    /// when empty). Banner widget is effectively hidden (zero-height in
    /// measure) when empty.
    void set_pins(std::vector<tesseract::PinnedEvent> pins);
    const std::vector<tesseract::PinnedEvent>& pins() const { return pins_; }

    /// Currently displayed pin index. 0 when empty.
    std::size_t current_index() const { return current_index_; }

    /// Fired when the user clicks the banner body. event_id is from the
    /// currently-displayed pin.
    std::function<void(const std::string& event_id)> on_jump_to;

    // tk::Widget overrides — match the signatures used by ThreadListView.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;

    // Layout constants exposed for tests.
    static constexpr float kBannerH    = 44.0f;
    static constexpr float kChevronSz  = 20.0f;
    static constexpr float kChevronPad = 4.0f;
    static constexpr float kCounterW   = 36.0f;
    static constexpr float kPadX       = 12.0f;

private:
    std::vector<tesseract::PinnedEvent> pins_;
    std::size_t current_index_ = 0;
    // Layout rects (world-space, refreshed on arrange).
    tk::Rect body_rect_{};
    tk::Rect up_rect_{};
    tk::Rect down_rect_{};
    bool press_body_ = false;
    bool press_up_   = false;
    bool press_down_ = false;
};

} // namespace tesseract::views
