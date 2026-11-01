#include "badge_flow.h"

#include "host.h"
#include "pill.h" // role_line_metrics
#include "theme.h"

#include <algorithm>

namespace tk
{

void BadgeFlow::set_badges(std::vector<Badge> badges)
{
    const bool same = badges.size() == badges_.size() &&
                      std::equal(badges.begin(), badges.end(), badges_.begin(),
                                 [](const Badge& a, const Badge& b)
                                 {
                                     return a.label == b.label && a.icon.data() == b.icon.data();
                                 });
    if (same)
    {
        // Keep the new image callbacks (they may capture a new URL) but skip
        // the relayout: nothing measurable changed.
        for (std::size_t i = 0; i < badges.size(); ++i)
        {
            badges_[i].image = std::move(badges[i].image);
        }
        return;
    }
    badges_ = std::move(badges);
    icons_.clear();
    icons_.resize(badges_.size());
    layouts_.clear();
    flow_width_ = -1.0f;
    if (host())
    {
        host()->mark_needs_relayout();
    }
}

std::string BadgeFlow::access_name() const
{
    std::string out;
    for (const auto& b : badges_)
    {
        if (!out.empty())
        {
            out += ", ";
        }
        out += b.label;
    }
    return out;
}

void BadgeFlow::on_fonts_changed()
{
    layouts_.clear();
    flow_width_ = -1.0f;
}

Size BadgeFlow::flow_(CanvasFactory& factory, float width)
{
    if (flow_width_ == width && layouts_.size() == badges_.size())
    {
        return flow_size_;
    }

    const LineMetrics lm = role_line_metrics(factory, FontRole::Small);
    row_h_ = (lm.ascent + lm.descent) > 0.0f ? (lm.ascent + lm.descent) : 16.0f;

    const float label_max = std::max(1.0f, width - kIconPx - kIconGap);
    layouts_.clear();
    placed_.assign(badges_.size(), {});
    float x = 0.0f, y = 0.0f, widest = 0.0f;
    for (std::size_t i = 0; i < badges_.size(); ++i)
    {
        TextStyle st{};
        st.role = FontRole::Small;
        st.halign = TextHAlign::Leading;
        st.trim = TextTrim::Ellipsis;
        st.max_width = label_max;
        layouts_.push_back(factory.build_text(badges_[i].label, st));
        const float text_w = layouts_.back() ? layouts_.back()->measure().w : 0.0f;
        const float w = kIconPx + kIconGap + std::min(text_w, label_max);
        if (x > 0.0f && x + w > width)
        {
            x = 0.0f;
            y += row_h_ + kRowGap;
        }
        placed_[i] = {x, y};
        x += w + kBadgeGap;
        widest = std::max(widest, placed_[i].x + w);
    }
    flow_width_ = width;
    flow_size_ = badges_.empty() ? Size{} : Size{widest, y + row_h_};
    return flow_size_;
}

Size BadgeFlow::measure(LayoutCtx& ctx, Size constraints)
{
    return flow_(ctx.factory, constraints.w > 0 ? constraints.w : 1.0e6f);
}

void BadgeFlow::paint(PaintCtx& ctx)
{
    if (badges_.empty())
    {
        return;
    }
    flow_(ctx.factory, bounds_.w);
    const Color muted = ctx.theme.palette.text_muted;
    for (std::size_t i = 0; i < badges_.size(); ++i)
    {
        const float bx = bounds_.x + placed_[i].x;
        const float by = bounds_.y + placed_[i].y;
        const Image* img = badges_[i].image ? badges_[i].image() : nullptr;
        if (img)
        {
            ctx.canvas.draw_circle_image(*img, {bx + kIconPx * 0.5f, by + row_h_ * 0.5f}, kIconPx);
        }
        else if (!badges_[i].icon.empty())
        {
            icons_[i].draw(ctx.canvas, ctx.factory, badges_[i].icon, {bx, by, kIconPx, row_h_},
                           kIconPx, muted);
        }
        if (layouts_[i])
        {
            ctx.canvas.draw_text(*layouts_[i], {bx + kIconPx + kIconGap, by}, muted);
        }
    }
}

} // namespace tk
