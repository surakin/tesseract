#pragma once

// Minimal container widget for secondary (pop-out) room windows.
// Stacks RoomView at full bounds with ImageViewerOverlay and
// VideoViewerOverlay on top, mirroring MainAppWidget's overlay pattern
// without the sidebar/room-list/login scaffolding.

#include "ImageViewerOverlay.h"
#include "RoomView.h"
#include "VideoViewerOverlay.h"

#include "tk/widget.h"

namespace tesseract::views
{

class PopoutRoomWidget : public tk::Widget
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

    void show_image_viewer(bool show);
    void show_video_viewer(bool show);

    // Widget overrides
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size constraints) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(tk::PaintCtx& ctx) override;

private:
    RoomView*           room_view_  = nullptr; // owned via add_child
    ImageViewerOverlay* img_viewer_ = nullptr;
    VideoViewerOverlay* vid_viewer_ = nullptr;
};

} // namespace tesseract::views
