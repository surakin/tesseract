#include "toast.h"

#include "theme.h"

#include <utility>

namespace tk
{

namespace
{
constexpr float kToastRevealMs = 130.0f;
} // namespace

void Toast::set_message(std::string message)
{
    if (message == message_) return;
    message_ = std::move(message);
    layout_.reset();
    reveal_.reset(0.0f);
    reveal_.set_target(1.0f);
}

void Toast::paint_overlay(PaintCtx& ctx, Rect surface_bounds)
{
    if (message_.empty()) return;

    if (!layout_ || layout_text_cache_ != message_)
    {
        TextStyle st{};
        st.role   = FontRole::Body;
        st.halign = TextHAlign::Leading;
        layout_ = ctx.factory.build_text(message_, st);
        layout_text_cache_ = message_;
    }
    if (!layout_) return;

    const Size text_sz = layout_->measure();
    const float pill_w = text_sz.w + 2.0f * kPadX;
    const float pill_h = text_sz.h + 2.0f * kPadY;
    const float pill_x = surface_bounds.x + (surface_bounds.w - pill_w) * 0.5f;
    const float pill_y =
        surface_bounds.y + surface_bounds.h - kBottomMargin - pill_h;

    const Rect pill_rect{pill_x, pill_y, pill_w, pill_h};

    const float t = reveal_.step(kToastRevealMs);
    const bool revealing = t < 1.0f;
    if (revealing)
    {
        ctx.canvas.push_opacity(t);
    }
    ctx.canvas.fill_rounded_rect(pill_rect, kRadius, ctx.theme.palette.text_primary);
    ctx.canvas.draw_text(*layout_, {pill_x + kPadX, pill_y + kPadY},
                         ctx.theme.palette.bg);
    if (revealing)
    {
        ctx.canvas.pop_opacity();
    }
}

} // namespace tk
