#pragma once

// KnockRequestsPanel — admin-side "Requests to join" panel (MSC2403).
// Swaps into RoomView's overlay-panel slot in place of RoomInfoPanel when
// the user clicks its "Requests to join (N)" row (mirrors how
// RoomSettingsView swaps in for the wrench icon; see RoomInfoPanel.h's
// on_room_settings_requested doc comment). Closing returns to RoomInfoPanel.
//
// Visual chrome (dimmed backdrop + a kPanelW-wide right-anchored strip,
// title + close button, hand-rolled wheel scrolling) mirrors RoomInfoPanel.
// Each pending request is a row with its own real Accept/Deny(/Deny & Ban)
// tk::Button children — modeled on DevicesSection::DeviceRow, the
// established pattern in this codebase for a variable-length list where
// every row carries its own interactive action buttons. The row list is
// rebuilt wholesale (clear_children + re-add) on every set_requests() call,
// since MSC2403 knock lists are small and change infrequently.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class KnockRequestsPanel : public tk::Widget
{
public:
    using ImageProvider = std::function<const tk::Image*(const std::string& mxc)>;

    KnockRequestsPanel();
    ~KnockRequestsPanel() override;

    static constexpr float kPanelW = 280.0f;

    void open(const std::string& room_id);
    void close();
    bool is_open() const { return open_; }
    const std::string& room_id() const { return room_id_; }

    // Replace the pending-request list. No-op on row identity (rebuilds
    // wholesale) — fine given the list is always small.
    void set_requests(std::vector<tesseract::KnockRequestInfo> requests);

    // Whether the current user can ban (gates the "Deny & Ban" button on
    // every row). Accept/Deny are always offered — the panel itself is
    // only ever opened when the caller already confirmed at least one of
    // invite/kick permission (see RoomInfoPanel's gating).
    void set_can_ban(bool can_ban);

    void set_avatar_provider(ImageProvider p);

    // Fired per-row. user_id identifies which request the action targets.
    std::function<void(std::string user_id)> on_accept;
    std::function<void(std::string user_id)> on_decline;
    std::function<void(std::string user_id, std::string reason)> on_decline_and_ban;

    // Fired when the close button is clicked — the shell reopens RoomInfoPanel.
    std::function<void()> on_close;

    // Fired when layout-affecting state changes (row list, can_ban) so the
    // host can request a relayout — mirrors RoomInfoPanel::on_layout_changed.
    std::function<void()> on_layout_changed;

    void on_theme_changed(const tk::Theme& t) override;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

private:
    class RowWidget;

    bool open_ = false;
    std::string room_id_;
    std::vector<tesseract::KnockRequestInfo> requests_;
    bool can_ban_ = false;
    ImageProvider image_provider_;

    tk::Button* close_btn_ = nullptr;
    std::vector<RowWidget*> rows_; // borrowed — owned by widget tree via add_child

    tk::Rect backdrop_rect_{};
    tk::Rect panel_rect_{};
    float scroll_offset_ = 0.0f;
    float content_height_ = 0.0f;

    std::unique_ptr<tk::TextLayout> title_layout_;
    std::unique_ptr<tk::TextLayout> empty_layout_; // "No pending requests" placeholder

    bool press_backdrop_ = false;

    void rebuild_rows_();

    static constexpr float kPadX      = 16.0f;
    static constexpr float kPadY      = 12.0f;
    static constexpr float kHeaderH   = 48.0f;
    static constexpr float kCloseSz   = 24.0f;
    static constexpr float kRowH      = 96.0f;
};

} // namespace tesseract::views
