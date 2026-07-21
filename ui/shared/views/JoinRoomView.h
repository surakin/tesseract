#pragma once

// JoinRoomView — content for looking up and joining a room by alias or
// room ID (MSC3266). Mounted as the Join tab of AddRoomView (see
// AddRoomView.h), which owns the modal backdrop, centred card, and
// Join/Create segmented header; this widget only renders its own content
// within the bounds it's given.
//
// Workflow:
//   1. User types an alias / room ID in the host-overlaid NativeTextField.
//   2. Clicks "Look up" → `on_lookup_requested` fires.
//   3. Host calls Client::get_room_summary() on a worker, then either
//      set_preview() (success) or set_error() (failure) on the UI thread.
//   4. User clicks "Join" → `on_join_requested` fires.
//   5. Host calls Client::join_room() on a worker, selects the new room
//      and dismisses the popup on success.
//
// The alias field is a tk::TextField — a real widget-tree child that owns
// and positions its own native edit control and pipes text changes back
// via `set_alias_text()`.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class JoinRoomView : public tk::Widget
{
protected:
    JoinRoomView();
    TK_WIDGET_FACTORY_FRIEND(JoinRoomView)

public:
    ~JoinRoomView() override = default;

    // Preferred content dimensions — used by AddRoomView to size its card.
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

    // ── Lifecycle ─────────────────────────────────────────────────────────
    // Resets to State::Idle, prefills the alias field, and requests native
    // focus on the next paint(). Called by AddRoomView when the Join tab
    // becomes active/open.
    void open(const std::string& prefill = "");
    // Resets state and fires on_close. Called by AddRoomView on Cancel/
    // Escape/outside-click/tab-switch-away.
    void close();
    bool is_open() const
    {
        return is_open_;
    }

    // Hiding (close()) doesn't cascade to the native alias field —
    // tk::Widget::set_visible is deliberately non-virtual/non-cascading —
    // so this shadow hides it explicitly, mirroring ForwardRoomPicker's idiom.
    void set_visible(bool v);

    // Suppresses this view's own title row — AddRoomView draws a shared
    // Join/Create segmented header in its place.
    void set_title_visible(bool v)
    {
        title_visible_ = v;
    }

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

    // Whether the alias field should be interactable (false in Joining state).
    bool alias_field_visible() const;

    // Moves native OS focus into the alias field — called by the shell when
    // the dialog is shown.
    void focus_alias_field();

    void on_theme_changed(const tk::Theme& t) override;

    // Programmatically sets the alias field's text (prefill / clear) —
    // pushes into both the internal model and the native control.
    void set_alias_text(std::string text);
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
    // Fired from close() (Cancel, Escape, outside click, or programmatic).
    std::function<void()> on_close;
    // Fired when the user clicks an http(s) or matrix: link in the room topic.
    std::function<void(std::string url)> on_link_clicked;
    // Fired when the pointer enters or leaves a link in the topic.
    std::function<void(std::string url)> on_link_hovered;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    void apply_state();
    // Fires on_lookup_requested if alias_text_ is non-empty — shared by the
    // "Look up" button and the alias field's Enter/submit handler.
    void request_lookup_();

    bool is_open_ = false;
    bool title_visible_ = true;
    // Set by open(); consumed by the next paint(). Deferred rather than
    // focused synchronously inside open() because arrange() — which
    // positions alias_field_'s native overlay via set_rect() — hasn't run
    // yet at that point. Mirrors ForwardRoomPicker::pending_focus_'s
    // identical rationale.
    bool pending_focus_ = false;

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
    tk::TextField* alias_field_ = nullptr;

    // Layout rects (world-space, valid after arrange()).
    tk::Rect preview_card_rect_{};
    tk::Rect topic_rect_{};  // world-space rect of the topic text block

    // Measured height of the avatar-row info column (name + join-rule pill +
    // member count), computed in arrange() and consumed by paint() to
    // position the topic below it — real content routinely exceeds the
    // avatar's own height, so this can't be a fixed constant.
    float preview_info_h_ = 0.0f;

    // Cached topic layout and its source spans (rebuilt on set_preview).
    std::unique_ptr<tk::TextLayout>    topic_layout_;
    std::vector<tk::TextSpan>          topic_spans_;

    std::string press_link_url_;
    std::string hover_link_url_;
};

} // namespace tesseract::views
