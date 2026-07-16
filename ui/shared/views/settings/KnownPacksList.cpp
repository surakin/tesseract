#include "KnownPacksList.h"

#include "tk/controls.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

KnownPacksList::KnownPacksList() = default;

void KnownPacksList::set_packs(std::vector<KnownPackRow> packs)
{
    packs_ = std::move(packs);
    rebuild_();
}

void KnownPacksList::rebuild_()
{
    clear_children();
    rows_.clear();
    empty_label_ = nullptr;

    if (packs_.empty())
    {
        auto lbl = tk::create_widget<tk::Label>(this, tk::tr("No image packs found yet."));
        empty_label_ = add_child(std::move(lbl));
        return;
    }

    for (const auto& p : packs_)
    {
        const std::string label =
            p.display_name.empty() ? p.room_id : p.display_name;
        auto cb = tk::create_widget<tk::CheckButton>(this, label, p.subscribed);
        const std::string room_id   = p.room_id;
        const std::string state_key = p.state_key;
        cb->on_change = [this, room_id, state_key](bool checked)
        {
            if (on_subscription_toggled)
                on_subscription_toggled(room_id, state_key, checked);
        };
        rows_.push_back(add_child(std::move(cb)));
    }
}

tk::Size KnownPacksList::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, kViewportH};
}

void KnownPacksList::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    tk::Size natural = VBox::measure(ctx, {bounds.w, 1.0e6f});
    content_height_ = natural.h;

    const float max_scroll = std::max(0.0f, content_height_ - bounds.h);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);

    const float laid_h = std::max(bounds.h, content_height_);
    VBox::arrange(ctx, {bounds.x, bounds.y - scroll_y_, bounds.w, laid_h});

    // VBox::arrange wrote `bounds_` to the laid-out (content-height) rect —
    // restore the viewport rect so paint clipping / hit-testing operate on
    // the visible area, mirroring DevicesSection::arrange.
    bounds_ = bounds;
}

void KnownPacksList::paint(tk::PaintCtx& ctx)
{
    // Mirrors DevicesSection::paint: a wheel event only triggers a repaint,
    // not a full relayout, so child bounds from the last arrange() may be
    // stale w.r.t. the current scroll_y_. Re-arrange here off the
    // PaintCtx's own CanvasFactory + Theme before painting/hit-testing.
    tk::LayoutCtx lc{ctx.factory, ctx.theme};
    const tk::Rect viewport = bounds_;
    const float laid_h = std::max(viewport.h, content_height_);
    VBox::arrange(lc, {viewport.x, viewport.y - scroll_y_, viewport.w, laid_h});
    bounds_ = viewport;

    ctx.canvas.push_clip_rect(bounds_);
    VBox::paint(ctx);

    if (content_height_ > bounds_.h)
    {
        const auto& pal = ctx.theme.palette;
        const float track_w = 4.0f;
        const float track_x = bounds_.x + bounds_.w - track_w - 2.0f;
        const float track_h = bounds_.h;
        const float track_y = bounds_.y;
        const float thumb_h =
            std::max(20.0f, track_h * (track_h / content_height_));
        const float max_scroll = content_height_ - bounds_.h;
        const float frac = max_scroll > 0 ? (scroll_y_ / max_scroll) : 0.0f;
        const float thumb_y = track_y + frac * (track_h - thumb_h);
        ctx.canvas.fill_rounded_rect({track_x, thumb_y, track_w, thumb_h},
                                     track_w * 0.5f,
                                     pal.text_muted.with_alpha(120));
    }

    ctx.canvas.pop_clip();
}

bool KnownPacksList::on_wheel(tk::Point /*local*/, float /*dx*/, float dy)
{
    if (content_height_ <= bounds_.h)
        return false;
    const float max_scroll = content_height_ - bounds_.h;
    const float prev = scroll_y_;
    scroll_y_ = std::clamp(scroll_y_ + dy, 0.0f, max_scroll);
    return scroll_y_ != prev;
}

void KnownPacksList::scroll_into_view(tk::Rect world_rect)
{
    // Mirrors SettingsPage::scroll_into_view — bounds_ is this list's own
    // fixed-height viewport rect, already world-coordinate like world_rect.
    if (world_rect.y < bounds_.y)
        scroll_y_ -= (bounds_.y - world_rect.y);
    else if (world_rect.y + world_rect.h > bounds_.y + bounds_.h)
        scroll_y_ += (world_rect.y + world_rect.h) - (bounds_.y + bounds_.h);
    const float max_scroll = std::max(0.0f, content_height_ - bounds_.h);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);
}

} // namespace tesseract::views
