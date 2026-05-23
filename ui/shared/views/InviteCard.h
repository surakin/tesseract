#pragma once

// InviteCard — full-panel widget shown when the selected room list entry is
// a pending room invitation.
//
// Two variants driven by InviteInfo::is_direct:
//
//   DM variant (is_direct == true):
//     80 px avatar of the inviter, display name, @user_id, then three buttons:
//     Accept (primary), Decline (subtle), Block (destructive).
//
//   Group variant (is_direct == false):
//     64 px avatar of the room, room name, topic (1-line ellipsis), "Invited
//     by <name>" line, then two buttons: Accept (primary), Decline (subtle).
//
// The widget is invisible (paints nothing, captures no input) until
// set_invite() has been called. Call clear() to return to that state.
//
// Avatar lookup: on each paint, the provider is called once with the mxc URL;
// it returns the decoded image from the shell's avatar cache, or nullptr on a
// cache miss. The shell kicks an async fetch on miss and triggers a surface
// repaint when the bytes arrive (same pattern as UserProfilePanel and
// JoinRoomView).

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <optional>
#include <string>

namespace tesseract::views
{

class InviteCard : public tk::Widget
{
public:
    // Synchronous avatar lookup — returns from the shell's tk_avatars_ cache.
    // Matches the typedef used by UserProfilePanel and JoinRoomView.
    using ImageProvider = std::function<const tk::Image*(const std::string& mxc)>;

    InviteCard();
    ~InviteCard() override = default;

    // Populate the card and make it visible.
    void set_invite(const tesseract::InviteInfo& info, ImageProvider provider);

    // Clear all state; widget becomes invisible.
    void clear();

    bool has_invite() const { return invite_.has_value(); }

    // Accept / Decline fire for both DM and group invitations.
    std::function<void()> on_accept;
    std::function<void()> on_decline;
    // Block fires only in the DM variant (is_direct == true).
    std::function<void()> on_block;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

private:
    std::optional<tesseract::InviteInfo> invite_;
    ImageProvider image_provider_;

    // Child widgets (borrowed — owned by widget tree via add_child).
    tk::Button* accept_btn_  = nullptr;
    tk::Button* decline_btn_ = nullptr;
    tk::Button* block_btn_   = nullptr; // only shown in DM variant

    // Cached text layouts (rebuilt lazily in paint when invite changes).
    mutable std::unique_ptr<tk::TextLayout> name_layout_;
    mutable std::unique_ptr<tk::TextLayout> secondary_layout_;  // @uid or topic
    mutable std::unique_ptr<tk::TextLayout> invited_by_layout_; // group variant only

    // Layout constants
    static constexpr float kMinW       = 320.0f;
    static constexpr float kMinH       = 280.0f;
    static constexpr float kAvatarD_DM = 80.0f;
    static constexpr float kAvatarD_GR = 64.0f;
    static constexpr float kPadX       = 24.0f;
    static constexpr float kPadY       = 20.0f;
    static constexpr float kGap        = 8.0f;
    static constexpr float kBtnH       = 36.0f;
    static constexpr float kBtnGap     = 8.0f;
    static constexpr float kContentW   = 280.0f;

    void reset_layouts();
};

} // namespace tesseract::views
