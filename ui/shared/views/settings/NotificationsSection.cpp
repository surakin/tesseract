#include "NotificationsSection.h"

#include "tesseract/settings.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kPadX = 24.0f; // horizontal inset for the checkbox rows
} // namespace

// ---------------------------------------------------------------------------

NotificationsSection::NotificationsSection()
{
    const auto& s = tesseract::Settings::instance();

    notif_cb_ = add_child(std::make_unique<tk::CheckButton>(
        "Enable notifications on this device", s.notifications_enabled));
    notif_cb_->on_change = [this](bool v)
    {
        if (on_notifications_changed)
            on_notifications_changed(v);
    };

    previews_cb_ = add_child(std::make_unique<tk::CheckButton>(
        "Show image & sticker previews in notifications",
        s.notification_image_previews));
    previews_cb_->on_change = [this](bool v)
    {
        if (on_image_previews_changed)
            on_image_previews_changed(v);
    };
}

void NotificationsSection::set_checked(bool enabled)
{
    notif_cb_->set_checked(enabled);
}

void NotificationsSection::set_image_previews_checked(bool enabled)
{
    previews_cb_->set_checked(enabled);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size NotificationsSection::measure(tk::LayoutCtx& ctx,
                                       tk::Size constraints)
{
    const float w       = constraints.w > 0 ? constraints.w : 0;
    const float inner_w = std::max(0.0f, w - kPadX * 2);
    const tk::Size cc{inner_w, 0};

    const float h1 = notif_cb_->measure(ctx, cc).h;
    const float h2 = previews_cb_->measure(ctx, cc).h;
    return {w, h1 + h2};
}

void NotificationsSection::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    const float inner_w = std::max(0.0f, bounds.w - kPadX * 2);
    const tk::Size cc{inner_w, 0};

    const float h1 = notif_cb_->measure(ctx, cc).h;
    const float h2 = previews_cb_->measure(ctx, cc).h;

    notif_cb_->arrange(ctx,
                       {bounds.x + kPadX, bounds.y, inner_w, h1});
    previews_cb_->arrange(ctx,
                          {bounds.x + kPadX, bounds.y + h1, inner_w, h2});
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void NotificationsSection::paint(tk::PaintCtx& ctx)
{
    notif_cb_->paint(ctx);
    previews_cb_->paint(ctx);
}

} // namespace tesseract::views
