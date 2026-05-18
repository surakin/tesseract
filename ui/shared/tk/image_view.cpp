#include "image_view.h"

#include <algorithm>

namespace tk
{

// ─────────────────────────────────────────────────────────────────────────
//  ImageView
// ─────────────────────────────────────────────────────────────────────────

namespace
{

Rect fit_rect(Size container, Size content, ImageView::ContentMode mode,
              Point origin)
{
    using Mode = ImageView::ContentMode;
    if (container.w <= 0 || container.h <= 0 || content.w <= 0 ||
        content.h <= 0)
    {
        return {origin.x, origin.y, 0, 0};
    }
    float sx = container.w / content.w;
    float sy = container.h / content.h;

    float w, h;
    switch (mode)
    {
    case Mode::Cover:
    {
        float s = std::max(sx, sy);
        w = content.w * s;
        h = content.h * s;
        break;
    }
    case Mode::Contain:
    {
        float s = std::min(sx, sy);
        w = content.w * s;
        h = content.h * s;
        break;
    }
    case Mode::Fill:
        w = container.w;
        h = container.h;
        break;
    case Mode::Center:
        w = content.w;
        h = content.h;
        break;
    }
    float cx = origin.x + (container.w - w) * 0.5f;
    float cy = origin.y + (container.h - h) * 0.5f;
    return {cx, cy, w, h};
}

} // namespace

Size ImageView::measure(LayoutCtx&, Size constraints)
{
    if (explicit_size_.w > 0 && explicit_size_.h > 0)
    {
        return explicit_size_;
    }
    if (image_)
    {
        Size natural{static_cast<float>(image_->width()),
                     static_cast<float>(image_->height())};
        // Clamp to constraints if any.
        if (constraints.w > 0 && natural.w > constraints.w)
        {
            float k = constraints.w / natural.w;
            natural.w *= k;
            natural.h *= k;
        }
        if (constraints.h > 0 && natural.h > constraints.h)
        {
            float k = constraints.h / natural.h;
            natural.w *= k;
            natural.h *= k;
        }
        return natural;
    }
    return {0, 0};
}

void ImageView::paint(PaintCtx& ctx)
{
    if (!image_)
    {
        return;
    }
    Size container{bounds_.w, bounds_.h};
    Size content{static_cast<float>(image_->width()),
                 static_cast<float>(image_->height())};
    Rect dst = fit_rect(container, content, mode_, {bounds_.x, bounds_.y});
    if (mode_ == ContentMode::Cover)
    {
        ctx.canvas.push_clip_rect(bounds_);
        ctx.canvas.draw_image(*image_, dst);
        ctx.canvas.pop_clip();
    }
    else
    {
        ctx.canvas.draw_image(*image_, dst);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Avatar
// ─────────────────────────────────────────────────────────────────────────

Size Avatar::measure(LayoutCtx&, Size /*constraints*/)
{
    return {diameter_, diameter_};
}

void Avatar::paint(PaintCtx& ctx)
{
    Point centre{bounds_.x + bounds_.w * 0.5f, bounds_.y + bounds_.h * 0.5f};
    float d = std::min(diameter_, std::min(bounds_.w, bounds_.h));
    if (image_)
    {
        ctx.canvas.draw_circle_image(*image_, centre, d);
        return;
    }
    Color bg = initials_bg_.value_or(ctx.theme.palette.avatar_initials_bg);
    Color fg = initials_fg_.value_or(ctx.theme.palette.avatar_initials_text);
    ctx.canvas.draw_initials_circle(display_name_, centre, d, bg, fg);
}

} // namespace tk
