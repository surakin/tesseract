#include "controls.h"

#include <algorithm>

namespace tk {

// ─────────────────────────────────────────────────────────────────────────
//  Label
// ─────────────────────────────────────────────────────────────────────────

Size Label::measure(LayoutCtx& ctx, Size constraints) {
    float max_w = constraints.w > 0 ? constraints.w : -1.0f;
    if (cached_ && cached_max_w_ == max_w) return cached_size_;

    TextStyle st{};
    st.role       = role_;
    st.halign     = halign_;
    st.wrap       = wrap_;
    st.trim       = trim_;
    st.max_width  = max_w;
    st.max_height = constraints.h > 0 ? constraints.h : -1.0f;
    cached_       = ctx.factory.build_text(text_, st);
    cached_max_w_ = max_w;
    cached_size_  = cached_ ? cached_->measure() : Size{};
    return cached_size_;
}

void Label::paint(PaintCtx& ctx) {
    if (!cached_) {
        TextStyle st{};
        st.role      = role_;
        st.halign    = halign_;
        st.wrap      = wrap_;
        st.trim      = trim_;
        st.max_width = bounds_.w;
        cached_      = ctx.factory.build_text(text_, st);
        if (cached_) cached_size_ = cached_->measure();
    }
    if (!cached_) return;
    Color c = colour_.value_or(ctx.theme.palette.text_primary);
    ctx.canvas.draw_text(*cached_, { bounds_.x, bounds_.y }, c);
}

// ─────────────────────────────────────────────────────────────────────────
//  Separator
// ─────────────────────────────────────────────────────────────────────────

Size Separator::measure(LayoutCtx&, Size constraints) {
    if (orientation_ == Orientation::Horizontal) {
        return { constraints.w > 0 ? constraints.w : 1, thickness_ };
    }
    return { thickness_, constraints.h > 0 ? constraints.h : 1 };
}

void Separator::paint(PaintCtx& ctx) {
    Color c = colour_.value_or(ctx.theme.palette.separator);
    ctx.canvas.fill_rect(bounds_, c);
}

// ─────────────────────────────────────────────────────────────────────────
//  Button
// ─────────────────────────────────────────────────────────────────────────

namespace {

constexpr float kBtnRadius      = 6.0f;
constexpr float kBtnHPad        = 16.0f;
constexpr float kBtnVPad        = 6.0f;
constexpr float kBtnMinHeight   = 32.0f;
constexpr float kBtnIconPad     = 6.0f;
constexpr float kBtnIconMinSize = 28.0f;

TextStyle button_text_style() {
    TextStyle st{};
    st.role   = FontRole::UiSemibold;
    st.halign = TextHAlign::Center;
    st.valign = TextVAlign::Center;
    return st;
}

// Pick fill colour given the current variant + interactive state.
Color button_fill(Button::Variant v, const Theme& th,
                   bool enabled, bool hovered, bool pressed) {
    if (!enabled) {
        return (v == Button::Variant::Primary)
            ? th.palette.accent.with_alpha(120)
            : Color::rgba(0, 0, 0, 0);
    }
    switch (v) {
        case Button::Variant::Primary:
            if (pressed) return th.palette.accent_pressed;
            if (hovered) return th.palette.accent_hover;
            return th.palette.accent;
        case Button::Variant::Subtle:
            if (pressed) return th.palette.subtle_pressed;
            if (hovered) return th.palette.subtle_hover;
            return Color::rgba(0, 0, 0, 0);
        case Button::Variant::Icon:
            if (pressed) return th.palette.subtle_pressed;
            if (hovered) return th.palette.subtle_hover;
            return Color::rgba(0, 0, 0, 0);
    }
    return th.palette.accent;
}

Color button_text(Button::Variant v, const Theme& th, bool enabled) {
    if (!enabled) return th.palette.text_muted;
    if (v == Button::Variant::Primary) return th.palette.text_on_accent;
    return th.palette.text_primary;
}

} // namespace

Size Button::measure(LayoutCtx& ctx, Size constraints) {
    if (variant_ == Variant::Icon) {
        float side = std::max({ min_size_.w, min_size_.h, kBtnIconMinSize });
        return { side, side };
    }

    if (!cached_) {
        TextStyle st = button_text_style();
        st.max_width = constraints.w > 0 ? constraints.w - kBtnHPad * 2 : -1.0f;
        cached_ = ctx.factory.build_text(label_, st);
        if (cached_) cached_size_ = cached_->measure();
    }
    float w = cached_size_.w + kBtnHPad * 2;
    float h = std::max(cached_size_.h + kBtnVPad * 2, kBtnMinHeight);
    return { std::max(w, min_size_.w),
             std::max(h, min_size_.h) };
}

void Button::paint(PaintCtx& ctx) {
    Color fill = button_fill(variant_, ctx.theme,
                              enabled_, hovered_, pressed_);
    ctx.canvas.fill_rounded_rect(bounds_, kBtnRadius, fill);

    if (variant_ == Variant::Icon) return;     // icon glyph drawn by parent

    if (!cached_) {
        TextStyle st = button_text_style();
        st.max_width = bounds_.w - kBtnHPad * 2;
        cached_ = ctx.factory.build_text(label_, st);
        if (cached_) cached_size_ = cached_->measure();
    }
    if (!cached_) return;
    float tx = bounds_.x + (bounds_.w - cached_size_.w) * 0.5f;
    float ty = bounds_.y + (bounds_.h - cached_size_.h) * 0.5f;
    ctx.canvas.draw_text(*cached_, { tx, ty },
                          button_text(variant_, ctx.theme, enabled_));
}

void Button::click() {
    if (enabled_ && on_click_) on_click_();
}

bool Button::on_pointer_down(Point /*local*/) {
    if (!enabled_) return false;
    pressed_ = true;
    return true;
}

void Button::on_pointer_up(Point /*local*/, bool inside_self) {
    bool was_pressed = pressed_;
    pressed_ = false;
    if (was_pressed && inside_self && enabled_ && on_click_) on_click_();
}

} // namespace tk
