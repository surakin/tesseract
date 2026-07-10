#pragma once

// Minimal container widget for secondary (pop-out) room windows.
// Stacks RoomView at full bounds with ImageViewerOverlay and
// VideoViewerOverlay on top, mirroring MainAppWidget's overlay pattern
// without the sidebar/room-list/login scaffolding.

#include "ConfirmDialog.h"
#include "ForwardRoomPicker.h"
#include "ImageViewerOverlay.h"
#include "RoomMediaView.h"
#include "RoomView.h"
#include "VideoViewerOverlay.h"

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
public:
    PopoutRoomWidget();
    ~PopoutRoomWidget() override = default;

    RoomView* room_view() const
    {
        return room_view_;
    }
    ImageViewerOverlay* image_viewer() const
    {
        return img_viewer_;
    }
    VideoViewerOverlay* video_viewer() const
    {
        return vid_viewer_;
    }
    ForwardRoomPicker* forward_picker() const
    {
        return forward_picker_;
    }
    ConfirmDialog* confirm_dialog() const
    {
        return confirm_dialog_;
    }
    RoomMediaView* room_media_view() const
    {
        return room_media_view_;
    }

    // Fires whenever the confirm dialog opens/closes, so the platform shell
    // can re-query native-overlay visibility (hide the compose text area /
    // search fields while the dialog covers them — native child controls
    // always paint over canvas-drawn overlays otherwise).
    std::function<void()> on_layout_changed;

    void show_image_viewer(bool show);
    void show_video_viewer(bool show);

private:
    RoomView*           room_view_       = nullptr; // owned via add_child
    ImageViewerOverlay* img_viewer_      = nullptr;
    VideoViewerOverlay* vid_viewer_      = nullptr;
    ForwardRoomPicker*  forward_picker_  = nullptr;
    ConfirmDialog*      confirm_dialog_  = nullptr;
    RoomMediaView*      room_media_view_ = nullptr;
};

} // namespace tesseract::views
