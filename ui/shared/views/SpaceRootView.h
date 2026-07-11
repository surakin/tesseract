#pragma once

// SpaceRootView — full-panel widget shown in the chat area for a Matrix space.
// Mirrors RoomPreviewView's centred summary card, but describes the joined
// space and its child counts rather than offering a join action.

#include "RoomSettingsView.h"

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <optional>
#include <string>

namespace tesseract::views
{

class SpaceRootView : public tk::Widget
{
public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc)>;

    SpaceRootView();
    ~SpaceRootView() override = default;

    void set_space(const tesseract::RoomInfo& space,
                   std::size_t joined_children,
                   std::size_t unjoined_children);
    void clear();
    void set_avatar_provider(AvatarProvider p);

    // Shell should kick an avatar fetch on miss and repaint.
    std::function<void(const std::string& mxc)> on_avatar_needed;

    // Owned settings overlay opened via the wrench icon (top-left, mirrors
    // RoomInfoPanel's). Replaces this view's own summary content entirely
    // while open — see arrange()/paint().
    RoomSettingsView* settings_view() const { return settings_view_; }

    // Wire the shell's post_delayed provider so settings_view_'s Room ID
    // copy-toast can auto-dismiss itself (mirrors RoomView::set_post_delayed).
    void set_post_delayed(std::function<void(int, std::function<void()>)> f);

    // Fired when settings_view_'s own layout-affecting state changes (open/
    // close, tab switches) so the shell can relayout native overlays.
    std::function<void()> on_layout_changed;
    // Fired once the wrench opens settings_view_, and once more from its
    // avatar-upload request — both carry the space's room id so the shell's
    // existing per-room-id settings plumbing (permission gating,
    // ShellBase::apply_room_settings_, avatar upload staging) can be reused
    // unchanged for spaces.
    std::function<void(std::string space_id)> on_settings_opened;
    std::function<void(std::string space_id)> on_settings_avatar_upload_requested;
    // Fired when the user clicks the Room ID row in settings_view_; the
    // shell performs the actual clipboard write (mirrors RoomSettingsView's
    // own on_copy_to_clipboard, forwarded through unchanged).
    std::function<void(std::string)> on_copy_to_clipboard;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;

private:
    std::optional<tesseract::RoomInfo> space_;
    std::size_t joined_children_ = 0;
    std::size_t unjoined_children_ = 0;
    AvatarProvider avatar_provider_;

    tk::Button*       settings_btn_  = nullptr;
    tk::IconCache     settings_icon_;
    RoomSettingsView* settings_view_ = nullptr;

    mutable std::unique_ptr<tk::TextLayout> name_layout_;
    mutable std::unique_ptr<tk::TextLayout> alias_layout_;
    mutable std::unique_ptr<tk::TextLayout> topic_layout_;
    mutable std::unique_ptr<tk::TextLayout> meta_layout_;
    mutable std::unique_ptr<tk::TextLayout> hint_layout_;
    mutable tk::CanvasFactory* factory_seen_ = nullptr;
    mutable float last_bounds_h_ = -1.0f;
    mutable float last_content_w_ = -1.0f;

    static constexpr float kAvatarD = 72.0f;
    static constexpr float kContentW = 340.0f;
    static constexpr float kPadY = 32.0f;
    static constexpr float kGap = 12.0f;

    void reset_layouts_();
    std::string child_count_label_() const;
};

} // namespace tesseract::views
