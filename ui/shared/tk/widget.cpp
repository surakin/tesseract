#include "widget.h"

namespace tk {

// `bounds_` is stored in world (root-surface) coordinates throughout the
// tree — that's the convention paint and arrange already use (every
// FlexBox::arrange call writes `parent.x + offset` into the child's
// bounds, and Label::paint draws at `{bounds_.x, bounds_.y}` directly).
// The dispatch routines below therefore traverse in world coords too:
// the inside-bounds check compares against `bounds_.x + bounds_.w` (not
// `0 + bounds_.w`), and we only convert to widget-local right before
// handing a Point to a virtual callback.

void Widget::arrange(LayoutCtx& ctx, Rect bounds) {
    bounds_ = bounds;
    // Default: every child fills the same bounds. Containers override.
    for (auto& ch : children_) {
        if (ch->visible()) ch->arrange(ctx, bounds);
    }
}

bool Widget::contains_world(Point world) const {
    return world.x >= bounds_.x && world.y >= bounds_.y &&
           world.x <  bounds_.x + bounds_.w &&
           world.y <  bounds_.y + bounds_.h;
}

Widget* Widget::dispatch_pointer_down(Point world) {
    if (!visible_ || !contains_world(world)) return nullptr;

    for (auto it = children().rbegin(); it != children().rend(); ++it) {
        Widget* ch = it->get();
        if (!ch->visible()) continue;
        if (Widget* hit = ch->dispatch_pointer_down(world)) return hit;
    }
    Point local{ world.x - bounds_.x, world.y - bounds_.y };
    return on_pointer_down(local) ? this : nullptr;
}

Widget* Widget::dispatch_pointer_move(Point world) {
    if (!visible_ || !contains_world(world)) return nullptr;

    for (auto it = children().rbegin(); it != children().rend(); ++it) {
        Widget* ch = it->get();
        if (!ch->visible()) continue;
        if (Widget* hit = ch->dispatch_pointer_move(world)) return hit;
    }
    Point local{ world.x - bounds_.x, world.y - bounds_.y };
    on_pointer_move(local);
    return this;
}

Point Widget::world_to_local(Point world) const {
    return { world.x - bounds_.x, world.y - bounds_.y };
}

bool Widget::dispatch_wheel(Point world, float dx, float dy) {
    if (!visible_ || !contains_world(world)) return false;

    // Try children first (topmost paint order).
    for (auto it = children().rbegin(); it != children().rend(); ++it) {
        Widget* ch = it->get();
        if (!ch->visible()) continue;
        if (ch->dispatch_wheel(world, dx, dy)) return true;
    }
    Point local{ world.x - bounds_.x, world.y - bounds_.y };
    return on_wheel(local, dx, dy);
}

Widget* Widget::hit_test(Point world) {
    if (!visible_ || !contains_world(world)) return nullptr;

    // Topmost child wins. Iterate in reverse paint order.
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        Widget* ch = it->get();
        if (!ch->visible()) continue;
        if (Widget* hit = ch->hit_test(world)) return hit;
    }
    return this;
}

} // namespace tk
