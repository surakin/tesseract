#include "slider.h"

#include "canvas.h"
#include "theme.h"

#include <algorithm>
#include <cmath>

namespace tk
{

namespace
{
constexpr float kThumbD = 14.0f;
constexpr float kTrackH = 4.0f;
constexpr float kMinH = 24.0f;
} // namespace

Slider::Slider(double min, double max, double step)
    : min_(min), max_(std::max(min, max)), step_(step), page_step_(step * 10.0), value_(min),
      limit_(std::max(min, max)), buffered_(min - 1.0)
{
}

double Slider::clamp_(double v) const
{
    if (!std::isfinite(v))
    {
        return min_;
    }
    return std::clamp(v, min_, std::max(min_, std::min(limit_, max_)));
}

void Slider::set_range(double min, double max)
{
    min_ = min;
    max_ = std::max(min, max);
    limit_ = limit_explicit_ ? std::clamp(limit_, min_, max_) : max_;
    value_ = clamp_(value_);
}

void Slider::set_step(double step)
{
    step_ = step;
}

void Slider::set_page_step(double step)
{
    page_step_ = step;
}

void Slider::set_value(double v)
{
    value_ = clamp_(v);
}

void Slider::set_limit(double v)
{
    limit_explicit_ = true;
    limit_ = std::isfinite(v) ? std::clamp(v, min_, max_) : max_;
    value_ = clamp_(value_);
}

void Slider::set_buffered(double v)
{
    buffered_ = std::isfinite(v) ? v : min_ - 1.0;
}

void Slider::set_keyboard_focusable(bool f)
{
    keyboard_focusable_ = f;
}

void Slider::set_colors(std::optional<Colors> c)
{
    colors_ = c;
}

void Slider::set_accessible_name(std::string name)
{
    name_ = std::move(name);
}

float Slider::frac_(double v) const
{
    const double span = max_ - min_;
    if (!(span > 0.0))
    {
        return 0.0f;
    }
    return static_cast<float>(std::clamp((v - min_) / span, 0.0, 1.0));
}

double Slider::value_at_(float local_x) const
{
    const float travel = bounds_.w - kThumbD;
    if (!(travel > 0.0f))
    {
        return min_;
    }
    const float f = std::clamp((local_x - kThumbD * 0.5f) / travel, 0.0f, 1.0f);
    return clamp_(min_ + static_cast<double>(f) * (max_ - min_));
}

Size Slider::measure(LayoutCtx&, Size constraints)
{
    return {constraints.w > 0 ? constraints.w : 120.0f, kMinH};
}

void Slider::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    const Colors c =
        colors_.value_or(Colors{pal.border, pal.subtle_pressed, pal.accent, pal.accent});
    const float alpha_fade = enabled_ ? 1.0f : 0.5f;
    auto fade = [&](Color col)
    {
        col.a = static_cast<decltype(col.a)>(col.a * alpha_fade);
        return col;
    };

    const float travel = std::max(0.0f, bounds_.w - kThumbD);
    const float x0 = bounds_.x + kThumbD * 0.5f;
    const float cy = bounds_.y + bounds_.h * 0.5f;
    const Rect track{x0, cy - kTrackH * 0.5f, travel, kTrackH};
    ctx.canvas.fill_rounded_rect(track, kTrackH * 0.5f, fade(c.track));

    if (buffered_ >= min_)
    {
        const float bw = travel * frac_(buffered_);
        if (bw > 0.0f)
        {
            ctx.canvas.fill_rounded_rect({track.x, track.y, bw, track.h}, kTrackH * 0.5f,
                                         fade(c.buffered));
        }
    }
    const float fx = travel * frac_(value_);
    if (fx > 0.0f)
    {
        ctx.canvas.fill_rounded_rect({track.x, track.y, fx, track.h}, kTrackH * 0.5f, fade(c.fill));
    }

    const Rect thumb{x0 + fx - kThumbD * 0.5f, cy - kThumbD * 0.5f, kThumbD, kThumbD};
    ctx.canvas.fill_rounded_rect(thumb, kThumbD * 0.5f, fade(c.thumb));
}

void Slider::apply_user_(double v, bool commit)
{
    const double nv = clamp_(v);
    if (nv != value_)
    {
        value_ = nv;
        if (on_changed)
        {
            on_changed(value_);
        }
    }
    if (commit && on_committed)
    {
        on_committed(value_);
    }
}

bool Slider::on_pointer_down(Point local)
{
    if (!enabled_)
    {
        return false;
    }
    dragging_ = true;
    apply_user_(value_at_(local.x), false);
    return true;
}

void Slider::on_pointer_drag(Point local)
{
    if (!enabled_ || !dragging_)
    {
        return;
    }
    apply_user_(value_at_(local.x), false);
}

void Slider::on_pointer_up(Point local, bool /*inside_self*/)
{
    if (!dragging_)
    {
        return;
    }
    dragging_ = false;
    if (enabled_)
    {
        apply_user_(value_at_(local.x), true);
    }
}

bool Slider::on_key_down(const KeyEvent& e)
{
    if (!enabled_ || !keyboard_focusable_ || !has_focus())
    {
        return false;
    }
    switch (e.key)
    {
    case Key::Left:
    case Key::Down:
        apply_user_(value_ - step_, true);
        return true;
    case Key::Right:
    case Key::Up:
        apply_user_(value_ + step_, true);
        return true;
    case Key::PageDown:
        apply_user_(value_ - page_step_, true);
        return true;
    case Key::PageUp:
        apply_user_(value_ + page_step_, true);
        return true;
    case Key::Home:
        apply_user_(min_, true);
        return true;
    case Key::End:
        apply_user_(std::min(limit_, max_), true);
        return true;
    default:
        return false;
    }
}

void Slider::set_enabled(bool enabled)
{
    Widget::set_enabled(enabled);
    if (!enabled)
    {
        dragging_ = false;
    }
}

void Slider::on_effective_visibility_changed_(bool now_visible)
{
    if (!now_visible)
    {
        dragging_ = false;
    }
}

AccessValue Slider::access_value() const
{
    return {true, min_, max_, value_};
}

bool Slider::access_set_value(double value)
{
    if (!enabled_)
    {
        return false;
    }
    apply_user_(value, true);
    return true;
}

} // namespace tk
