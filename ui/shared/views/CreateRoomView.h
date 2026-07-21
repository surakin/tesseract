#pragma once

// CreateRoomView — content for creating a new room (name, topic, alias,
// visibility, encryption). Mounted as the Create tab of AddRoomView (see
// AddRoomView.h), which owns the modal backdrop, centred card, and
// Join/Create segmented header; this widget only renders its own content
// within the bounds it's given.
//
// Workflow:
//   1. User fills in the fields and clicks "Create" → `on_create_requested`
//      fires with a populated RoomCreateOptions.
//   2. Host calls Client::create_room() on a worker (via
//      ShellBase::create_room_command_), and calls set_state(Creating) while
//      it's in flight.
//   3. On success the host navigates to the new room and dismisses the
//      overlay; on failure it calls set_error().
//
// Unlike JoinRoomView, this widget has no open()/close()/is_open() of its
// own — lifecycle (including field reset) is owned by the wrapping
// AddRoomView; call reset() when the Create tab becomes active.

#include "tk/canvas.h"
#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/text_area.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>

namespace tesseract::views
{

class CreateRoomView : public tk::Widget
{
protected:
    CreateRoomView();
    TK_WIDGET_FACTORY_FRIEND(CreateRoomView)

public:
    ~CreateRoomView() override = default;

    // Preferred content dimensions — used by AddRoomView to size its card.
    static constexpr float kPreferredW = 440.0f;
    static constexpr float kPreferredH = 380.0f;

    enum class State
    {
        Idle,     // editing fields
        Creating, // create_room in flight
        Error,    // creation failed
    };

    void set_state(State s);
    State state() const
    {
        return state_;
    }

    // Shows an error message and transitions to State::Error.
    void set_error(std::string msg);

    // Clears all fields and returns to State::Idle. Called by AddRoomView
    // when the Create tab becomes active/open.
    void reset();

    // Hiding doesn't cascade to the native fields — tk::Widget::set_visible
    // is deliberately non-virtual/non-cascading — so this shadow hides them
    // explicitly, mirroring JoinRoomView/ForwardRoomPicker's idiom.
    void set_visible(bool v);

    // Suppresses this view's own title row — AddRoomView draws a shared
    // Join/Create segmented header in its place.
    void set_title_visible(bool v)
    {
        title_visible_ = v;
    }

    // Moves native OS focus into the name field — called by AddRoomView
    // when the Create tab becomes active/open.
    void focus_name_field();

    void on_theme_changed(const tk::Theme& t) override;

    // Fired when the user clicks "Create" with a populated options struct
    // built from the current field values.
    std::function<void(tesseract::RoomCreateOptions options)> on_create_requested;
    // Fired when the user clicks "Cancel".
    std::function<void()> on_cancel;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    void apply_state();
    tesseract::RoomCreateOptions build_options_() const;

    bool title_visible_ = true;
    State state_ = State::Idle;
    std::string error_msg_;

    // Child widgets (borrowed — owned by widget tree via add_child).
    tk::TextField* name_field_ = nullptr;
    tk::TextArea* topic_field_ = nullptr;
    tk::TextField* alias_field_ = nullptr;
    tk::ComboBox* visibility_combo_ = nullptr;
    tk::CheckButton* encryption_check_ = nullptr;
    tk::Button* create_btn_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;
    tk::Label* status_lbl_ = nullptr;
};

} // namespace tesseract::views
