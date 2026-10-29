#include "radio.h"

#include "tk/host.h"

#include <algorithm>

namespace tk
{

namespace
{
constexpr float kRbSize     = 18.0f;
constexpr float kRbBorder   = 1.5f;
constexpr float kRbDot      = 8.0f;
constexpr float kRbGap      = 10.0f;
constexpr float kRbMinH     = 32.0f;
constexpr float kRbHoverRad = 4.0f;
} // namespace

RadioButton::RadioButton(std::string label) : label_(std::move(label))
{
}

Size RadioButton::measure(LayoutCtx& ctx, Size constraints)
{
    float avail_w = constraints.w > 0 ? constraints.w - kRbSize - kRbGap : -1.0f;
    if (avail_w <= 0.0f || natural_width_)
        avail_w = -1.0f; // natural-width buttons never ellipsize
    if (!label_layout_ || cached_max_w_ != avail_w)
    {
        TextStyle st{};
        st.role      = FontRole::Body;
        st.halign    = TextHAlign::Leading;
        st.valign    = TextVAlign::Top;
        st.trim      = TextTrim::Ellipsis;
        st.max_width = avail_w;
        label_layout_ = ctx.factory.build_text(label_, st);
        cached_max_w_ = avail_w;
        label_size_   = label_layout_ ? label_layout_->measure() : Size{};
    }
    const float h = std::max({kRbSize, label_size_.h, kRbMinH});
    const float natural = kRbSize + kRbGap + label_size_.w;
    const float w = (constraints.w > 0 && !natural_width_) ? constraints.w
                                                           : natural;
    return {w, h};
}

void RadioButton::arrange(LayoutCtx&, Rect bounds)
{
    bounds_ = bounds;
    const float avail_w =
        natural_width_ ? -1.0f : std::max(0.0f, bounds.w - kRbSize - kRbGap);
    if (cached_max_w_ != avail_w)
    {
        label_layout_.reset();
        cached_max_w_ = -2.0f;
    }
}

void RadioButton::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    if (enabled_ && (hovered_ || pressed_))
    {
        ctx.canvas.fill_rounded_rect(
            bounds_, kRbHoverRad,
            pressed_ ? pal.subtle_pressed : pal.subtle_hover);
    }

    const Rect ring{bounds_.x, bounds_.y + (bounds_.h - kRbSize) * 0.5f, kRbSize,
                    kRbSize};
    ctx.canvas.stroke_rounded_rect(ring, kRbSize * 0.5f,
                                   checked_ ? pal.accent : pal.border, kRbBorder);
    if (checked_)
    {
        const Rect dot{ring.x + (kRbSize - kRbDot) * 0.5f,
                       ring.y + (kRbSize - kRbDot) * 0.5f, kRbDot, kRbDot};
        ctx.canvas.fill_rounded_rect(dot, kRbDot * 0.5f, pal.accent);
    }

    if (!label_layout_)
    {
        const float avail_w =
            natural_width_ ? -1.0f : std::max(0.0f, bounds_.w - kRbSize - kRbGap);
        TextStyle st{};
        st.role      = FontRole::Body;
        st.halign    = TextHAlign::Leading;
        st.valign    = TextVAlign::Top;
        st.trim      = TextTrim::Ellipsis;
        st.max_width = avail_w;
        label_layout_ = ctx.factory.build_text(label_, st);
        label_size_   = label_layout_ ? label_layout_->measure() : Size{};
        cached_max_w_ = avail_w;
    }
    if (label_layout_)
    {
        ctx.canvas.draw_text(
            *label_layout_,
            {bounds_.x + kRbSize + kRbGap,
             bounds_.y + (bounds_.h - label_size_.h) * 0.5f},
            enabled_ ? pal.text_primary : pal.text_disabled);
    }
}

bool RadioButton::on_pointer_down(Point)
{
    if (!enabled_)
        return false;
    pressed_ = true;
    return true;
}

void RadioButton::on_pointer_up(Point, bool inside_self)
{
    const bool was = pressed_;
    pressed_ = false;
    if (was && inside_self && enabled_ && on_select_)
    {
        auto cb = on_select_;
        cb(this);
    }
}

bool RadioButton::on_pointer_move(Point)
{
    if (!enabled_ || hovered_)
        return false;
    hovered_ = true;
    return true;
}

void RadioButton::on_pointer_leave()
{
    hovered_ = false;
    pressed_ = false;
}

bool RadioButton::on_key_down(const KeyEvent& e)
{
    if (!enabled_ || !has_focus())
        return false;
    switch (e.key)
    {
    case Key::Enter:
    case Key::Space:
        if (on_select_)
        {
            auto cb = on_select_;
            cb(this);
        }
        return true;
    case Key::Left:
    case Key::Up:
    case Key::Right:
    case Key::Down:
        if (on_move_)
        {
            auto cb = on_move_;
            cb(this, (e.key == Key::Right || e.key == Key::Down) ? 1 : -1);
        }
        return true;
    default:
        return false;
    }
}

bool RadioButton::access_default_action()
{
    if (!enabled_ || !on_select_)
        return false;
    auto cb = on_select_;
    cb(this);
    return true;
}

// ---------------------------------------------------------------------------

RadioGroup::RadioGroup(Axis axis) : FlexBox(axis)
{
}

void RadioGroup::set_options(std::vector<Option> options)
{
    clear_children();
    buttons_.clear();
    values_.clear();
    for (auto& o : options)
    {
        auto b = create_widget<RadioButton>(this, std::move(o.label));
        b->set_natural_width(axis() == Axis::Horizontal);
        b->on_select_ = [this](RadioButton* self) { select_(self, true); };
        b->on_move_   = [this](RadioButton* self, int dir) { move_(self, dir); };
        buttons_.push_back(add_child(std::move(b)));
        values_.push_back(std::move(o.value));
    }
    set_selected_value(selected_);
}

void RadioGroup::set_selected_value(const std::string& value)
{
    selected_ = value;
    for (std::size_t i = 0; i < buttons_.size(); ++i)
        buttons_[i]->set_checked(values_[i] == value);
}

const std::string& RadioGroup::selected_value() const
{
    return selected_;
}

void RadioGroup::select_(RadioButton* b, bool fire)
{
    for (std::size_t i = 0; i < buttons_.size(); ++i)
    {
        if (buttons_[i] != b)
            continue;
        const bool changed = values_[i] != selected_;
        set_selected_value(values_[i]);
        if (host())
            host()->request_focus(b);
        if (changed && fire && on_changed)
        {
            auto cb = on_changed;
            cb(values_[i]);
        }
        return;
    }
}

void RadioGroup::move_(RadioButton* from, int dir)
{
    const int n = static_cast<int>(buttons_.size());
    for (int i = 0; i < n; ++i)
    {
        if (buttons_[i] != from)
            continue;
        for (int step = 1; step <= n; ++step)
        {
            RadioButton* next = buttons_[((i + dir * step) % n + n) % n];
            if (next->enabled())
            {
                select_(next, true);
                return;
            }
        }
        return;
    }
}

} // namespace tk
