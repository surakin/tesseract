#include "views/CameraOverlayHost.h"

namespace tesseract::views
{

CameraOverlayHost::CameraOverlayHost(tk::Widget* parent, std::function<void()> layout_changed)
    : parent_(parent), layout_changed_(std::move(layout_changed))
{
}

void CameraOverlayHost::open()
{
    if (widget_ || !parent_)
    {
        return;
    }
    if (is_call_active && is_call_active())
    {
        return;
    }
    // No device probe here: CameraWidget itself explains a missing, busy or
    // blocked camera rather than the command silently doing nothing.
    auto widget = std::make_unique<CameraWidget>();
    widget_ = widget.get();
    widget_->on_frame_captured =
        [this](std::vector<std::uint8_t> bgra, std::uint32_t w, std::uint32_t h)
    {
        if (on_captured)
        {
            on_captured(std::move(bgra), w, h);
        }
    };
    widget_->on_dismissed = [this]
    {
        close();
    };
    parent_->add_child(std::move(widget));
    widget_->open();
    // Trigger arrange + repaint so the new child gets bounds and appears.
    if (layout_changed_)
    {
        layout_changed_();
    }
}

void CameraOverlayHost::close()
{
    if (!widget_)
    {
        return;
    }
    CameraWidget* w = widget_;
    widget_ = nullptr;
    parent_->remove_child(w);
    if (layout_changed_)
    {
        layout_changed_();
    }
}

} // namespace tesseract::views
