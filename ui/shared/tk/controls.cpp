#include "controls.h"

#include <algorithm>

namespace tk
{

// ─────────────────────────────────────────────────────────────────────────
//  Label
// ─────────────────────────────────────────────────────────────────────────

Size Label::measure(LayoutCtx& ctx, Size constraints)
{
    float max_w = constraints.w > 0 ? constraints.w : -1.0f;
    if (cached_ && cached_max_w_ == max_w)
    {
        return cached_size_;
    }

    TextStyle st{};
    st.role = role_;
    st.halign = halign_;
    st.wrap = wrap_;
    st.trim = trim_;
    st.max_width = max_w;
    st.max_height = constraints.h > 0 ? constraints.h : -1.0f;
    cached_ = ctx.factory.build_text(text_, st);
    cached_max_w_ = max_w;
    Size s = cached_ ? cached_->measure() : Size{};
    cached_size_ = {std::max(s.w, min_size_.w), std::max(s.h, min_size_.h)};
    return cached_size_;
}

void Label::paint(PaintCtx& ctx)
{
    if (!cached_)
    {
        TextStyle st{};
        st.role = role_;
        st.halign = halign_;
        st.wrap = wrap_;
        st.trim = trim_;
        st.max_width = bounds_.w;
        cached_ = ctx.factory.build_text(text_, st);
        if (cached_)
        {
            cached_size_ = cached_->measure();
        }
    }
    if (!cached_)
    {
        return;
    }
    Color c = colour_.value_or(ctx.theme.palette.text_primary);
    ctx.canvas.draw_text(*cached_, {bounds_.x, bounds_.y}, c);
}

// ─────────────────────────────────────────────────────────────────────────
//  Separator
// ─────────────────────────────────────────────────────────────────────────

Size Separator::measure(LayoutCtx&, Size constraints)
{
    if (orientation_ == Orientation::Horizontal)
    {
        return {constraints.w > 0 ? constraints.w : 1, thickness_};
    }
    return {thickness_, constraints.h > 0 ? constraints.h : 1};
}

void Separator::paint(PaintCtx& ctx)
{
    Color c = colour_.value_or(ctx.theme.palette.separator);
    ctx.canvas.fill_rect(bounds_, c);
}

// ─────────────────────────────────────────────────────────────────────────
//  Button
// ─────────────────────────────────────────────────────────────────────────

namespace
{

constexpr float kBtnRadius = 6.0f;
constexpr float kBtnHPad = 16.0f;
constexpr float kBtnVPad = 6.0f;
constexpr float kBtnMinHeight = 32.0f;
constexpr float kBtnIconPad = 6.0f;
constexpr float kBtnIconMinSize = 28.0f;

TextStyle button_text_style()
{
    // Leading + Top so the cached layout draws at its natural origin.
    // Button::paint then offsets that origin to centre the glyph inside
    // bounds_. If we asked for Center/Center here instead, the Qt
    // backend would re-centre the text inside its layout rect — whose
    // max_height defaults to 8192 px — and the glyph would end up
    // thousands of pixels below the button.
    TextStyle st{};
    st.role = FontRole::UiSemibold;
    st.halign = TextHAlign::Leading;
    st.valign = TextVAlign::Top;
    return st;
}

// Pick fill colour given the current variant + interactive state.
Color button_fill(Button::Variant v, const Theme& th, bool enabled,
                  bool hovered, bool pressed)
{
    if (!enabled)
    {
        if (v == Button::Variant::Primary)
            return th.palette.accent.with_alpha(120);
        if (v == Button::Variant::Destructive)
            return th.palette.destructive.with_alpha(120);
        return Color::rgba(0, 0, 0, 0);
    }
    switch (v)
    {
    case Button::Variant::Primary:
        if (pressed)
        {
            return th.palette.accent_pressed;
        }
        if (hovered)
        {
            return th.palette.accent_hover;
        }
        return th.palette.accent;
    case Button::Variant::Subtle:
        if (pressed)
        {
            return th.palette.subtle_pressed;
        }
        if (hovered)
        {
            return th.palette.subtle_hover;
        }
        return Color::rgba(0, 0, 0, 0);
    case Button::Variant::Icon:
        if (pressed)
        {
            return th.palette.subtle_pressed;
        }
        if (hovered)
        {
            return th.palette.subtle_hover;
        }
        return Color::rgba(0, 0, 0, 0);
    case Button::Variant::Destructive:
        if (pressed)
        {
            return th.palette.destructive_pressed;
        }
        if (hovered)
        {
            return th.palette.destructive_hover;
        }
        return th.palette.destructive;
    }
    return th.palette.accent;
}

Color button_text(Button::Variant v, const Theme& th, bool enabled)
{
    if (!enabled)
    {
        return th.palette.text_muted;
    }
    if (v == Button::Variant::Primary || v == Button::Variant::Destructive)
    {
        return th.palette.text_on_accent;
    }
    return th.palette.text_primary;
}

} // namespace

Size Button::measure(LayoutCtx& ctx, Size constraints)
{
    if (variant_ == Variant::Icon)
    {
        float side = std::max({min_size_.w, min_size_.h, kBtnIconMinSize});
        return {side, side};
    }

    if (!cached_)
    {
        TextStyle st = button_text_style();
        // No max_width clamp — button labels are single-line and the
        // Qt/D2D/Cairo backends all clip drawText to the layout rect, so
        // a tight max_width here would slice the glyph for any button
        // whose `min_size` is smaller than `cached_w + 2 * kBtnHPad` (the
        // 40 × 40 emoji button in the compose bar is the canonical case).
        st.max_width = -1.0f;
        cached_ = ctx.factory.build_text(label_, st);
        if (cached_)
        {
            cached_size_ = cached_->measure();
        }
    }
    float w = cached_size_.w + kBtnHPad * 2;
    float h = std::max(cached_size_.h + kBtnVPad * 2, kBtnMinHeight);
    return {std::max(w, min_size_.w), std::max(h, min_size_.h)};
}

void Button::paint(PaintCtx& ctx)
{
    Color fill = button_fill(variant_, ctx.theme, enabled_, hovered_, pressed_);
    ctx.canvas.fill_rounded_rect(bounds_, kBtnRadius, fill);

    if (variant_ == Variant::Icon)
    {
        return; // icon glyph drawn by parent
    }

    if (!cached_)
    {
        TextStyle st = button_text_style();
        st.max_width = -1.0f; // see Button::measure for why
        cached_ = ctx.factory.build_text(label_, st);
        if (cached_)
        {
            cached_size_ = cached_->measure();
        }
    }
    if (!cached_)
    {
        return;
    }
    float tx = bounds_.x + (bounds_.w - cached_size_.w) * 0.5f;
    float ty = bounds_.y + (bounds_.h - cached_size_.h) * 0.5f;
    ctx.canvas.draw_text(*cached_, {tx, ty},
                         button_text(variant_, ctx.theme, enabled_));
}

void Button::click()
{
    if (enabled_ && on_click_)
    {
        // Copy the handler before invoking — a handler that destroys this
        // button (e.g. by rebuilding its parent's children) would otherwise
        // free the std::function while operator() is still executing.
        auto cb = on_click_;
        cb();
    }
}

bool Button::on_pointer_down(Point /*local*/)
{
    if (!enabled_)
    {
        return false;
    }
    pressed_ = true;
    return true;
}

void Button::on_pointer_up(Point /*local*/, bool inside_self)
{
    bool was_pressed = pressed_;
    pressed_ = false;
    if (was_pressed && inside_self && enabled_ && on_click_)
    {
        // See Button::click() — copy before invoking so the handler can
        // safely free this button.
        auto cb = on_click_;
        cb();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  SwitchButton
// ─────────────────────────────────────────────────────────────────────────

namespace
{
constexpr float kSwTrackW = 36.0f;
constexpr float kSwTrackH = 20.0f;
constexpr float kSwKnobD  = 16.0f;
constexpr float kSwKnobPad = 2.0f;
} // namespace

Size SwitchButton::measure(LayoutCtx& ctx, Size constraints)
{
    if (!cached_)
    {
        TextStyle st = button_text_style();
        st.max_width = -1.0f;
        cached_      = ctx.factory.build_text(label_, st);
        if (cached_) cached_size_ = cached_->measure();
    }
    float w = (constraints.w > 0) ? constraints.w
                                  : cached_size_.w + kBtnHPad + kSwTrackW;
    float h = std::max(cached_size_.h + kBtnVPad * 2, kBtnMinHeight);
    return {w, h};
}

void SwitchButton::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    if (hovered_)
        ctx.canvas.fill_rounded_rect(bounds_, kBtnRadius, pal.subtle_hover);

    // Label — left-aligned, vertically centred.
    if (!cached_)
    {
        TextStyle st = button_text_style();
        st.max_width = -1.0f;
        cached_      = ctx.factory.build_text(label_, st);
        if (cached_) cached_size_ = cached_->measure();
    }
    if (cached_)
    {
        float ly = bounds_.y + (bounds_.h - cached_size_.h) * 0.5f;
        ctx.canvas.draw_text(*cached_, {bounds_.x, ly},
                             enabled_ ? pal.text_primary : pal.text_muted);
    }

    // Switch track + knob — right-aligned.
    float track_x = bounds_.x + bounds_.w - kSwTrackW;
    float track_y = bounds_.y + (bounds_.h - kSwTrackH) * 0.5f;
    Rect  track{track_x, track_y, kSwTrackW, kSwTrackH};
    ctx.canvas.fill_rounded_rect(track, kSwTrackH * 0.5f,
                                 checked_ ? pal.accent : pal.subtle_pressed);

    float knob_x = checked_ ? (track_x + kSwTrackW - kSwKnobPad - kSwKnobD)
                            : (track_x + kSwKnobPad);
    float knob_y = track_y + (kSwTrackH - kSwKnobD) * 0.5f;
    Rect  knob{knob_x, knob_y, kSwKnobD, kSwKnobD};
    ctx.canvas.fill_rounded_rect(knob, kSwKnobD * 0.5f, pal.text_on_accent);
    ctx.canvas.stroke_rounded_rect(knob, kSwKnobD * 0.5f, pal.border, 1.0f);
}

bool SwitchButton::on_pointer_down(Point /*local*/)
{
    if (!enabled_) return false;
    pressed_ = true;
    return true;
}

void SwitchButton::on_pointer_up(Point /*local*/, bool inside_self)
{
    bool was_pressed = pressed_;
    pressed_ = false;
    if (was_pressed && inside_self && enabled_)
    {
        checked_ = !checked_;
        if (on_change)
        {
            auto cb = on_change; // copy: handler may rebuild parent
            cb(checked_);
        }
    }
}

bool SwitchButton::on_pointer_move(Point /*local*/)
{
    if (!hovered_) { hovered_ = true; return true; }
    return false;
}

void SwitchButton::on_pointer_leave()
{
    hovered_ = false;
    pressed_ = false;
}

// ─────────────────────────────────────────────────────────────────────────
//  CheckButton
// ─────────────────────────────────────────────────────────────────────────

namespace
{

constexpr float kCbBoxSize  = 18.0f;
constexpr float kCbBoxRad   = 4.0f;
constexpr float kCbBorder   = 1.5f;
constexpr float kCbGap      = 10.0f;
constexpr float kCbMinH     = 36.0f;
constexpr float kCbHoverRad = 4.0f;

} // namespace

CheckButton::CheckButton(std::string label, bool checked)
    : label_(std::move(label)), checked_(checked)
{
}

void CheckButton::set_checked(bool checked)
{
    checked_ = checked;
}

void CheckButton::set_enabled(bool enabled)
{
    enabled_ = enabled;
}

void CheckButton::set_font_role(FontRole role)
{
    font_role_     = role;
    label_layout_.reset();
    cached_max_w_  = -2.0f;
}

Size CheckButton::measure(LayoutCtx& ctx, Size constraints)
{
    float avail_w = constraints.w > 0
                        ? constraints.w - kCbBoxSize - kCbGap
                        : -1.0f;
    if (avail_w <= 0.0f)
        avail_w = -1.0f;

    if (!label_layout_ || cached_max_w_ != avail_w)
    {
        TextStyle st{};
        st.role      = font_role_;
        st.halign    = TextHAlign::Leading;
        st.valign    = TextVAlign::Top;
        st.trim      = TextTrim::Ellipsis;
        st.max_width = avail_w;
        label_layout_ = ctx.factory.build_text(label_, st);
        cached_max_w_ = avail_w;
        label_size_   = label_layout_ ? label_layout_->measure() : Size{};
    }

    float h = std::max({kCbBoxSize, label_size_.h, kCbMinH});
    float w = constraints.w > 0
                  ? constraints.w
                  : kCbBoxSize + kCbGap + label_size_.w;
    return {w, h};
}

void CheckButton::arrange(LayoutCtx& /*ctx*/, Rect bounds)
{
    bounds_ = bounds;
    float avail_w = std::max(0.0f, bounds.w - kCbBoxSize - kCbGap);
    if (cached_max_w_ != avail_w)
    {
        label_layout_.reset();
        cached_max_w_ = -2.0f;
    }
}

void CheckButton::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    if (hovered_ || pressed_)
    {
        Color bg = pressed_ ? pal.subtle_pressed : pal.subtle_hover;
        ctx.canvas.fill_rounded_rect(bounds_, kCbHoverRad, bg);
    }

    float box_y = bounds_.y + (bounds_.h - kCbBoxSize) * 0.5f;
    Rect  box{bounds_.x, box_y, kCbBoxSize, kCbBoxSize};

    if (checked_)
    {
        ctx.canvas.fill_rounded_rect(box, kCbBoxRad, pal.accent);
        TextStyle st{};
        st.role      = FontRole::UiSemibold;
        st.max_width = box.w;
        auto lo = ctx.factory.build_text("\xE2\x9C\x93", st); // U+2713 ✓
        if (lo)
        {
            Size sz = lo->measure();
            ctx.canvas.draw_text(*lo,
                                 {box.x + (box.w - sz.w) * 0.5f,
                                  box.y + (box.h - sz.h) * 0.5f},
                                 pal.text_on_accent);
        }
    }
    else
    {
        ctx.canvas.fill_rounded_rect(box, kCbBoxRad,
                                     Color::rgba(0, 0, 0, 0));
        ctx.canvas.stroke_rounded_rect(box, kCbBoxRad, pal.border, kCbBorder);
    }

    if (!label_layout_)
    {
        float avail_w = std::max(0.0f, bounds_.w - kCbBoxSize - kCbGap);
        TextStyle st{};
        st.role      = font_role_;
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
        float lx = bounds_.x + kCbBoxSize + kCbGap;
        float ly = bounds_.y + (bounds_.h - label_size_.h) * 0.5f;
        ctx.canvas.draw_text(*label_layout_, {lx, ly},
                             enabled_ ? pal.text_primary : pal.text_muted);
    }
}

bool CheckButton::on_pointer_down(Point /*local*/)
{
    if (!enabled_)
        return false;
    pressed_ = true;
    return true;
}

void CheckButton::on_pointer_up(Point /*local*/, bool inside_self)
{
    bool was = pressed_;
    pressed_ = false;
    if (was && inside_self && enabled_)
    {
        checked_ = !checked_;
        if (on_change)
            on_change(checked_);
    }
}

bool CheckButton::on_pointer_move(Point /*local*/)
{
    if (!hovered_)
    {
        hovered_ = true;
        return true;
    }
    return false;
}

void CheckButton::on_pointer_leave()
{
    hovered_ = false;
    pressed_ = false;
}

} // namespace tk
