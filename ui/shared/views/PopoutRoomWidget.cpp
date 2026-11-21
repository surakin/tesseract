#include "PopoutRoomWidget.h"

namespace tesseract::views
{

PopoutRoomWidget::PopoutRoomWidget()
{
    auto rv = tk::create_widget<RoomView>(this);
    room_view_ = add_child(std::move(rv));

    auto viewer = std::make_unique<MediaViewerOverlay>();
    media_viewer_ = add_child(std::move(viewer));
    media_viewer_->set_visible(false);

    auto fp = tk::create_widget<ForwardRoomPicker>(this);
    forward_picker_ = add_child(std::move(fp));
    forward_picker_->set_visible(false);

    auto confirm = std::make_unique<ConfirmDialog>();
    confirm_dialog_ = add_child(std::move(confirm));
    room_view_->set_confirm_provider(
        [this](ConfirmDialog::Options opts, std::function<void()> on_confirm)
        {
            confirm_dialog_->open(std::move(opts), std::move(on_confirm));
        });
    confirm_dialog_->on_layout_changed = [this]
    {
        if (on_layout_changed)
        {
            on_layout_changed();
        }
    };

    create_poll_dialog_ = add_child(std::make_unique<CreatePollDialog>());
    create_poll_dialog_->on_layout_changed = [this]
    {
        if (on_layout_changed)
        {
            on_layout_changed();
        }
    };
}

void PopoutRoomWidget::open_camera_overlay(std::function<bool()> is_call_active,
                                           CameraOverlayHost::CapturedFn on_captured)
{
    if (!camera_overlay_)
    {
        camera_overlay_ = std::make_unique<CameraOverlayHost>(this,
                                                              [this]
                                                              {
                                                                  if (on_layout_changed)
                                                                  {
                                                                      on_layout_changed();
                                                                  }
                                                              });
    }
    camera_overlay_->is_call_active = std::move(is_call_active);
    camera_overlay_->on_captured = std::move(on_captured);
    camera_overlay_->open();
}

bool PopoutRoomWidget::on_key_down(const tk::KeyEvent& event)
{
    if (event.key != tk::Key::Escape)
    {
        return false;
    }
    if (camera_overlay_ && camera_overlay_->is_open())
    {
        camera_overlay_->close();
        return true;
    }
    if (create_poll_dialog_ && create_poll_dialog_->is_open())
    {
        create_poll_dialog_->close();
        return true;
    }
    return false;
}

void PopoutRoomWidget::show_media_viewer(bool show)
{
    if (media_viewer_)
    {
        media_viewer_->set_visible(show);
    }
}

bool PopoutRoomWidget::any_modal_open_() const
{
    return (room_view_ && room_view_->is_overlay_open()) ||
           (media_viewer_ && media_viewer_->is_open()) ||
           (forward_picker_ && forward_picker_->is_open()) ||
           (confirm_dialog_ && confirm_dialog_->is_open()) ||
           (create_poll_dialog_ && create_poll_dialog_->is_open()) ||
           (camera_overlay_ && camera_overlay_->is_open());
}

tk::Rect PopoutRoomWidget::compose_text_area_rect() const
{
    if (any_modal_open_())
    {
        return {};
    }
    if (!room_view_ || !room_view_->visible_in_tree())
    {
        return {};
    }
    return room_view_->compose_text_area_rect();
}

void PopoutRoomWidget::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    tk::Stack::arrange(ctx, bounds); // stacks every child at full bounds, as today

    // Mirrors MainAppWidget::arrange()'s identical tail — a full-bleed modal
    // (image/video viewer, forward picker, confirm dialog, ...) paints over
    // RoomView on the canvas, but the compose bar's self-owned native
    // text_area() would otherwise keep floating above it. Only a force-HIDE
    // is needed: the "show" case is already handled by ComposeBar's own
    // arrange() (reached via tk::Stack::arrange() above).
    if (room_view_ && room_view_->compose_bar())
    {
        if (auto* ta = room_view_->compose_bar()->text_area())
        {
            if (compose_text_area_rect().empty())
            {
                ta->set_visible(false);
            }
        }
    }
}

void PopoutRoomWidget::paint_before_children(tk::PaintCtx&)
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
}

} // namespace tesseract::views
