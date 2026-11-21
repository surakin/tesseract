#pragma once

// Minimal container widget for secondary (pop-out) room windows.
// Stacks RoomView at full bounds with MediaViewerOverlay on top, mirroring MainAppWidget's overlay pattern
// without the sidebar/room-list/login scaffolding.

#include "CameraOverlayHost.h"
#include "ConfirmDialog.h"
#include "CreatePollDialog.h"
#include "ForwardRoomPicker.h"
#include "MediaViewerOverlay.h"
#include "RoomMediaView.h"
#include "RoomView.h"

#include "tk/layout.h"

namespace tesseract::views
{

// tk::Stack already provides exactly what this container needs: measure()
// returns the max size across visible children, and the inherited
// Widget::arrange()/paint() defaults already stack every child at the same
// bounds and paint visible ones in insertion order (with hit-testing/
// pointer dispatch already reverse-order, topmost-child-wins) — no need to
// hand-roll any of that here.
class PopoutRoomWidget : public tk::Stack
{
protected:
    // host() is nullable — see RoomView/ForwardRoomPicker's own constructors.
    PopoutRoomWidget();
    TK_WIDGET_FACTORY_FRIEND(PopoutRoomWidget)

public:
    ~PopoutRoomWidget() override = default;

    RoomView* room_view() const
    {
        return room_view_;
    }
    MediaViewerOverlay* media_viewer() const
    {
        return media_viewer_;
    }
    ForwardRoomPicker* forward_picker() const
    {
        return forward_picker_;
    }
    ConfirmDialog* confirm_dialog() const
    {
        return confirm_dialog_;
    }
    // The pop-out's own create-poll form (the /poll command opens it here, not
    // in the main window).
    CreatePollDialog* create_poll_dialog() const
    {
        return create_poll_dialog_;
    }
    // RoomView-owned (not a PopoutRoomWidget-level overlay) — see
    // RoomView.h's own room_media_view_ doc comment / MainAppWidget's
    // identical delegating accessor. Every existing caller
    // (RoomWindowBase, per-shell RoomWindow.cpp) keeps working unchanged.
    RoomMediaView* room_media_view() const
    {
        return room_view_ ? room_view_->room_media_view() : nullptr;
    }

    // Fires whenever the confirm / poll dialog opens/closes, so the host relayouts;
    // arrange() then hides the compose text area while the dialog covers it.
    std::function<void()> on_layout_changed;

    void show_media_viewer(bool show);

    // Opens the selfie camera overlay over this window (the /selfie command).
    // `on_captured` receives the raw BGRA8888 frame after the countdown;
    // `is_call_active` keeps the camera free while a call is using it. A no-op
    // while one is already open.
    void open_camera_overlay(std::function<bool()> is_call_active,
                             CameraOverlayHost::CapturedFn on_captured);

    // Escape closes the camera overlay or the poll form.
    bool on_key_down(const tk::KeyEvent& event) override;

    // Mirrors MainAppWidget::compose_text_area_rect(): empty while
    // any_modal_open_() or room_view_ isn't visible_in_tree(), so arrange()
    // knows when to force-hide the compose bar's native text_area().
    tk::Rect compose_text_area_rect() const;

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;

private:
    // True when any overlay stacked above RoomView (media viewer,
    // forward picker, confirm dialog, room media gallery) is open — mirrors
    // MainAppWidget::any_modal_open_().
    bool any_modal_open_() const;

    RoomView* room_view_ = nullptr; // owned via add_child
    MediaViewerOverlay* media_viewer_ = nullptr;
    ForwardRoomPicker* forward_picker_ = nullptr;
    ConfirmDialog* confirm_dialog_ = nullptr;
    CreatePollDialog* create_poll_dialog_ = nullptr;
    std::unique_ptr<CameraOverlayHost> camera_overlay_;

    bool modal_was_open_ = false;
};

} // namespace tesseract::views
