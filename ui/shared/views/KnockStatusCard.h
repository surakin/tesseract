#pragma once

// KnockStatusCard — full-panel widget shown when the selected room-list
// entry is one of the current user's pending knock requests (MSC2403),
// i.e. a "Requests to Join" row. Modeled directly on InviteCard, but with
// a single variant (room avatar/name/topic — a knock always targets a
// room, never a person) and a single "Cancel Request" action in place of
// InviteCard's Accept/Decline/Block.
//
// The widget is invisible (paints nothing, captures no input) until
// set_knock() has been called. Call clear() to return to that state.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <optional>
#include <string>

namespace tesseract::views
{

class KnockStatusCard : public tk::Widget
{
public:
    // Synchronous avatar lookup — same contract as InviteCard::ImageProvider.
    using ImageProvider = std::function<const tk::Image*(const std::string& mxc)>;

    KnockStatusCard();
    ~KnockStatusCard() override = default;

    // Populate the card and make it visible.
    void set_knock(const tesseract::KnockedRoomInfo& info, ImageProvider provider);

    // Clear all state; widget becomes invisible.
    void clear();

    bool has_knock() const { return knock_.has_value(); }

    // Fires when the user clicks "Cancel Request".
    std::function<void()> on_cancel;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint_before_children(tk::PaintCtx&) override;

private:
    std::optional<tesseract::KnockedRoomInfo> knock_;
    ImageProvider image_provider_;

    // Child widget (borrowed — owned by widget tree via add_child).
    tk::Button* cancel_btn_ = nullptr;

    // Cached text layouts (rebuilt lazily in paint when the knock changes).
    mutable std::unique_ptr<tk::TextLayout> name_layout_;
    mutable std::unique_ptr<tk::TextLayout> topic_layout_;
    mutable std::unique_ptr<tk::TextLayout> status_layout_;
    mutable std::unique_ptr<tk::TextLayout> reason_layout_; // shown when non-empty

    // Layout constants — mirror InviteCard's group variant.
    static constexpr float kMinW     = 320.0f;
    static constexpr float kMinH     = 280.0f;
    static constexpr float kAvatarD  = 64.0f;
    static constexpr float kPadX     = 24.0f;
    static constexpr float kPadY     = 20.0f;
    static constexpr float kGap      = 8.0f;
    static constexpr float kBtnH     = 36.0f;
    static constexpr float kContentW = 280.0f;

    void reset_layouts();
};

} // namespace tesseract::views
