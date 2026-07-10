#include "PopoutRoomWidget.h"

namespace tesseract::views
{

PopoutRoomWidget::PopoutRoomWidget()
{
    auto rv = std::make_unique<RoomView>();
    room_view_ = add_child(std::move(rv));

    // Gallery sits behind the lightboxes so opening an item from it can
    // still show its lightbox on top (matches MainAppWidget's ordering).
    auto rmv = std::make_unique<RoomMediaView>();
    room_media_view_ = add_child(std::move(rmv));
    room_media_view_->set_visible(false);

    auto img = std::make_unique<ImageViewerOverlay>();
    img_viewer_ = add_child(std::move(img));
    img_viewer_->set_visible(false);

    auto vid = std::make_unique<VideoViewerOverlay>();
    vid_viewer_ = add_child(std::move(vid));
    vid_viewer_->set_visible(false);

    auto fp = std::make_unique<ForwardRoomPicker>();
    forward_picker_ = add_child(std::move(fp));
    forward_picker_->set_visible(false);

    auto confirm = std::make_unique<ConfirmDialog>();
    confirm_dialog_ = add_child(std::move(confirm));
    room_view_->set_confirm_provider(
        [this](ConfirmDialog::Options opts, std::function<void()> on_confirm)
        { confirm_dialog_->open(std::move(opts), std::move(on_confirm)); });
    confirm_dialog_->on_layout_changed = [this]
    {
        if (on_layout_changed)
        {
            on_layout_changed();
        }
    };
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

} // namespace tesseract::views
