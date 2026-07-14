#include "form_layout.h"

#include <algorithm>

namespace tk
{

FormLayoutGroup::~FormLayoutGroup()
{
    for (auto* f : members_)
        f->label_group_ = nullptr;
}

void FormLayoutGroup::add(FormLayout* f)
{
    members_.push_back(f);
}

void FormLayoutGroup::remove(FormLayout* f)
{
    members_.erase(std::remove(members_.begin(), members_.end(), f), members_.end());
}

float FormLayoutGroup::shared_label_width(LayoutCtx& ctx) const
{
    float w = 0;
    for (auto* f : members_)
        w = std::max(w, f->local_label_width(ctx));
    return w;
}

FormLayout::~FormLayout()
{
    if (label_group_)
        label_group_->remove(this);
}

FormLayout& FormLayout::set_label_group(FormLayoutGroup* group)
{
    if (label_group_ == group)
        return *this;
    if (label_group_)
        label_group_->remove(this);
    label_group_ = group;
    if (label_group_)
        label_group_->add(this);
    return *this;
}

float FormLayout::local_label_width(LayoutCtx& ctx) const
{
    float lw = 0;
    for (auto& row : rows_)
    {
        if (!row.label->visible())
            continue;
        lw = std::max(lw, row.label->measure(ctx, {0, 0}).w);
    }
    return lw;
}

Size FormLayout::measure(LayoutCtx& ctx, Size constraints)
{
    if (rows_.empty())
        return {0, 0};

    // Pass 1: find the label column width — shared across the group if one
    // is set, so this form's controls align with sibling forms' controls.
    float lw = label_group_ ? label_group_->shared_label_width(ctx) : local_label_width(ctx);

    const float inner_w = constraints.w > 0 ? constraints.w - padding_.horizontal() : 0;
    const float ctrl_w  = inner_w > 0 ? std::max(0.0f, inner_w - lw - label_gap_) : 0;

    // Pass 2: measure controls and accumulate row heights.
    float total_h        = padding_.top + padding_.bottom;
    float max_ctrl_nat_w = 0;
    bool  first          = true;
    for (auto& row : rows_)
    {
        if (!row.label->visible() && !row.control->visible())
            continue;
        if (!first)
            total_h += spacing_;
        first = false;

        Size ls        = row.label->measure(ctx, {lw, 0});
        Size cs        = row.control->measure(ctx, {ctrl_w, 0});
        max_ctrl_nat_w = std::max(max_ctrl_nat_w, cs.w);
        total_h       += std::max(ls.h, cs.h);
    }

    const float total_w = constraints.w > 0
                              ? constraints.w
                              : padding_.horizontal() + lw + label_gap_ + max_ctrl_nat_w;
    return {total_w, total_h};
}

void FormLayout::arrange(LayoutCtx& ctx, Rect bounds)
{
    bounds_ = bounds;
    if (rows_.empty())
        return;

    // Re-measure labels to get the definitive column width at arrange time.
    float lw = label_group_ ? label_group_->shared_label_width(ctx) : local_label_width(ctx);

    const float left   = bounds.x + padding_.left;
    const float ctrl_x = left + lw + label_gap_;
    const float ctrl_w = std::max(0.0f, bounds.x + bounds.w - padding_.right - ctrl_x);
    float       y      = bounds.y + padding_.top;

    bool first = true;
    for (auto& row : rows_)
    {
        if (!row.label->visible() && !row.control->visible())
            continue;
        if (!first)
            y += spacing_;
        first = false;

        Size        ls    = row.label->measure(ctx, {lw, 0});
        Size        cs    = row.control->measure(ctx, {ctrl_w, 0});
        const float row_h = std::max(ls.h, cs.h);

        row.label->arrange(ctx,
                           {left,   y + (row_h - ls.h) * 0.5f, lw,     ls.h});
        row.control->arrange(ctx,
                             {ctrl_x, y + (row_h - cs.h) * 0.5f, ctrl_w, cs.h});
        y += row_h;
    }
}

void FormLayout::paint(PaintCtx& ctx)
{
    for (auto& ch : children())
    {
        if (ch->visible())
            ch->paint(ctx);
    }
}

} // namespace tk
