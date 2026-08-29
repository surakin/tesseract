#include "group_box.h"

#include "theme.h"

#include <tesseract/visual.h>

namespace tk
{

namespace
{

constexpr float kGroupBoxRadius = tesseract::visual::kRadiusSM;
constexpr float kGroupBoxBorderW = 1.0f;
// Gap between the box's top edge and a title drawn above it (fieldset-style
// caption). Callers that pass a title must reserve this much extra space of
// their own above the rect they arrange this widget to.
constexpr float kGroupBoxTitleGap = 4.0f;

} // namespace

GroupBox::GroupBox(std::string title)
    : title_(std::move(title))
{
}

Size GroupBox::measure(LayoutCtx&, Size constraints)
{
    return {constraints.w, constraints.h};
}

void GroupBox::paint(PaintCtx& ctx)
{
    if (bounds_.empty())
        return;

    const auto& pal = ctx.theme.palette;
    ctx.canvas.stroke_rounded_rect(bounds_, kGroupBoxRadius, pal.border, kGroupBoxBorderW);

    if (title_.empty())
        return;

    if (!title_layout_)
    {
        TextStyle st;
        st.role = FontRole::Caption;
        st.halign = TextHAlign::Leading;
        title_layout_ = ctx.factory.build_text(title_, st);
    }
    if (title_layout_)
    {
        const float title_h = title_layout_->measure().h;
        ctx.canvas.draw_text(*title_layout_,
                             {bounds_.x, bounds_.y - kGroupBoxTitleGap - title_h},
                             pal.text_secondary);
    }
}

} // namespace tk
