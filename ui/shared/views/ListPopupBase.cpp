#include "views/ListPopupBase.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include <algorithm>

namespace tesseract::views
{

tk::Size ListPopupBase::measure(tk::LayoutCtx&, tk::Size)
{
    return {width(), row_height() * float(visible_rows())};
}

void ListPopupBase::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
}

void ListPopupBase::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    int n           = visible_rows();
    float rh        = row_height();

    // Opaque background base — prevents transparent bleed-through on hover rows.
    ctx.canvas.fill_rect(bounds_, pal.bg);

    for (int i = 0; i < n; ++i)
    {
        tk::Rect row{bounds_.x, bounds_.y + float(i) * rh, bounds_.w, rh};

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

        // Row separator (except after last row).
        if (i < n - 1)
        {
            tk::Rect sep{row.x, row.y + row.h - 1.0f, row.w, 1.0f};
            ctx.canvas.fill_rect(sep, pal.separator);
        }
    }

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
    pressed_index_ = row_at(local.y);
    return pressed_index_ >= 0;
}

void ListPopupBase::on_pointer_up(tk::Point local, bool inside_self)
{
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

bool ListPopupBase::on_wheel(tk::Point /*local*/, float /*dx*/, float dy)
{
    if (dy == 0.0f || visible_rows() == 0)
        return false;
    int delta = dy > 0.0f ? 1 : -1;
    int next  = std::clamp(selected_index_ + delta, 0, visible_rows() - 1);
    if (next == selected_index_)
        return false;
    selected_index_ = next;
    return true;
}

} // namespace tesseract::views
