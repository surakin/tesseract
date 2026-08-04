#include "KnownPacksList.h"

#include "tk/controls.h"
#include "tk/i18n.h"

#include <algorithm>

namespace tesseract::views
{

KnownPacksList::KnownPacksList()
{
    content_ = add_child(tk::create_widget<tk::VBox>(this));
}

void KnownPacksList::set_packs(std::vector<KnownPackRow> packs)
{
    packs_ = std::move(packs);
    rebuild_();
}

void KnownPacksList::rebuild_()
{
    content_->clear_children();
    rows_.clear();
    empty_label_ = nullptr;

    if (packs_.empty())
    {
        auto lbl = tk::create_widget<tk::Label>(content_, tk::tr("No image packs found yet."));
        empty_label_ = content_->add_child(std::move(lbl));
        return;
    }

    for (const auto& p : packs_)
    {
        const std::string label =
            p.display_name.empty() ? p.room_id : p.display_name;
        auto cb = tk::create_widget<tk::CheckButton>(content_, label, p.subscribed);
        const std::string room_id   = p.room_id;
        const std::string state_key = p.state_key;
        cb->on_change = [this, room_id, state_key](bool checked)
        {
            if (on_subscription_toggled)
                on_subscription_toggled(room_id, state_key, checked);
        };
        rows_.push_back(content_->add_child(std::move(cb)));
    }
}

tk::Size KnownPacksList::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, kViewportH};
}

void KnownPacksList::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    tk::Size natural = content_->measure(ctx, {bounds.w, 1.0e6f});
    content_height_ = natural.h;

    clamp_scroll();

    const float laid_h = std::max(bounds.h, content_height_);
    content_->arrange(ctx, {bounds.x, bounds.y - scroll_y_, bounds.w, laid_h});
}

void KnownPacksList::paint_before_children(tk::PaintCtx& ctx)
{
    // Advance any in-flight trackpad-momentum fling before the re-arrange
    // below picks up scroll_y_.
    step_kinetic();

    // Mirrors SettingsPage::paint_before_children — a wheel event only
    // triggers a repaint, not a full relayout, so content_'s bounds from the
    // last arrange() may be stale w.r.t. the current scroll_y_.
    tk::LayoutCtx lc{ctx.factory, ctx.theme};
    const float laid_h = std::max(bounds_.h, content_height_);
    content_->arrange(lc, {bounds_.x, bounds_.y - scroll_y_, bounds_.w, laid_h});

    ctx.canvas.push_clip_rect(bounds_);
}

void KnownPacksList::paint_after_children(tk::PaintCtx& ctx)
{
    ctx.canvas.pop_clip();
    paint_scrollbar(ctx);
}

bool KnownPacksList::on_wheel(tk::Point /*local*/, float /*dx*/, float dy, bool is_touchpad)
{
    return on_wheel_scroll(dy, is_touchpad);
}

tk::Widget* KnownPacksList::dispatch_pointer_down(tk::Point world)
{
    if (!visible() || !contains_world(world))
        return nullptr;
    tk::Point local = world_to_local(world);
    if (thumb_hit(local) && scrollbar_on_pointer_down(local))
        return this;
    return tk::Widget::dispatch_pointer_down(world);
}

void KnownPacksList::on_pointer_drag(tk::Point local)
{
    scrollbar_on_pointer_drag(local);
}

void KnownPacksList::on_pointer_up(tk::Point /*local*/, bool /*inside_self*/)
{
    scrollbar_on_pointer_up();
}

} // namespace tesseract::views
