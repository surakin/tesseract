#include "widget.h"

#include "host.h" // Host::queue_for_deletion, called via RootWidget::queue_for_deletion

#include <tesseract/visual.h>

#include <algorithm>
#include <cassert>

namespace tk
{

namespace detail
{
std::vector<Host*>& pending_host_stack()
{
    thread_local std::vector<Host*> stack;
    return stack;
}
} // namespace detail

// Reads whatever Host is ambient at construction time — the top of
// pending_host_stack() if create_widget()/create_root_widget() pushed one
// (widget.h), nullptr if this widget was constructed directly with
// no such call anywhere on the current call stack. See the doc comment on
// pending_host_stack() (widget.h) for why this is a stack read, not a write
// into not-yet-live storage.
Widget::Widget()
{
    auto& stack = detail::pending_host_stack();
    host_ = stack.empty() ? nullptr : stack.back();
}

void paint_drag_hover_highlight(PaintCtx& ctx, Rect rect)
{
    constexpr float kInset  = 4.0f;
    constexpr float kRadius = tesseract::visual::kRadiusMD;
    const Rect area{rect.x + kInset, rect.y + kInset,
                    std::max(0.0f, rect.w - 2.0f * kInset),
                    std::max(0.0f, rect.h - 2.0f * kInset)};
    if (area.empty())
        return;
    const Color accent = ctx.theme.palette.accent;
    ctx.canvas.fill_rounded_rect(area, kRadius, accent.with_alpha(28));
    ctx.canvas.stroke_rounded_rect(area, kRadius, accent.with_alpha(192), 2.0f);
}

void paint_focus_ring(PaintCtx& ctx, Rect rect, float radius)
{
    ctx.canvas.stroke_rounded_rect(rect, radius, ctx.theme.palette.accent, 2.0f);
}

// `bounds_` is stored in world (root-surface) coordinates throughout the
// tree — that's the convention paint and arrange already use (every
// FlexBox::arrange call writes `parent.x + offset` into the child's
// bounds, and Label::paint draws at `{bounds_.x, bounds_.y}` directly).
// The dispatch routines below therefore traverse in world coords too:
// the inside-bounds check compares against `bounds_.x + bounds_.w` (not
// `0 + bounds_.w`), and we only convert to widget-local right before
// handing a Point to a virtual callback.

void Widget::arrange(LayoutCtx& ctx, Rect bounds)
{
    bounds_ = bounds;
    // Default: every child fills the same bounds. Containers override.
    for (auto& ch : children_)
    {
        if (ch->visible())
        {
            ch->arrange(ctx, bounds);
        }
    }
}

void Widget::paint(PaintCtx& ctx)
{
    paint_children(ctx);
}

void Widget::paint_children(PaintCtx& ctx)
{
    for (auto& ch : children_)
    {
        if (ch->visible())
        {
            ch->paint(ctx);
        }
    }
}

bool Widget::contains_world(Point world) const
{
    return world.x >= bounds_.x && world.y >= bounds_.y &&
           world.x < bounds_.x + bounds_.w && world.y < bounds_.y + bounds_.h;
}

// Snapshot child raw pointers (topmost-first) before dispatching. A pointer
// handler can rebuild this widget's child list (e.g. a click that swaps
// views); iterating children_ directly would then invalidate the live
// reverse-iterator. The snapshot is stable across such a mutation.
static std::vector<Widget*>
snapshot_children_rev(const std::vector<std::unique_ptr<Widget>>& children)
{
    std::vector<Widget*> out;
    out.reserve(children.size());
    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        out.push_back(it->get());
    }
    return out;
}

Widget* Widget::dispatch_pointer_down(Point world)
{
    if (!visible_ || !contains_world(world))
    {
        return nullptr;
    }

    for (Widget* ch : snapshot_children_rev(children()))
    {
        if (!ch->visible())
        {
            continue;
        }
        if (Widget* hit = ch->dispatch_pointer_down(world))
        {
            return hit;
        }
    }
    Point local{world.x - bounds_.x, world.y - bounds_.y};
    return on_pointer_down(local) ? this : nullptr;
}

Widget* Widget::dispatch_right_click(Point world)
{
    if (!visible_ || !contains_world(world))
    {
        return nullptr;
    }
    for (Widget* ch : snapshot_children_rev(children()))
    {
        if (!ch->visible())
        {
            continue;
        }
        if (Widget* hit = ch->dispatch_right_click(world))
        {
            return hit;
        }
    }
    Point local{world.x - bounds_.x, world.y - bounds_.y};
    return on_right_click(local) ? this : nullptr;
}

Widget* Widget::dispatch_file_drop(Point world, FileDropPayload& payload)
{
    if (!visible_ || !contains_world(world))
    {
        return nullptr;
    }
    for (Widget* ch : snapshot_children_rev(children()))
    {
        if (!ch->visible())
        {
            continue;
        }
        if (Widget* hit = ch->dispatch_file_drop(world, payload))
        {
            return hit;
        }
    }
    Point local{world.x - bounds_.x, world.y - bounds_.y};
    return on_file_drop(local, payload) ? this : nullptr;
}

Widget* Widget::dispatch_drag_hover(Point world)
{
    if (!visible_ || !contains_world(world))
    {
        return nullptr;
    }
    for (Widget* ch : snapshot_children_rev(children()))
    {
        if (!ch->visible())
        {
            continue;
        }
        if (Widget* hit = ch->dispatch_drag_hover(world))
        {
            return hit;
        }
    }
    Point local{world.x - bounds_.x, world.y - bounds_.y};
    return on_drag_hover(local) ? this : nullptr;
}

bool Widget::dispatch_key_down(const KeyEvent& event)
{
    if (!visible_)
    {
        return false;
    }

    for (Widget* ch : snapshot_children_rev(children()))
    {
        if (!ch->visible())
        {
            continue;
        }
        if (ch->dispatch_key_down(event))
        {
            return true;
        }
    }
    return on_key_down(event);
}

// Two rects are considered part of the same visual "row" for reading-order
// purposes if their vertical extents overlap at all — not just exact-y
// equality — so a row of differently-sized widgets (e.g. an HBox with
// Cross::Center/End alignment, where children legitimately get slightly
// different y) still sorts left-to-right within the row instead of letting
// that small y difference take priority. Not a strict weak ordering in a
// fully general/adversarial layout, but comparisons only ever happen within
// one widget's own immediate children (see collect_focus_order below), a
// bounded and usually visually-consistent set, which keeps this practical.
static bool reading_order_less(const Rect& a, const Rect& b)
{
    const bool same_row = a.y < b.bottom() && b.y < a.bottom();
    return same_row ? a.x < b.x : a.y < b.y;
}

// Pre-order DFS in reading order — top-to-bottom rows, left-to-right within
// a row (NOT reverse/topmost-first like the dispatch_* routines above, and
// NOT raw insertion order either). At each level, children are snapshotted
// and stable-sorted by their own bounds() before recursing, so Tab
// traversal follows visual position regardless of add_child() order —
// needed for Stack/rect-positioned children (grids, pickers) as much as
// VBox/HBox ones, whose insertion order previously only coincidentally
// matched visual order. stable_sort keeps not-yet-arranged (all-zero-rect)
// or genuinely same-rect (Stack) children in their original insertion order
// as a deterministic tiebreak. Invisible subtrees are skipped entirely.
static void collect_focus_order(Widget* w, std::vector<Widget*>& out)
{
    if (!w->visible())
        return;
    if (w->focusable() && w->enabled())
        out.push_back(w);

    std::vector<Widget*> kids;
    kids.reserve(w->children().size());
    for (auto& ch : w->children())
        kids.push_back(ch.get());
    std::stable_sort(kids.begin(), kids.end(),
                     [](Widget* a, Widget* b)
                     { return reading_order_less(a->bounds(), b->bounds()); });

    for (Widget* ch : kids)
        collect_focus_order(ch, out);
}

Widget* next_focusable(Widget* root, Widget* current, bool forward)
{
    if (!root)
        return nullptr;

    std::vector<Widget*> order;
    collect_focus_order(root, order);
    if (order.empty())
        return nullptr;

    if (!current)
        return forward ? order.front() : order.back();

    auto it = std::find(order.begin(), order.end(), current);
    if (it == order.end())
        return forward ? order.front() : order.back();

    const std::size_t n   = order.size();
    std::size_t       idx = static_cast<std::size_t>(it - order.begin());
    idx                   = forward ? (idx + 1) % n : (idx + n - 1) % n;
    return order[idx];
}

void scroll_widget_into_view(Widget* w)
{
    if (!w)
        return;
    const Rect target = w->bounds();
    for (Widget* p = w->parent(); p; p = p->parent())
    {
        if (auto* region = dynamic_cast<ScrollableRegion*>(p))
            region->scroll_into_view(target);
    }
}

Widget* Widget::dispatch_pointer_move(Point world, bool* dirty)
{
    if (!visible_ || !contains_world(world))
    {
        return nullptr;
    }

    for (Widget* ch : snapshot_children_rev(children()))
    {
        if (!ch->visible())
        {
            continue;
        }
        if (Widget* hit = ch->dispatch_pointer_move(world, dirty))
        {
            return hit;
        }
    }
    if (!enabled_)
    {
        // Disabled widgets never claim hover — mirrors dispatch_pointer_down's
        // enabled_ gate at the on_pointer_down call sites. Returning nullptr
        // here (instead of `this`) also makes sure a previously hovered widget
        // gets on_pointer_leave when the pointer moves onto a disabled one.
        return nullptr;
    }
    Point local{world.x - bounds_.x, world.y - bounds_.y};
    if (on_pointer_move(local) && dirty)
    {
        *dirty = true;
    }
    return this;
}

Point Widget::world_to_local(Point world) const
{
    return {world.x - bounds_.x, world.y - bounds_.y};
}

bool Widget::dispatch_wheel(Point world, float dx, float dy)
{
    if (!visible_ || !contains_world(world))
    {
        return false;
    }

    // Try children first (topmost paint order).
    for (Widget* ch : snapshot_children_rev(children()))
    {
        if (!ch->visible())
        {
            continue;
        }
        if (ch->dispatch_wheel(world, dx, dy))
        {
            return true;
        }
    }
    Point local{world.x - bounds_.x, world.y - bounds_.y};
    return on_wheel(local, dx, dy);
}

Widget* Widget::hit_test(Point world)
{
    if (!visible_ || !contains_world(world))
    {
        return nullptr;
    }

    // Topmost child wins. Iterate in reverse paint order.
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        Widget* ch = it->get();
        if (!ch->visible())
        {
            continue;
        }
        if (Widget* hit = ch->hit_test(world))
        {
            return hit;
        }
    }
    return this;
}

void Widget::remove_child(Widget* child)
{
    assert(child != nullptr && "remove_child: child pointer is null");
    assert(child->parent_ == this && "remove_child: child's parent is not this");

    // Find the child in the children_ vector by comparing raw pointers.
    for (auto it = children_.begin(); it != children_.end(); ++it)
    {
        if (it->get() == child)
        {
            std::unique_ptr<Widget> removed = std::move(*it);
            children_.erase(it);
            removed->parent_ = nullptr;
            // If this tree is rooted in a RootWidget, ownership passes to it
            // (Host's deferred-deletion queue) instead of destroying
            // `removed` right here.
            if (RootWidget* rw = get_root_widget())
                rw->queue_for_deletion(std::move(removed));
            return;
        }
    }

    // Child not found in children_ vector — assertion failure.
    assert(false && "remove_child: child not found in children_");
}

void Widget::clear_children()
{
    if (children_.empty())
        return;

    RootWidget* rw = get_root_widget();

    for (auto& child : children_)
    {
        child->parent_ = nullptr;
        if (rw)
            rw->queue_for_deletion(std::move(child));
    }
    children_.clear();
}

RootWidget* Widget::get_root_widget()
{
    Widget* root = this;
    while (root->parent_) root = root->parent_;
    return dynamic_cast<RootWidget*>(root);
}

void RootWidget::queue_for_deletion(std::unique_ptr<Widget> subtree)
{
    host()->queue_for_deletion(std::move(subtree));
}

} // namespace tk
