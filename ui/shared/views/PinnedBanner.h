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
#include "tk/controls.h"
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
    const std::vector<tesseract::PinnedEvent>& pins() const
    {
        return pins_;
    }

    /// Currently displayed pin index. 0 when empty.
    std::size_t current_index() const
    {
        return current_index_;
    }

    /// Fired when the user clicks the banner body. event_id is from the
    /// currently-displayed pin.
    std::function<void(const std::string& event_id)> on_jump_to;

    // tk::Widget overrides — match the signatures used by ThreadListView.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    // A "Pinned messages" group ("2 of 5" as its description when there are
    // several); the body and the previous/next chevrons are real buttons.
    tk::Role access_role() const override
    {
        return pins_.empty() ? tk::Role::None : tk::Role::Group;
    }
    std::string access_name() const override;
    std::string access_description() const override;

    // The "<sender>: <body>" line shown for the current pin.
    std::string current_preview() const;

    // Test hooks: the three buttons (null-safe accessors).
    tk::Button* body_button() const
    {
        return body_btn_;
    }
    tk::Button* previous_button() const
    {
        return up_btn_;
    }
    tk::Button* next_button() const
    {
        return down_btn_;
    }

    // Layout constants exposed for tests.
    static constexpr float kBannerH = 44.0f;
    static constexpr float kChevronSz = 20.0f;
    static constexpr float kChevronPad = 4.0f;
    static constexpr float kCounterW = 36.0f;
    static constexpr float kPadX = 12.0f;

private:
    std::vector<tesseract::PinnedEvent> pins_;
    std::size_t current_index_ = 0;
    // Body rect (world-space, refreshed on arrange) — the preview text is
    // painted over body_btn_'s hover/press fill.
    tk::Rect body_rect_{};
    tk::Button* body_btn_ = nullptr; // jump to the current pin
    tk::Button* up_btn_ = nullptr;   // previous pin
    tk::Button* down_btn_ = nullptr; // next pin

    void step_(int delta);
    void sync_buttons_();
};

} // namespace tesseract::views
