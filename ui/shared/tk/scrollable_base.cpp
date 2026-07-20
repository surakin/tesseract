#include "scrollable_base.h"

#include "host.h"

#include <algorithm>
#include <cmath>

namespace tk
{

namespace
{

constexpr float kScrollbarWidth = 6.0f;
constexpr float kScrollbarRadius = 3.0f;
constexpr float kScrollbarMinLen = 24.0f;
constexpr float kScrollbarInset = 2.0f;

} // namespace

void ScrollableBase::clamp_scroll()
{
    float max_scroll = std::max(0.0f, content_height() - bounds_.h);
    if (scroll_y_ < 0)
    {
        scroll_y_ = 0;
    }
    if (scroll_y_ > max_scroll)
    {
        scroll_y_ = max_scroll;
    }
}

ScrollableBase::ThumbGeom ScrollableBase::thumb_geom() const
{
    ThumbGeom g{0, 0, 0, 0};
    float total = content_height();
    if (total <= bounds_.h || bounds_.h <= 0)
    {
        return g;
    }
    g.track_top = bounds_.y + kScrollbarInset;
    g.track_h = bounds_.h - kScrollbarInset * 2;
    g.thumb_h = std::max(kScrollbarMinLen, g.track_h * (bounds_.h / total));
    g.thumb_top =
        g.track_top + (g.track_h - g.thumb_h) *
                          (scroll_y_ / std::max(1.0f, total - bounds_.h));
    return g;
}

bool ScrollableBase::thumb_hit(Point local) const
{
    // local is widget-local — convert to world to compare with the
    // thumb_geom (which is in world coords because bounds_ is world).
    float world_x = local.x + bounds_.x;
    float world_y = local.y + bounds_.y;
    if (content_height() <= bounds_.h)
    {
        return false;
    }
    float right = bounds_.x + bounds_.w - kScrollbarInset;
    float left = right - kScrollbarWidth;
    if (world_x < left || world_x > right)
    {
        return false;
    }
    ThumbGeom g = thumb_geom();
    return world_y >= g.thumb_top && world_y < g.thumb_top + g.thumb_h;
}

void ScrollableBase::paint_scrollbar(PaintCtx& ctx) const
{
    float total = content_height();
    if (total > bounds_.h && bounds_.h > 0)
    {
        float track_h = bounds_.h - kScrollbarInset * 2;
        float thumb_h =
            std::max(kScrollbarMinLen, track_h * (bounds_.h / total));
        float thumb_y = bounds_.y + kScrollbarInset +
                        (track_h - thumb_h) *
                            (scroll_y_ / std::max(1.0f, total - bounds_.h));
        Rect thumb{bounds_.x + bounds_.w - kScrollbarWidth - kScrollbarInset,
                   thumb_y, kScrollbarWidth, thumb_h};
        Color c = ctx.theme.palette.text_muted.with_alpha(128);
        ctx.canvas.fill_rounded_rect(thumb, kScrollbarRadius, c);
    }
}

bool ScrollableBase::on_wheel_scroll(float dy, bool is_touchpad)
{
    kinetic_.on_wheel_delta(dy, is_touchpad);
    bool changed = apply_scroll_delta(dy);
    if (kinetic_.active())
    {
        if (auto* h = host())
        {
            h->request_repaint();
        }
    }
    return changed;
}

void ScrollableBase::step_kinetic()
{
    if (!kinetic_.active())
    {
        return;
    }
    float d = kinetic_.step();
    if (d != 0.0f && !apply_scroll_delta(d))
    {
        // Hit a clamp bound — stop dead, no rubber-band/overscroll.
        kinetic_.cancel();
    }
    if (kinetic_.active())
    {
        if (auto* h = host())
        {
            h->request_repaint();
        }
    }
}

bool ScrollableBase::scrollbar_on_pointer_down(Point local)
{
    if (!thumb_hit(local))
    {
        return false;
    }
    kinetic_.cancel();
    scrollbar_drag_ = true;
    ThumbGeom g = thumb_geom();
    drag_anchor_y_ = (local.y + bounds_.y) - g.thumb_top;
    return true;
}

bool ScrollableBase::scrollbar_on_pointer_drag(Point local)
{
    if (!scrollbar_drag_)
    {
        return false;
    }
    ThumbGeom g = thumb_geom();
    float total = content_height();
    float travel = g.track_h - g.thumb_h;
    if (travel <= 0 || total <= bounds_.h)
    {
        return false;
    }
    float wanted_thumb_top = (local.y + bounds_.y) - drag_anchor_y_;
    float t = (wanted_thumb_top - g.track_top) / travel;
    if (t < 0)
    {
        t = 0;
    }
    if (t > 1)
    {
        t = 1;
    }
    scroll_y_ = t * (total - bounds_.h);
    clamp_scroll();
    return true;
}

bool ScrollableBase::scrollbar_on_pointer_up()
{
    if (!scrollbar_drag_)
    {
        return false;
    }
    scrollbar_drag_ = false;
    return true;
}

} // namespace tk
