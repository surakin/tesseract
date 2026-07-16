#include "PopoutRoomWidget.h"


namespace tesseract::views
{

PopoutRoomWidget::PopoutRoomWidget()
{
    auto rv = tk::create_widget<RoomView>(this);
    room_view_ = add_child(std::move(rv));

    auto img = std::make_unique<ImageViewerOverlay>();
    img_viewer_ = add_child(std::move(img));
    img_viewer_->set_visible(false);

    auto vid = std::make_unique<VideoViewerOverlay>();
    vid_viewer_ = add_child(std::move(vid));
    vid_viewer_->set_visible(false);

    auto fp = tk::create_widget<ForwardRoomPicker>(this);
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

bool PopoutRoomWidget::any_modal_open_() const
{
    return (room_view_       && room_view_->is_overlay_open()) ||
           (img_viewer_      && img_viewer_->is_open()) ||
           (vid_viewer_      && vid_viewer_->is_open()) ||
           (forward_picker_  && forward_picker_->is_open()) ||
           (confirm_dialog_  && confirm_dialog_->is_open());
}

void PopoutRoomWidget::paint(tk::PaintCtx& ctx)
{
    const bool modal_open = any_modal_open_();
    if (modal_open && !modal_was_open_ && host())
    {
        // A modal overlay just opened in this pop-out room window — drop
        // tk-level keyboard focus so its stale focus ring doesn't keep
        // showing through/around the overlay. Mirrors MainAppWidget's
        // identical fix for the main window.
        host()->clear_focus();
    }
    modal_was_open_ = modal_open;
    Stack::paint(ctx);
}

} // namespace tesseract::views
