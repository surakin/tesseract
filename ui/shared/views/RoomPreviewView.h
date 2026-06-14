#pragma once

// RoomPreviewView — full-panel widget shown in the chat area when the user
// clicks an unjoined room in a space's "Available to Join" section.
//
// Displays avatar, name, alias, topic, member count, join-rule label and a
// "Join" button. Invisible until set_summary() is called.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <optional>
#include <string>

namespace tesseract::views
{

class RoomPreviewView : public tk::Widget
{
public:
    enum class State { Idle, Joining };

    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc)>;

    RoomPreviewView();
    ~RoomPreviewView() override = default;

    void set_summary(const tesseract::RoomSummary& s);
    void clear();
    void set_state(State s);
    void set_avatar_provider(AvatarProvider p);

    // Fired when user clicks Join; room_id is RoomSummary::room_id.
    std::function<void(const std::string& room_id)> on_join;
    // Fired when the user dismisses the preview without joining.
    std::function<void()> on_dismiss;
    // Shell should kick an avatar fetch on miss and repaint.
    std::function<void(const std::string& mxc)> on_avatar_needed;

    // Test helpers.
    bool join_button_enabled() const;
    void trigger_join_for_test();

    // tk::Widget overrides.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;

private:
    std::optional<tesseract::RoomSummary> summary_;
    State          state_           = State::Idle;
    AvatarProvider avatar_provider_;

    tk::Button* join_btn_    = nullptr;
    tk::Button* dismiss_btn_ = nullptr;

    mutable std::unique_ptr<tk::TextLayout> name_layout_;
    mutable std::unique_ptr<tk::TextLayout> alias_layout_;
    mutable std::unique_ptr<tk::TextLayout> topic_layout_;
    mutable std::unique_ptr<tk::TextLayout> meta_layout_;
    mutable tk::CanvasFactory*              factory_seen_    = nullptr;
    mutable float                           last_bounds_h_   = -1.0f;

    static constexpr float kAvatarD  = 72.0f;
    static constexpr float kContentW = 300.0f;
    static constexpr float kPadY     = 32.0f;
    static constexpr float kGap      = 12.0f;
    static constexpr float kBtnH     = 36.0f;
    static constexpr float kBtnGap   = 8.0f;

    std::string join_rule_label_() const;
    void        reset_layouts_();
    void        fire_join_();
};

} // namespace tesseract::views
