#include "PopoutRoomWidget.h"

namespace tesseract::views
{

PopoutRoomWidget::PopoutRoomWidget()
{
    auto rv = std::make_unique<RoomView>();
    room_view_ = add_child(std::move(rv));

    auto img = std::make_unique<ImageViewerOverlay>();
    img_viewer_ = add_child(std::move(img));
    img_viewer_->set_visible(false);

    auto vid = std::make_unique<VideoViewerOverlay>();
    vid_viewer_ = add_child(std::move(vid));
    vid_viewer_->set_visible(false);
}

void PopoutRoomWidget::show_image_viewer(bool show)
{
    if (img_viewer_)
    {
        img_viewer_->set_visible(show);
    }
}

void PopoutRoomWidget::show_video_viewer(bool show)
{
    if (vid_viewer_)
    {
        vid_viewer_->set_visible(show);
    }
}

tk::Size PopoutRoomWidget::measure(tk::LayoutCtx& ctx, tk::Size constraints)
{
    // Overlays are full-bounds; measure them to satisfy the layout contract
    // but the returned size is driven by room_view_ alone.
    if (img_viewer_)
        img_viewer_->measure(ctx, constraints);
    if (vid_viewer_)
        vid_viewer_->measure(ctx, constraints);
    return room_view_ ? room_view_->measure(ctx, constraints) : constraints;
}

void PopoutRoomWidget::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    // Record our own bounds: pointer dispatch tests contains_world() on this
    // root before descending to children, so without this bounds_ stays {0,0,
    // 0,0} and every click/move/wheel is dropped before reaching room_view_
    // (the children still paint, since paint() calls them directly).
    bounds_ = bounds;
    if (room_view_)
    {
        room_view_->arrange(ctx, bounds);
    }
    if (img_viewer_)
    {
        img_viewer_->arrange(ctx, bounds);
    }
    if (vid_viewer_)
    {
        vid_viewer_->arrange(ctx, bounds);
    }
}

void PopoutRoomWidget::paint(tk::PaintCtx& ctx)
{
    if (room_view_)
    {
        room_view_->paint(ctx);
    }
    if (img_viewer_ && img_viewer_->visible())
    {
        img_viewer_->paint(ctx);
    }
    if (vid_viewer_ && vid_viewer_->visible())
    {
        vid_viewer_->paint(ctx);
    }
}

} // namespace tesseract::views
