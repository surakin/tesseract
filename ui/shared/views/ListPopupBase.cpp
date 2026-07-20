#include "views/ListPopupBase.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include <algorithm>
#include <cmath>

namespace tesseract::views
{

tk::Size ListPopupBase::measure(tk::LayoutCtx&, tk::Size)
{
    return {width(), row_height() * float(visible_rows())};
}

void ListPopupBase::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
    clamp_scroll();
}

void ListPopupBase::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    int total       = (int)row_count();
    float rh        = row_height();

    step_kinetic();

    // Opaque background base — prevents transparent bleed-through on hover rows.
    ctx.canvas.fill_rect(bounds_, pal.bg);

    ctx.canvas.push_clip_rect(bounds_);

    int first = std::max(0, (int)(scroll_y_ / rh));
    int last  = std::min(total, (int)std::ceil((scroll_y_ + bounds_.h) / rh));

    for (int i = first; i < last; ++i)
    {
        tk::Rect row{bounds_.x, bounds_.y + float(i) * rh - scroll_y_, bounds_.w,
                    rh};

        bool selected = (i == selected_index_);
        bool hovered  = (i == hovered_index_);

        // Background: selected > hovered > normal (normal == pal.bg already).
        if (selected)
        {
            ctx.canvas.fill_rect(row, pal.sidebar_selected);
        }
        else if (hovered)
        {
            ctx.canvas.fill_rect(row, pal.subtle_hover);
        }

        paint_row(ctx, row, std::size_t(i), selected, hovered);

        // Row separator (except after the last actual row — it may not be
        // the last one drawn, when the list is taller than the viewport).
        if (i < total - 1)
        {
            tk::Rect sep{row.x, row.y + row.h - 1.0f, row.w, 1.0f};
            ctx.canvas.fill_rect(sep, pal.separator);
        }
    }

    ctx.canvas.pop_clip();

    // Scrollbar overlay — only drawn when content overflows the viewport.
    paint_scrollbar(ctx);

    // 1px border around the entire popup.
    ctx.canvas.fill_rect({bounds_.x, bounds_.y, bounds_.w, 1.0f}, pal.separator);
    ctx.canvas.fill_rect(
        {bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f},
        pal.separator);
    ctx.canvas.fill_rect({bounds_.x, bounds_.y, 1.0f, bounds_.h}, pal.separator);
    ctx.canvas.fill_rect(
        {bounds_.x + bounds_.w - 1.0f, bounds_.y, 1.0f, bounds_.h},
        pal.separator);
}

bool ListPopupBase::on_pointer_down(tk::Point local)
{
    if (scrollbar_on_pointer_down(local))
    {
        return true;
    }
    pressed_index_ = row_at(local.y);
    return pressed_index_ >= 0;
}

void ListPopupBase::on_pointer_up(tk::Point local, bool inside_self)
{
    if (scrollbar_on_pointer_up())
    {
        return; // drag releases never activate a row
    }
    if (!inside_self)
    {
        pressed_index_ = -1;
        return;
    }
    int r = row_at(local.y);
    if (r >= 0 && r == pressed_index_ && r < (int)row_count())
    {
        on_row_activated(std::size_t(r));
    }
    pressed_index_ = -1;
}

void ListPopupBase::on_pointer_drag(tk::Point local)
{
    scrollbar_on_pointer_drag(local);
}

bool ListPopupBase::on_pointer_move(tk::Point local)
{
    int prev       = hovered_index_;
    hovered_index_ = row_at(local.y);
    return hovered_index_ != prev;
}

void ListPopupBase::on_pointer_leave()
{
    hovered_index_ = -1;
}

bool ListPopupBase::on_wheel(tk::Point /*local*/, float /*dx*/, float dy, bool is_touchpad)
{
    if (dy == 0.0f)
        return false;

    if (content_height() > bounds_.h)
    {
        return on_wheel_scroll(dy, is_touchpad);
    }

    // Content fits the viewport (today's case for Mention/Shortcode) — no
    // scrolling needed, so the wheel moves the selection instead.
    if (visible_rows() == 0)
        return false;
    int delta = dy > 0.0f ? 1 : -1;
    int next  = std::clamp(selected_index_ + delta, 0, visible_rows() - 1);
    if (next == selected_index_)
        return false;
    selected_index_ = next;
    return true;
}

} // namespace tesseract::views
