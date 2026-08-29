#include "SettingsPage.h"

#include "SettingsGroup.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

// Outer page inset + spacing between adjacent groups/widgets. Matches the
// padding the per-section code used to redeclare in its own anonymous
// namespace before this base class existed.
constexpr float kPagePadX = 24.0f;
constexpr float kPagePadY = 16.0f;
constexpr float kGroupSpacing = 20.0f;

} // namespace

SettingsPage::SettingsPage()
{
    auto content = tk::create_widget<tk::VBox>(this);
    content->set_padding(tk::Edges{kPagePadY, kPagePadX, kPagePadY, kPagePadX});
    content->set_spacing(kGroupSpacing);
    content_ = add_child(std::move(content));
}

SettingsGroup* SettingsPage::add_group(std::string header)
{
    return content_->add_child(std::make_unique<SettingsGroup>(std::move(header)));
}

// ---------------------------------------------------------------------------
// Scroll support: lay content_ out at its natural height (which may be
// taller than the viewport), then clip painting to the page's bounds. Wheel
// events adjust scroll_y_ within [0, max] via ScrollableBase::on_wheel_scroll.
// ---------------------------------------------------------------------------

tk::Size SettingsPage::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill whatever the host gives us; SettingsPage is the inner widget of a
    // SideTabView, which always feeds us a fixed viewport.
    return constraints;
}

void SettingsPage::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // Probe the natural content height by asking content_'s own measure with
    // an unbounded vertical constraint.
    tk::Size natural = content_->measure(ctx, {bounds.w, 1.0e6f});
    content_height_ = natural.h;

    // Clamp scroll into range now that we know the content height.
    clamp_scroll();

    // Arrange content_ at the natural height, shifted up by scroll_y_.
    // Off-screen widgets just sit outside the page's bounds — paint clips
    // them away and pointer dispatch's contains_world filters them.
    const float laid_h = std::max(bounds.h, content_height_);
    content_->arrange(ctx, {bounds.x, bounds.y - scroll_y_, bounds.w, laid_h});
}

void SettingsPage::paint_before_children(tk::PaintCtx& ctx)
{
    // Advance any in-flight trackpad-momentum fling before the re-arrange
    // below picks up scroll_y_.
    step_kinetic();

    // The host's wheel handler triggers request_repaint() but not a full
    // relayout, so content_'s bounds from the last arrange() may be stale
    // w.r.t. the current scroll_y_. Re-arrange here off the PaintCtx's own
    // CanvasFactory + Theme so content lands at the current scroll offset
    // and pointer hit-testing stays consistent.
    tk::LayoutCtx lc{ctx.factory, ctx.theme};
    const float laid_h = std::max(bounds_.h, content_height_);
    content_->arrange(lc, {bounds_.x, bounds_.y - scroll_y_, bounds_.w, laid_h});

    ctx.canvas.push_clip_rect(bounds_);
}

void SettingsPage::paint_after_children(tk::PaintCtx& ctx)
{
    ctx.canvas.pop_clip();
    paint_scrollbar(ctx);
}

bool SettingsPage::on_wheel(tk::Point /*local*/, float /*dx*/, float dy, bool is_touchpad)
{
    return on_wheel_scroll(dy, is_touchpad);
}

tk::Widget* SettingsPage::dispatch_pointer_down(tk::Point world)
{
    if (!visible() || !contains_world(world))
        return nullptr;
    tk::Point local = world_to_local(world);
    if (thumb_hit(local) && scrollbar_on_pointer_down(local))
        return this;
    return tk::Widget::dispatch_pointer_down(world);
}

void SettingsPage::on_pointer_drag(tk::Point local)
{
    scrollbar_on_pointer_drag(local);
}

void SettingsPage::on_pointer_up(tk::Point /*local*/, bool /*inside_self*/)
{
    scrollbar_on_pointer_up();
}

} // namespace tesseract::views
