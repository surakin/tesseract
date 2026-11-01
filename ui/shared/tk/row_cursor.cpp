#include "row_cursor.h"

#include "host.h"
#include "scrollable_base.h"

namespace tk
{

bool RowKeyboardCursor::row_is_stop(const WidgetRowAccessibility& rows, std::size_t i)
{
    switch (rows.access_role_for_widget_row(i))
    {
    case Role::Button:
    case Role::Link:
    case Role::RadioButton:
    case Role::CheckBox:
    case Role::Switch:
    case Role::MenuItem:
    case Role::GridCell:
        break;
    default:
        return false;
    }
    if (rows.access_state_for_widget_row(i).disabled)
    {
        return false;
    }
    const Rect r = rows.access_rect_for_widget_row(i);
    return r.w > 0.0f && r.h > 0.0f;
}

bool RowKeyboardCursor::has_stops(const WidgetRowAccessibility& rows)
{
    const std::size_t n = rows.access_row_count();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (row_is_stop(rows, i))
        {
            return true;
        }
    }
    return false;
}

void RowKeyboardCursor::land_(Widget& owner, const WidgetRowAccessibility& rows, int index)
{
    index_ = index;
    const auto i = static_cast<std::size_t>(index);
    const Rect r = rows.access_rect_for_widget_row(i);
    if (auto* region = dynamic_cast<ScrollableBase*>(&owner))
    {
        region->scroll_into_view(r);
    }
    if (Host* h = owner.host())
    {
        // Re-read the rect: scrolling may have moved it.
        h->show_tooltip(&owner, rows.access_name_for_widget_row(i),
                        rows.access_rect_for_widget_row(i));
        h->request_repaint();
    }
}

bool RowKeyboardCursor::handle_key(Widget& owner, WidgetRowAccessibility& rows, const KeyEvent& e)
{
    if (!owner.has_focus() || e.ctrl || e.alt || e.meta)
    {
        return false;
    }
    const int n = static_cast<int>(rows.access_row_count());
    if (index_ >= n || (index_ >= 0 && !row_is_stop(rows, static_cast<std::size_t>(index_))))
    {
        index_ = -1;
    }

    const auto next_stop = [&](int from, int dir)
    {
        for (int i = from + dir; i >= 0 && i < n; i += dir)
        {
            if (row_is_stop(rows, static_cast<std::size_t>(i)))
            {
                return i;
            }
        }
        return -1;
    };

    switch (e.key)
    {
    case Key::Left:
    case Key::Up:
    case Key::Right:
    case Key::Down:
    {
        const int dir = (e.key == Key::Right || e.key == Key::Down) ? 1 : -1;
        const int from = index_ >= 0 ? index_ : (dir > 0 ? -1 : n);
        const int to = next_stop(from, dir);
        if (to >= 0)
        {
            land_(owner, rows, to);
        }
        return true; // swallow at the ends instead of leaking to a parent
    }
    case Key::Home:
    case Key::End:
    {
        const int to = e.key == Key::Home ? next_stop(-1, 1) : next_stop(n, -1);
        if (to >= 0)
        {
            land_(owner, rows, to);
        }
        return true;
    }
    case Key::Enter:
    case Key::Space:
        if (index_ < 0)
        {
            const int to = next_stop(-1, 1);
            if (to < 0)
            {
                return false;
            }
            land_(owner, rows, to);
            return true;
        }
        rows.access_activate_widget_row(static_cast<std::size_t>(index_));
        return true;
    default:
        return false;
    }
}

void RowKeyboardCursor::paint_ring(PaintCtx& ctx, const Widget& owner,
                                   const WidgetRowAccessibility& rows) const
{
    if (index_ >= 0 && static_cast<std::size_t>(index_) < rows.access_row_count())
    {
        const Rect r = rows.access_rect_for_widget_row(static_cast<std::size_t>(index_));
        if (r.w > 0.0f && r.h > 0.0f)
        {
            paint_focus_ring(ctx, r);
            return;
        }
    }
    paint_focus_ring(ctx, owner.bounds());
}

} // namespace tk
