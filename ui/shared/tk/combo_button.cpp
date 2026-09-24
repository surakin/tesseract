#include "combo_button.h"

#include "access_tree.h"
#include "host.h"

#include <tesseract/visual.h>

#include <algorithm>

namespace tk
{

namespace
{

constexpr float kCBBtnH        = 32.0f;
constexpr float kCBBtnRadius   = tesseract::visual::kRadiusSM;
constexpr float kCBHPad        = 12.0f; // text → button-edge inset
constexpr float kCBChevronW    = 24.0f; // right-side chevron zone width
constexpr float kCBDropRadius  = tesseract::visual::kRadiusSM;
constexpr float kCBHoverFadeMs = 110.0f;

// Pick fill colour given the variant + interactive state — mirrors
// controls.cpp's anonymous-namespace button_fill(), which isn't exported.
Color cb_zone_fill(Button::Variant v, const Theme& th, bool enabled, bool hovered, bool pressed)
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
        if (pressed) return th.palette.accent_pressed;
        if (hovered) return th.palette.accent_hover;
        return th.palette.accent;
    case Button::Variant::Destructive:
        if (pressed) return th.palette.destructive_pressed;
        if (hovered) return th.palette.destructive_hover;
        return th.palette.destructive;
    case Button::Variant::Subtle:
    case Button::Variant::Icon:
        if (pressed) return th.palette.subtle_pressed;
        if (hovered) return th.palette.subtle_hover;
        return Color::rgba(0, 0, 0, 0);
    }
    return th.palette.accent;
}

Color cb_zone_text(Button::Variant v, const Theme& th, bool enabled)
{
    if (!enabled)
        return th.palette.text_muted;
    if (v == Button::Variant::Primary || v == Button::Variant::Destructive)
        return th.palette.text_on_accent;
    return th.palette.text_primary;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  ComboButton::DropdownList — the popup surface's root widget. Owns only
// row painting + mouse hit-testing/hover; keyboard nav and commit logic
// stay in ComboButton itself, mirroring ComboBox::DropdownList.
// ─────────────────────────────────────────────────────────────────────────

class ComboButton::DropdownList : public Widget, public WidgetRowAccessibility
{
public:
    DropdownList() = default;

    void set_options(const std::vector<Option>* options, std::string selected_value)
    {
        options_ = options;
        selected_value_ = std::move(selected_value);
        layouts_.clear();
    }

    void set_hovered(int row) { hovered_ = row; }

    std::function<void(std::size_t index)> on_row_activated;
    std::function<void(int row)> on_hover_changed;

    Size measure(LayoutCtx&, Size constraints) override
    {
        const float w = constraints.w > 0 ? constraints.w : 220.0f;
        const std::size_t n = options_ ? options_->size() : 0;
        return {w, static_cast<float>(n) * kDropRowH};
    }

    void arrange(LayoutCtx&, Rect bounds) override
    {
        bounds_ = bounds;
    }

    void paint_before_children(PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rounded_rect(bounds_, kCBDropRadius, pal.chrome_bg);
        ctx.canvas.stroke_rounded_rect(bounds_, kCBDropRadius, pal.popup_border, 1.0f);

        if (!options_)
            return;
        if (layouts_.size() < options_->size())
            layouts_.resize(options_->size());

        for (int i = 0; i < static_cast<int>(options_->size()); ++i)
        {
            const auto& opt = (*options_)[static_cast<std::size_t>(i)];
            const float ry = bounds_.y + kDropRowH * static_cast<float>(i);
            const Rect row{bounds_.x, ry, bounds_.w, kDropRowH};

            if (i == hovered_)
                ctx.canvas.fill_rect(row, pal.subtle_hover);

            auto& layout = layouts_[static_cast<std::size_t>(i)];
            if (!layout)
            {
                TextStyle st{};
                st.role      = FontRole::Body;
                st.halign    = TextHAlign::Leading;
                st.trim      = TextTrim::Ellipsis;
                st.max_width = row.w - kCBHPad - kCBChevronW;
                layout = ctx.factory.build_text(opt.label, st);
            }
            if (layout)
            {
                const Size  sz = layout->measure();
                const float ty = ry + (kDropRowH - sz.h) * 0.5f;
                ctx.canvas.draw_text(*layout, {row.x + kCBHPad, ty}, pal.text_primary);
            }

            if (opt.value == selected_value_)
            {
                TextStyle st{};
                st.role      = FontRole::Body;
                st.max_width = kCBChevronW;
                auto ck = ctx.factory.build_text("\xE2\x9C\x93", st); // U+2713 ✓
                if (ck)
                {
                    const Size  sz  = ck->measure();
                    const float ckx = row.x + row.w - kCBHPad - sz.w;
                    const float cky = ry + (kDropRowH - sz.h) * 0.5f;
                    ctx.canvas.draw_text(*ck, {ckx, cky}, pal.accent);
                }
            }
        }
    }

    bool on_pointer_down(Point local) override
    {
        return hit_row_(local) >= 0;
    }

    void on_pointer_up(Point local, bool /*inside_self*/) override
    {
        const int idx = hit_row_(local);
        if (idx >= 0 && on_row_activated)
            on_row_activated(static_cast<std::size_t>(idx));
    }

    bool on_pointer_move(Point local) override
    {
        const int idx = hit_row_(local);
        if (idx == hovered_)
            return false;
        hovered_ = idx;
        if (on_hover_changed)
            on_hover_changed(idx);
        return true;
    }

    void on_pointer_leave() override
    {
        if (hovered_ != -1 && on_hover_changed)
            on_hover_changed(-1);
        hovered_ = -1;
    }

    Role access_role() const override { return Role::List; }
    std::size_t access_row_count() const override
    {
        return options_ ? options_->size() : 0;
    }
    Role access_role_for_widget_row(std::size_t index) const override
    {
        return (options_ && index < options_->size()) ? Role::ListItem : Role::None;
    }
    std::string access_name_for_widget_row(std::size_t index) const override
    {
        return (options_ && index < options_->size()) ? (*options_)[index].label
                                                        : std::string{};
    }
    AccessState access_state_for_widget_row(std::size_t index) const override
    {
        AccessState s;
        if (options_ && index < options_->size())
            s.selected = (*options_)[index].value == selected_value_;
        return s;
    }
    bool access_activate_widget_row(std::size_t index) override
    {
        if (!options_ || index >= options_->size() || !on_row_activated)
            return false;
        on_row_activated(index);
        return true;
    }
    Rect access_rect_for_widget_row(std::size_t index) const override
    {
        if (!options_ || index >= options_->size())
            return {};
        return {bounds_.x, bounds_.y + kDropRowH * static_cast<float>(index),
                bounds_.w, kDropRowH};
    }

private:
    int hit_row_(Point local) const
    {
        if (!options_ || local.x < bounds_.x || local.x >= bounds_.x + bounds_.w)
            return -1;
        const int idx = static_cast<int>((local.y - bounds_.y) / kDropRowH);
        return (idx >= 0 && idx < static_cast<int>(options_->size())) ? idx : -1;
    }

    const std::vector<Option>* options_ = nullptr; // borrowed from the owning ComboButton
    std::string selected_value_;
    mutable std::vector<std::unique_ptr<TextLayout>> layouts_;
    int hovered_ = -1;
};

// ─────────────────────────────────────────────────────────────────────────
//  ComboButton
// ─────────────────────────────────────────────────────────────────────────

void ComboButton::set_options(std::vector<Option> options)
{
    options_ = std::move(options);
    invalidate_cache_();
    last_w_ = -1.0f;
    if (dropdown_)
    {
        dropdown_->set_options(&options_, selected_value_);
        reposition_popup_();
    }
}

void ComboButton::set_selected_value(const std::string& value)
{
    selected_value_ = value;
    invalidate_cache_();
    if (dropdown_)
    {
        dropdown_->set_options(&options_, selected_value_);
        reposition_popup_();
    }
}

void ComboButton::collapse()
{
    set_expanded_(false);
    hovered_option_ = -1;
    chevron_pressed_ = false;
}

void ComboButton::set_enabled(bool enabled)
{
    Widget::set_enabled(enabled);
    if (!enabled_) collapse();
}

// ── measure / arrange ─────────────────────────────────────────────────────

Size ComboButton::measure(LayoutCtx& /*ctx*/, Size constraints)
{
    return {constraints.w > 0 ? constraints.w : 0, kCBBtnH};
}

void ComboButton::arrange(LayoutCtx& /*ctx*/, Rect bounds)
{
    bounds_ = bounds;
    chevron_rect_ = {bounds.x + bounds.w - kCBChevronW, bounds.y, kCBChevronW, bounds.h};
    main_rect_ = {bounds.x, bounds.y, bounds.w - kCBChevronW, bounds.h};

    if (bounds.w != last_w_)
    {
        invalidate_cache_();
        last_w_ = bounds.w;
    }
    reposition_popup_();
}

// ── internal helpers ──────────────────────────────────────────────────────

void ComboButton::activate_()
{
    if (!enabled_)
        return;
    if (on_activate)
        on_activate(selected_value_);
}

void ComboButton::set_expanded_(bool expanded)
{
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;

    auto* h = host();
    if (!h)
        return;

    if (expanded)
    {
        if (!popup_)
        {
            popup_ = h->make_popup_surface();
            if (!popup_)
                return;
            popup_->set_corner_radius(kCBDropRadius);
            auto list = std::make_unique<DropdownList>();
            dropdown_ = list.get();
            dropdown_->on_row_activated = [this](std::size_t idx) { commit_(idx); };
            dropdown_->on_hover_changed = [this](int row) { set_hovered_(row); };
            popup_->set_root(std::move(list));
        }
        dropdown_->set_options(&options_, selected_value_);
        dropdown_->set_hovered(hovered_option_);
        reposition_popup_();
        popup_->set_visible(true);
    }
    else if (popup_)
    {
        popup_->set_visible(false);
    }
}

void ComboButton::reposition_popup_()
{
    if (!popup_ || !expanded_)
        return;
    const float h = static_cast<float>(options_.size()) * kDropRowH;
    // Anchored under the full button width (both zones), not just the
    // chevron — the dropdown reads as belonging to the whole control.
    popup_->set_rect(bounds_, {bounds_.w, h});
}

void ComboButton::commit_(std::size_t index)
{
    if (index >= options_.size())
        return;
    selected_value_ = options_[index].value;
    invalidate_cache_();
    collapse();
    if (on_selection_changed)
        on_selection_changed(selected_value_);
}

void ComboButton::set_hovered_(int index)
{
    hovered_option_ = index;
    if (dropdown_)
    {
        dropdown_->set_hovered(index);
        if (popup_)
            popup_->request_repaint();
    }
}

void ComboButton::on_popup_dismiss()
{
    collapse();
}

void ComboButton::on_theme_changed(const Theme& t)
{
    if (popup_)
        popup_->set_theme(t);
}

bool ComboButton::in_main_zone_(Point w) const
{
    return w.x >= main_rect_.x && w.x < main_rect_.x + main_rect_.w &&
           w.y >= main_rect_.y && w.y < main_rect_.y + main_rect_.h;
}

bool ComboButton::in_chevron_zone_(Point w) const
{
    return w.x >= chevron_rect_.x && w.x < chevron_rect_.x + chevron_rect_.w &&
           w.y >= chevron_rect_.y && w.y < chevron_rect_.y + chevron_rect_.h;
}

// ── paint (button chrome only — the dropdown paints in its own surface) ──

void ComboButton::paint_before_children(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // Single rounded-rect container clipped to bounds_, with the two zones
    // filled independently so hover/press feedback is per-zone while the
    // outer silhouette still reads as one control.
    ctx.canvas.fill_rounded_rect(bounds_, kCBBtnRadius, Color::rgba(0, 0, 0, 0));

    const Color main_fill =
        cb_zone_fill(variant_, ctx.theme, enabled_, main_hovered_, main_pressed_);
    const Color chevron_fill =
        cb_zone_fill(variant_, ctx.theme, enabled_, chevron_hovered_ || expanded_, chevron_pressed_);

    ctx.canvas.push_clip_rounded_rect(bounds_, kCBBtnRadius);
    ctx.canvas.fill_rounded_rect(bounds_, kCBBtnRadius, main_fill);
    ctx.canvas.fill_rect(chevron_rect_, chevron_fill);
    ctx.canvas.pop_clip();
    ctx.canvas.stroke_rounded_rect(bounds_, kCBBtnRadius, pal.border);

    // Divider between the two zones.
    ctx.canvas.draw_line({chevron_rect_.x, bounds_.y + 6.0f},
                         {chevron_rect_.x, bounds_.y + bounds_.h - 6.0f},
                         cb_zone_text(variant_, ctx.theme, enabled_).with_alpha(60), 1.0f);

    // Main zone label — selected option.
    if (!main_label_layout_)
    {
        std::string label_text;
        for (const auto& opt : options_)
        {
            if (opt.value == selected_value_)
            {
                label_text = opt.label;
                break;
            }
        }
        if (label_text.empty() && !options_.empty())
            label_text = options_[0].label;

        // FontRole::UiSemibold matches tk::Button's own button_text_style()
        // (controls.cpp) — the main zone reads as a button, same weight as
        // every other Button in this view (e.g. cancel_btn_). Leading/Top
        // for the same reason as button_text_style(): the tx/ty math below
        // centres the natural-size layout in main_rect_ itself, so letting
        // the backend also centre within max_width would double-offset it
        // toward the chevron.
        TextStyle st{};
        st.role      = FontRole::UiSemibold;
        st.halign    = TextHAlign::Leading;
        st.valign    = TextVAlign::Top;
        st.trim      = TextTrim::Ellipsis;
        st.max_width = main_rect_.w - kCBHPad;
        main_label_layout_ = ctx.factory.build_text(label_text, st);
    }
    if (main_label_layout_)
    {
        const Size  sz = main_label_layout_->measure();
        const float tx = main_rect_.x + (main_rect_.w - sz.w) * 0.5f;
        const float ty = main_rect_.y + (main_rect_.h - sz.h) * 0.5f;
        ctx.canvas.draw_text(*main_label_layout_, {tx, ty}, cb_zone_text(variant_, ctx.theme, enabled_));
    }

    // Chevron ▾
    {
        TextStyle st{};
        st.role      = FontRole::Small;
        st.max_width = kCBChevronW;
        auto cv = ctx.factory.build_text("\xE2\x96\xBE", st); // U+25BE ▾
        if (cv)
        {
            const Size  sz  = cv->measure();
            const float cvx = chevron_rect_.x + (chevron_rect_.w - sz.w) * 0.5f;
            const float cvy = chevron_rect_.y + (chevron_rect_.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*cv, {cvx, cvy}, cb_zone_text(variant_, ctx.theme, enabled_));
        }
    }

    if (expanded_ && ctx.host)
        ctx.host->register_popup(this);
}

// ── pointer events (button chrome only — dropdown rows are a separate surface) ──

bool ComboButton::on_pointer_down(Point local)
{
    if (!enabled_) return false;

    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    if (in_chevron_zone_(w))
    {
        chevron_pressed_ = true;
        return true;
    }
    if (in_main_zone_(w))
    {
        main_pressed_ = true;
        return true;
    }
    return false;
}

void ComboButton::on_pointer_up(Point local, bool inside_self)
{
    const Point w{local.x + bounds_.x, local.y + bounds_.y};

    if (chevron_pressed_)
    {
        chevron_pressed_ = false;
        if (inside_self && in_chevron_zone_(w))
        {
            if (expanded_)
            {
                collapse();
            }
            else
            {
                set_hovered_(-1);
                for (int i = 0; i < static_cast<int>(options_.size()); ++i)
                {
                    if (options_[static_cast<std::size_t>(i)].value == selected_value_)
                    {
                        set_hovered_(i);
                        break;
                    }
                }
                set_expanded_(true);
            }
        }
        return;
    }
    if (main_pressed_)
    {
        main_pressed_ = false;
        if (inside_self && in_main_zone_(w))
        {
            activate_();
        }
    }
}

bool ComboButton::on_pointer_move(Point local)
{
    if (!enabled_) return false;

    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    const bool on_main = in_main_zone_(w);
    const bool on_chevron = in_chevron_zone_(w);

    bool changed = false;
    if (on_main != main_hovered_)
    {
        main_hovered_ = on_main;
        changed = true;
    }
    if (on_chevron != chevron_hovered_)
    {
        chevron_hovered_ = on_chevron;
        changed = true;
    }
    return changed;
}

void ComboButton::on_pointer_leave()
{
    main_hovered_ = false;
    main_pressed_ = false;
    chevron_hovered_ = false;
    chevron_pressed_ = false;
}

// ── keyboard ──────────────────────────────────────────────────────────────

bool ComboButton::on_key_down(const KeyEvent& e)
{
    if (!enabled_)
        return false;

    if (e.key == Key::Tab || e.key == Key::Backtab)
    {
        if (expanded_)
            collapse();
        return false;
    }

    if (!expanded_)
    {
        if (!has_focus())
            return false;

        if (e.key == Key::Enter || e.key == Key::Space)
        {
            activate_();
            return true;
        }
        if (e.key == Key::Down)
        {
            int start = -1;
            for (int i = 0; i < static_cast<int>(options_.size()); ++i)
            {
                if (options_[static_cast<std::size_t>(i)].value == selected_value_)
                {
                    start = i;
                    break;
                }
            }
            set_expanded_(true);
            set_hovered_(start);
            return true;
        }
        return false;
    }

    // Expanded.
    if (e.key == Key::Escape)
    {
        collapse();
        return true;
    }
    if (e.key == Key::Up || e.key == Key::Down)
    {
        const int n = static_cast<int>(options_.size());
        if (n > 0)
        {
            set_hovered_(hovered_option_ < 0
                             ? 0
                             : std::clamp(hovered_option_ + (e.key == Key::Down ? 1 : -1),
                                          0, n - 1));
        }
        return true;
    }
    if (e.key == Key::Enter || e.key == Key::Space)
    {
        if (hovered_option_ >= 0 && hovered_option_ < static_cast<int>(options_.size()))
            commit_(static_cast<std::size_t>(hovered_option_));
        else
            collapse();
        return true;
    }
    return true; // swallow other keys while the dropdown is open
}

} // namespace tk
