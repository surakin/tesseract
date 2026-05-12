#include "widget.h"

namespace tk {

void Widget::arrange(LayoutCtx& ctx, Rect bounds) {
    bounds_ = bounds;
    // Default: every child fills the same bounds. Containers override.
    for (auto& ch : children_) {
        if (ch->visible()) ch->arrange(ctx, bounds);
    }
}

Widget* Widget::dispatch_pointer_down(Point local) {
    if (!visible_) return nullptr;
    if (local.x < 0 || local.y < 0 ||
        local.x >= bounds_.w || local.y >= bounds_.h) return nullptr;

    for (auto it = children().rbegin(); it != children().rend(); ++it) {
        Widget* ch = it->get();
        if (!ch->visible()) continue;
        Point child_local{ local.x - ch->bounds_.x,
                            local.y - ch->bounds_.y };
        if (Widget* hit = ch->dispatch_pointer_down(child_local)) return hit;
    }
    return on_pointer_down(local) ? this : nullptr;
}

Point Widget::world_to_local(Point world) const {
    Point p = world;
    for (const Widget* w = this; w; w = w->parent()) {
        p.x -= w->bounds_.x;
        p.y -= w->bounds_.y;
    }
    return p;
}

bool Widget::dispatch_wheel(Point local, float dx, float dy) {
    if (!visible_) return false;
    if (local.x < 0 || local.y < 0 ||
        local.x >= bounds_.w || local.y >= bounds_.h) return false;

    // Try children first (topmost paint order).
    for (auto it = children().rbegin(); it != children().rend(); ++it) {
        Widget* ch = it->get();
        if (!ch->visible()) continue;
        Point child_local{ local.x - ch->bounds_.x,
                            local.y - ch->bounds_.y };
        if (ch->dispatch_wheel(child_local, dx, dy)) return true;
    }
    return on_wheel(local, dx, dy);
}

Widget* Widget::hit_test(Point local) {
    if (!visible_) return nullptr;
    if (local.x < 0 || local.y < 0 ||
        local.x >= bounds_.w || local.y >= bounds_.h) return nullptr;

    // Topmost child wins. Iterate in reverse paint order.
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        Widget* ch = it->get();
        if (!ch->visible()) continue;
        Point child_local{ local.x - ch->bounds_.x,
                            local.y - ch->bounds_.y };
        if (Widget* hit = ch->hit_test(child_local)) return hit;
    }
    return this;
}

} // namespace tk
