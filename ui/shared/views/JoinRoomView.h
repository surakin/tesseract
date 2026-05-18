#pragma once

// JoinRoomView — floating dialog for looking up and joining a room by
// alias or room ID (MSC3266). The shell creates a popup surface sized to
// kPreferredW × kPreferredH and mounts this widget as its root.
//
// Workflow:
//   1. User types an alias / room ID in the host-overlaid NativeTextField.
//   2. Clicks "Look up" → `on_lookup_requested` fires.
//   3. Shell calls Client::get_room_summary() on a worker, then either
//      set_preview() (success) or set_error() (failure) on the UI thread.
//   4. User clicks "Join" → `on_join_requested` fires.
//   5. Shell calls Client::join_room() on a worker, selects the new room
//      and dismisses the popup on success.
//
// NativeTextField: host overlays a native edit control over `alias_field_rect()`
// and pipes text changes back via `set_alias_text()`. Same pattern as
// LoginView::homeserver_field_rect() and RecoveryBanner::recovery_key_field_rect().

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>

namespace tesseract::views
{

class JoinRoomView : public tk::Widget
{
public:
    JoinRoomView();
    ~JoinRoomView() override = default;

    // Preferred popup dimensions the shell should use.
    static constexpr float kPreferredW = 440.0f;
    static constexpr float kPreferredH = 420.0f;

    enum class State
    {
        Idle,    // waiting for user to type an alias
        Loading, // get_room_summary in flight
        Preview, // summary arrived — preview card visible
        Joining, // join_room in flight
        Error,   // lookup or join failed
    };

    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;

    void set_state(State s);
    State state() const
    {
        return state_;
    }

    // Populate the preview card and transition to State::Preview.
    void set_preview(const tesseract::RoomSummary& summary);

    // Show an error message and transition to State::Error.
    void set_error(std::string msg);

    void set_avatar_provider(AvatarProvider p);

    // Room ID from the last successful set_preview(); empty otherwise.
    const std::string& preview_room_id() const
    {
        return preview_.room_id;
    }

    // NativeTextField overlay rect (widget-local). Empty rect when the
    // field should not be interactable (Joining state).
    tk::Rect alias_field_rect() const;
    bool alias_field_visible() const;

    // Host pipes the alias input text back in.
    void set_alias_text(std::string text)
    {
        alias_text_ = std::move(text);
    }
    const std::string& alias_text() const
    {
        return alias_text_;
    }

    // Fired when the user clicks "Look up".
    std::function<void(const std::string& alias)> on_lookup_requested;
    // Fired when the user clicks "Join".
    std::function<void(const std::string& room_id_or_alias)> on_join_requested;
    // Fired when the user clicks "Cancel" / "✕".
    std::function<void()> on_cancel;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    void apply_state();

    State state_ = State::Idle;
    std::string alias_text_;
    std::string error_msg_;
    tesseract::RoomSummary preview_;
    AvatarProvider avatar_provider_;

    // Child widgets (borrowed — owned by widget tree via add_child).
    tk::Button* lookup_btn_ = nullptr;
    tk::Button* join_btn_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;
    tk::Label* status_lbl_ = nullptr;

    // Layout rects (world-space, valid after arrange()).
    tk::Rect alias_field_rect_{};
    tk::Rect preview_card_rect_{};
};

} // namespace tesseract::views
