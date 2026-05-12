#include "layout.h"

#include <algorithm>

namespace tk {

namespace {

// Tiny axis-abstraction: pack/unpack Size and Rect along the layout
// axis. Lets the measure/arrange code be written once for both
// orientations.
struct AxisOps {
    bool vertical;

    float main(Size s)  const { return vertical ? s.h : s.w; }
    float cross(Size s) const { return vertical ? s.w : s.h; }
    Size  pack(float main, float cross) const {
        return vertical ? Size{ cross, main } : Size{ main, cross };
    }
    Rect  pack_rect(float main_origin, float main_size,
                     float cross_origin, float cross_size,
                     Rect parent) const {
        if (vertical) {
            return { parent.x + cross_origin, parent.y + main_origin,
                     cross_size, main_size };
        }
        return { parent.x + main_origin, parent.y + cross_origin,
                 main_size, cross_size };
    }
    float main_pad_lo (Edges e) const { return vertical ? e.top    : e.left;  }
    float main_pad_hi (Edges e) const { return vertical ? e.bottom : e.right; }
    float cross_pad_lo(Edges e) const { return vertical ? e.left   : e.top;   }
    float cross_pad_hi(Edges e) const { return vertical ? e.right  : e.bottom;}
};

} // namespace

Size FlexBox::measure(LayoutCtx& ctx, Size constraints) {
    AxisOps ax{ axis_ == Axis::Vertical };

    float pad_main  = ax.main_pad_lo (padding_) + ax.main_pad_hi (padding_);
    float pad_cross = ax.cross_pad_lo(padding_) + ax.cross_pad_hi(padding_);

    float avail_main  = std::max(0.0f, ax.main (constraints) - pad_main);
    float avail_cross = std::max(0.0f, ax.cross(constraints) - pad_cross);

    int visible_count = 0;
    for (auto& ch : children()) if (ch->visible()) ++visible_count;
    float total_spacing = visible_count > 1
        ? spacing_ * (visible_count - 1)
        : 0.0f;

    float used_main      = 0;
    float max_cross_seen = 0;
    Size  child_constraints = ax.pack(avail_main, avail_cross);
    for (auto& ch : children()) {
        if (!ch->visible()) continue;
        Size s = ch->measure(ctx, child_constraints);
        if (!ch->layout_hints().fill_main) {
            used_main += ax.main(s);
        }
        max_cross_seen = std::max(max_cross_seen, ax.cross(s));
    }
    used_main += total_spacing;

    float content_cross = avail_cross > 0
        ? std::min(max_cross_seen, avail_cross)
        : max_cross_seen;
    return ax.pack(std::min(used_main + 0.0f, avail_main),
                    content_cross + pad_cross);
}

void FlexBox::arrange(LayoutCtx& ctx, Rect bounds) {
    bounds_ = bounds;
    AxisOps ax{ axis_ == Axis::Vertical };

    float pad_main_lo  = ax.main_pad_lo (padding_);
    float pad_main_hi  = ax.main_pad_hi (padding_);
    float pad_cross_lo = ax.cross_pad_lo(padding_);
    float pad_cross_hi = ax.cross_pad_hi(padding_);

    float inner_main_size  = std::max(0.0f,
        ax.main(Size{ bounds.w, bounds.h }) - pad_main_lo - pad_main_hi);
    float inner_cross_size = std::max(0.0f,
        ax.cross(Size{ bounds.w, bounds.h }) - pad_cross_lo - pad_cross_hi);

    // First pass: measure non-flex children + count flex children.
    std::vector<Size> measured;
    measured.reserve(children().size());
    float fixed_main_total = 0;
    int   flex_count       = 0;
    int   visible_count    = 0;
    for (auto& ch : children()) {
        Size s{};
        if (ch->visible()) {
            ++visible_count;
            Size child_constraints = ax.pack(inner_main_size, inner_cross_size);
            s = ch->measure(ctx, child_constraints);
            if (ch->layout_hints().fill_main) {
                ++flex_count;
            } else {
                fixed_main_total += ax.main(s);
            }
        }
        measured.push_back(s);
    }

    float total_spacing = visible_count > 1
        ? spacing_ * (visible_count - 1)
        : 0.0f;
    float flex_pool = std::max(0.0f,
        inner_main_size - fixed_main_total - total_spacing);
    float flex_each = flex_count > 0 ? flex_pool / flex_count : 0;

    // Main-axis start offset for the first child.
    float leftover = std::max(0.0f,
        inner_main_size - fixed_main_total - total_spacing
            - flex_each * flex_count);
    float gap_extra = 0;   // per-child gap above the natural spacing
    float origin_main = pad_main_lo;
    switch (main_align_) {
        case Main::Start:        break;
        case Main::Center:       origin_main += leftover * 0.5f; break;
        case Main::End:          origin_main += leftover;        break;
        case Main::SpaceBetween:
            if (visible_count > 1)
                gap_extra = leftover / (visible_count - 1);
            break;
        case Main::SpaceAround:
            if (visible_count > 0) {
                gap_extra   = leftover / visible_count;
                origin_main += gap_extra * 0.5f;
            }
            break;
    }

    // Second pass: arrange.
    float cursor = origin_main;
    for (size_t i = 0; i < children().size(); ++i) {
        Widget* ch = children()[i].get();
        if (!ch->visible()) continue;

        float my_main  = ch->layout_hints().fill_main
            ? flex_each
            : ax.main(measured[i]);
        float my_cross = ch->layout_hints().fill_cross
                          || cross_align_ == Cross::Stretch
            ? inner_cross_size
            : std::min(ax.cross(measured[i]), inner_cross_size);

        float cross_origin = pad_cross_lo;
        switch (cross_align_) {
            case Cross::Start:   break;
            case Cross::Center:  cross_origin += (inner_cross_size - my_cross) * 0.5f; break;
            case Cross::End:     cross_origin += inner_cross_size - my_cross;          break;
            case Cross::Stretch: break;   // cross size already = inner_cross_size
        }

        Rect child_bounds = ax.pack_rect(cursor, my_main,
                                          cross_origin, my_cross,
                                          bounds);
        ch->arrange(ctx, child_bounds);

        cursor += my_main + spacing_ + gap_extra;
    }
}

void FlexBox::paint(PaintCtx& ctx) {
    for (auto& ch : children()) {
        if (ch->visible()) ch->paint(ctx);
    }
}

} // namespace tk
