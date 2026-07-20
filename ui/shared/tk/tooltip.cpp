#include "tooltip.h"

#include "theme.h"

#include <algorithm>

namespace tk
{

namespace
{
constexpr float kPad = 4.0f;
constexpr float kRadius = 4.0f;
constexpr float kGap = 2.0f;
// Cap the tooltip's natural width so long text wraps instead of running off
// the edge of the app window; also never exceed the surface itself (minus
// the pill's own padding) on a narrow window.
constexpr float kMaxTextWidth = 360.0f;
constexpr float kRevealMs = 110.0f;
} // namespace

void Tooltip::set_content(std::string text, Rect anchor_world)
{
    text_ = std::move(text);
    anchor_ = anchor_world;
}

void Tooltip::paint_overlay(PaintCtx& ctx, Rect surface_bounds)
{
    if (text_.empty()) return;

    const float max_width = std::max(
        0.0f, std::min(kMaxTextWidth, surface_bounds.w - kPad * 4));

    if (!layout_ || layout_text_cache_ != text_ ||
        layout_max_width_cache_ != max_width)
    {
        TextStyle style{};
        style.role = FontRole::Caption;
        style.wrap = true;
        style.max_width = max_width;
        layout_ = ctx.factory.build_text(text_, style);
        layout_text_cache_ = text_;
        layout_max_width_cache_ = max_width;
    }
    if (!layout_) return;

    const Size tsz = layout_->measure();
    float tx = anchor_.x + (anchor_.w - tsz.w) / 2.0f - kPad;
    float ty = anchor_.y - tsz.h - kPad * 2 - kGap;
    if (ty < surface_bounds.y)
    {
        ty = anchor_.bottom() + kGap;
    }
    // Clamp so the tooltip stays fully inside the surface.
    tx = std::max(surface_bounds.x + kPad,
                  std::min(tx, surface_bounds.right() - tsz.w - kPad * 2 - kPad));
    ty = std::max(surface_bounds.y + kPad,
                  std::min(ty, surface_bounds.bottom() - tsz.h - kPad * 2 - kPad));

    Rect bg{tx, ty, tsz.w + kPad * 2, tsz.h + kPad * 2};

    const float t = reveal_.step(kRevealMs);
    const bool revealing = t < 1.0f;
    if (revealing)
    {
        ctx.canvas.push_opacity(t);
    }
    ctx.canvas.fill_rounded_rect(bg, kRadius, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(bg, kRadius, ctx.theme.palette.popup_border,
                                   1.0f);
    ctx.canvas.draw_text(*layout_, {bg.x + kPad, bg.y + kPad},
                          ctx.theme.palette.text_primary);
    if (revealing)
    {
        ctx.canvas.pop_opacity();
    }
}

} // namespace tk
