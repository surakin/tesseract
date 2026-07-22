#include "combobox.h"

#include "host.h"

#include <tesseract/visual.h>

#include <algorithm>

namespace tk
{

namespace
{

constexpr float kBtnH       = 32.0f;
constexpr float kBtnRadius  = tesseract::visual::kRadiusSM;
constexpr float kHPad       = 12.0f;  // text → button-edge inset
constexpr float kChevronW   = 20.0f;  // right-side chevron slot width
constexpr float kDropRadius = tesseract::visual::kRadiusSM;
constexpr float kComboBoxHoverFadeMs = 110.0f;

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  ComboBox::DropdownList — the popup surface's root widget. Owns only row
// painting + mouse hit-testing/hover; keyboard nav and commit logic stay in
// ComboBox itself (it keeps tk-level keyboard focus the whole time this is
// open — no native control involved, unlike LanguagePicker).
// ─────────────────────────────────────────────────────────────────────────

class ComboBox::DropdownList : public Widget
{
public:
    // Public, plain-constructible (like MentionPopup/etc.) — always a popup
    // surface's *root* widget, mounted via PopupSurfaceHandle::set_root().
    DropdownList() = default;

    // Data mutators only — repaint/relayout is ComboBox's job via the
    // PopupSurfaceHandle directly (this widget's own host() is always null:
    // it's mounted detached via PopupSurfaceHandle::set_root() — see
    // PopupSurfaceHandle::request_repaint()'s doc comment in host.h).
    void set_options(const std::vector<Option>* options, std::string selected_value)
    {
        options_ = options;
        selected_value_ = std::move(selected_value);
        layouts_.clear();
    }

    void set_hovered(int row)
    {
        hovered_ = row;
    }

    std::function<void(std::size_t index)> on_row_activated;
    std::function<void(int row)> on_hover_changed; // mouse-driven only

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

    void paint(PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rounded_rect(bounds_, kDropRadius, pal.chrome_bg);

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
                st.max_width = row.w - kHPad - kChevronW;
                layout = ctx.factory.build_text(opt.label, st);
            }
            if (layout)
            {
                const Size  sz = layout->measure();
                const float ty = ry + (kDropRowH - sz.h) * 0.5f;
                ctx.canvas.draw_text(*layout, {row.x + kHPad, ty}, pal.text_primary);
            }

            if (opt.value == selected_value_)
            {
                TextStyle st{};
                st.role      = FontRole::Body;
                st.max_width = kChevronW;
                auto ck = ctx.factory.build_text("\xE2\x9C\x93", st); // U+2713 ✓
                if (ck)
                {
                    const Size  sz  = ck->measure();
                    const float ckx = row.x + row.w - kHPad - sz.w;
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

private:
    int hit_row_(Point local) const
    {
        if (!options_ || local.x < bounds_.x || local.x >= bounds_.x + bounds_.w)
            return -1;
        const int idx = static_cast<int>((local.y - bounds_.y) / kDropRowH);
        return (idx >= 0 && idx < static_cast<int>(options_->size())) ? idx : -1;
    }

    const std::vector<Option>* options_ = nullptr; // borrowed from the owning ComboBox
    std::string selected_value_;
    mutable std::vector<std::unique_ptr<TextLayout>> layouts_;
    int hovered_ = -1;
};

// ─────────────────────────────────────────────────────────────────────────
//  ComboBox
// ─────────────────────────────────────────────────────────────────────────

void ComboBox::set_options(std::vector<Option> options)
{
    options_ = std::move(options);
    button_label_layout_.reset();
    last_w_ = -1.0f;
    if (dropdown_)
    {
        dropdown_->set_options(&options_, selected_value_);
        reposition_popup_(); // no-op while collapsed; refreshes if already open
    }
}

void ComboBox::set_selected_value(const std::string& value)
{
    selected_value_ = value;
    button_label_layout_.reset();
    if (dropdown_)
    {
        dropdown_->set_options(&options_, selected_value_);
        reposition_popup_();
    }
}

void ComboBox::collapse()
{
    set_expanded_(false);
    hovered_option_ = -1;
    button_pressed_ = false;
}

void ComboBox::set_enabled(bool enabled)
{
    Widget::set_enabled(enabled);
    if (!enabled_) collapse();
}

// ── measure / arrange ─────────────────────────────────────────────────────

Size ComboBox::measure(LayoutCtx& /*ctx*/, Size constraints)
{
    return {constraints.w > 0 ? constraints.w : 0, kBtnH};
}

void ComboBox::arrange(LayoutCtx& /*ctx*/, Rect bounds)
{
    bounds_      = bounds;
    button_rect_ = bounds;

    if (bounds.w != last_w_)
    {
        button_label_layout_.reset();
        last_w_ = bounds.w;
    }
    reposition_popup_();
}

// ── internal helpers ──────────────────────────────────────────────────────

void ComboBox::set_expanded_(bool expanded)
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

void ComboBox::reposition_popup_()
{
    if (!popup_ || !expanded_)
        return;
    const float h = static_cast<float>(options_.size()) * kDropRowH;
    popup_->set_rect(button_rect_, {button_rect_.w, h});
}

void ComboBox::commit_(std::size_t index)
{
    if (index >= options_.size())
        return;
    selected_value_ = options_[index].value;
    button_label_layout_.reset();
    collapse();
    if (on_changed)
        on_changed(selected_value_);
}

void ComboBox::set_hovered_(int index)
{
    hovered_option_ = index;
    if (dropdown_)
    {
        dropdown_->set_hovered(index);
        // Needed for keyboard-driven changes (on_key_down's Up/Down): unlike
        // a mouse hover, no pointer event is dispatched to the popup's own
        // surface to trigger its usual automatic repaint.
        if (popup_)
            popup_->request_repaint();
    }
}

void ComboBox::on_popup_dismiss()
{
    collapse();
}

void ComboBox::on_theme_changed(const Theme& t)
{
    if (popup_)
        popup_->set_theme(t);
}

// ── paint (button chrome only — the dropdown paints in its own surface) ──

void ComboBox::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    if (enabled_ && button_pressed_)
    {
        // Pressed is a deliberate, immediate action — no fade.
        ctx.canvas.fill_rounded_rect(button_rect_, kBtnRadius, pal.subtle_pressed);
    }
    else
    {
        button_hover_fade_.set_target(enabled_ && button_hovered_ ? 1.0f : 0.0f);
        const float fade = button_hover_fade_.step(kComboBoxHoverFadeMs);
        if (button_hover_fade_.still_animating())
        {
            if (auto* h = host())
            {
                h->request_repaint();
            }
        }
        if (fade > 0.0f)
        {
            const Color fill =
                Color::lerp(Color::rgba(0, 0, 0, 0), pal.subtle_hover, fade);
            ctx.canvas.fill_rounded_rect(button_rect_, kBtnRadius, fill);
        }
    }
    ctx.canvas.stroke_rounded_rect(button_rect_, kBtnRadius, pal.border);

    // Label of selected option.
    if (!button_label_layout_)
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
        {
            label_text = options_[0].label;
        }
        TextStyle st{};
        st.role      = FontRole::Body;
        st.halign    = TextHAlign::Leading;
        st.trim      = TextTrim::Ellipsis;
        st.max_width = button_rect_.w - kHPad - kChevronW;
        button_label_layout_ = ctx.factory.build_text(label_text, st);
    }
    if (button_label_layout_)
    {
        const Size  sz = button_label_layout_->measure();
        const float ty = button_rect_.y + (button_rect_.h - sz.h) * 0.5f;
        ctx.canvas.draw_text(*button_label_layout_, {button_rect_.x + kHPad, ty},
                             enabled_ ? pal.text_primary : pal.text_muted);
    }

    // Chevron ▾
    {
        TextStyle st{};
        st.role      = FontRole::Small;
        st.max_width = kChevronW;
        auto cv = ctx.factory.build_text("\xE2\x96\xBE", st); // U+25BE ▾
        if (cv)
        {
            const Size  sz  = cv->measure();
            const float cvx = button_rect_.x + button_rect_.w - kChevronW +
                              (kChevronW - sz.w) * 0.5f;
            const float cvy = button_rect_.y + (button_rect_.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*cv, {cvx, cvy}, pal.text_secondary);
        }
    }

    // Register as the active popup purely for Host's "click outside
    // dismisses" behavior (see this class's doc comment) — the dropdown
    // itself no longer paints here.
    if (expanded_ && ctx.host)
        ctx.host->register_popup(this);
}

// ── pointer events (button only — dropdown rows are a separate surface) ──

bool ComboBox::on_pointer_down(Point local)
{
    if (!enabled_) return false;

    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    if (w.x >= button_rect_.x && w.x < button_rect_.x + button_rect_.w &&
        w.y >= button_rect_.y && w.y < button_rect_.y + button_rect_.h)
    {
        button_pressed_ = true;
        return true;
    }
    return false;
}

void ComboBox::on_pointer_up(Point local, bool inside_self)
{
    if (!button_pressed_)
        return;
    button_pressed_ = false;
    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    const bool on_btn =
        inside_self && w.x >= button_rect_.x &&
        w.x < button_rect_.x + button_rect_.w &&
        w.y >= button_rect_.y &&
        w.y < button_rect_.y + button_rect_.h;
    if (on_btn)
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
}

bool ComboBox::on_pointer_move(Point local)
{
    if (!enabled_) return false;

    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    const bool on_btn =
        w.x >= button_rect_.x && w.x < button_rect_.x + button_rect_.w &&
        w.y >= button_rect_.y && w.y < button_rect_.y + button_rect_.h;
    if (on_btn != button_hovered_)
    {
        button_hovered_ = on_btn;
        return true;
    }
    return false;
}

void ComboBox::on_pointer_leave()
{
    button_hovered_ = false;
    button_pressed_ = false;
}

// ── keyboard ──────────────────────────────────────────────────────────────

bool ComboBox::on_key_down(const KeyEvent& e)
{
    if (!enabled_)
    {
        return false;
    }

    if (e.key == Key::Tab || e.key == Key::Backtab)
    {
        if (expanded_)
        {
            collapse();
        }
        return false;
    }

    if (!expanded_)
    {
        if (has_focus() &&
            (e.key == Key::Enter || e.key == Key::Space || e.key == Key::Up ||
             e.key == Key::Down))
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

    // Expanded — reached as the tk-focused widget (canvas keyboard focus
    // never left the button, unlike LanguagePicker's native field).
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
        {
            commit_(static_cast<std::size_t>(hovered_option_));
        }
        else
        {
            collapse();
        }
        return true;
    }
    return true; // swallow other keys while the dropdown is open
}

} // namespace tk
