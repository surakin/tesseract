#include "scroll_view.h"

namespace tk
{

Widget* ScrollView::set_child(std::unique_ptr<Widget> child)
{
    if (child_)
    {
        remove_child(child_);
        child_ = nullptr;
    }
    child_ = add_child(std::move(child));
    return child_;
}

Size ScrollView::measure(LayoutCtx&, Size constraints)
{
    return constraints;
}

void ScrollView::arrange(LayoutCtx& ctx, Rect bounds)
{
    bounds_ = bounds;
    if (!child_)
    {
        content_h_ = 0.0f;
        return;
    }
    // Unbounded height probe: the child reports its own natural (wrapped,
    // for text) height for this width, regardless of the viewport — the
    // viewport constrains what's visible via clipping in paint(), not the
    // child's own arranged size.
    const Size natural = child_->measure(ctx, {bounds.w, 1'000'000.0f});
    content_h_ = natural.h;
    clamp_scroll();
    child_->arrange(ctx, {bounds.x, bounds.y - scroll_y_, bounds.w, content_h_});
}

void ScrollView::paint(PaintCtx& ctx)
{
    if (!child_)
        return;
    ctx.canvas.push_clip_rect(bounds_);
    child_->paint(ctx);
    ctx.canvas.pop_clip();
    paint_scrollbar(ctx);
}

bool ScrollView::on_wheel(Point, float, float dy, bool is_touchpad)
{
    if (!child_)
        return false;
    const float prev = scroll_y_;
    const bool consumed = on_wheel_scroll(dy, is_touchpad);
    if (scroll_y_ != prev && on_layout_changed)
        on_layout_changed();
    return consumed;
}

bool ScrollView::on_pointer_down(Point local)
{
    return scrollbar_on_pointer_down(local);
}

void ScrollView::on_pointer_drag(Point local)
{
    const float prev = scroll_y_;
    scrollbar_on_pointer_drag(local);
    if (scroll_y_ != prev && on_layout_changed)
        on_layout_changed();
}

void ScrollView::on_pointer_up(Point, bool)
{
    scrollbar_on_pointer_up();
}

} // namespace tk
