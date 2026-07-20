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
constexpr float kDropRowH   = 32.0f;
constexpr float kDropRadius = tesseract::visual::kRadiusSM;
constexpr float kComboBoxHoverFadeMs = 110.0f;

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

void ComboBox::set_options(std::vector<Option> options)
{
    options_ = std::move(options);
    layouts_.clear();
    last_w_ = -1.0f;
}

void ComboBox::set_selected_value(const std::string& value)
{
    selected_value_ = value;
    if (!layouts_.empty())
    {
        layouts_[0].reset(); // invalidate button-label layout
    }
}

void ComboBox::collapse()
{
    expanded_       = false;
    hovered_option_ = -1;
    button_pressed_ = false;
}

void ComboBox::set_enabled(bool enabled)
{
    Widget::set_enabled(enabled);
    if (!enabled_) collapse();
}

// ── measure / arrange ─────────────────────────────────────────────────────────

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
        layouts_.clear();
        last_w_ = bounds.w;
    }
}

// ── world-space containment (extended for expanded popup) ────────────────────

bool ComboBox::contains_world(Point world) const
{
    if (Widget::contains_world(world))
        return true;
    // When the dropdown is open, extend the hit region to include it.
    // dropdown_rect_ is computed in paint(); it's zero until the first frame.
    if (expanded_ && !dropdown_rect_.empty())
    {
        return world.x >= dropdown_rect_.x &&
               world.y >= dropdown_rect_.y &&
               world.x <  dropdown_rect_.x + dropdown_rect_.w &&
               world.y <  dropdown_rect_.y + dropdown_rect_.h;
    }
    return false;
}

// ── hit-test helper ───────────────────────────────────────────────────────────

int ComboBox::hit_dropdown_row(Point w) const
{
    if (!expanded_)
    {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(options_.size()); ++i)
    {
        const float ry = dropdown_rect_.y + kDropRowH * static_cast<float>(i);
        if (w.x >= dropdown_rect_.x && w.x < dropdown_rect_.x + dropdown_rect_.w &&
            w.y >= ry && w.y < ry + kDropRowH)
        {
            return i;
        }
    }
    return -1;
}

// ── paint ─────────────────────────────────────────────────────────────────────

void ComboBox::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // Ensure layout slots for button label (0) + N option rows (1..N).
    const int needed = 1 + static_cast<int>(options_.size());
    if (static_cast<int>(layouts_.size()) < needed)
    {
        layouts_.resize(static_cast<std::size_t>(needed));
    }

    // ── Button ────────────────────────────────────────────────────────────────

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

    // Label of selected option (layout index 0).
    if (!layouts_[0])
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
        layouts_[0]  = ctx.factory.build_text(label_text, st);
    }
    if (layouts_[0])
    {
        const Size  sz = layouts_[0]->measure();
        const float ty = button_rect_.y + (button_rect_.h - sz.h) * 0.5f;
        ctx.canvas.draw_text(*layouts_[0], {button_rect_.x + kHPad, ty},
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

    // Register as the active popup so the host draws the dropdown after the
    // full widget tree and routes pointer events to it with priority.
    if (expanded_ && ctx.host)
        ctx.host->register_popup(this);
}

void ComboBox::on_popup_dismiss()
{
    collapse();
}

// ── overlay paint (dropdown popup — rendered after all siblings) ──────────────

void ComboBox::paint_overlay(PaintCtx& ctx)
{
    if (!expanded_)
    {
        dropdown_was_open_ = false;
        return;
    }
    if (!dropdown_was_open_)
    {
        dropdown_reveal_.reset(0.0f);
        dropdown_reveal_.set_target(1.0f);
        dropdown_was_open_ = true;
    }

    const auto& pal = ctx.theme.palette;

    // Ensure layout slots exist (paint() may not have run yet on first frame).
    const int needed = 1 + static_cast<int>(options_.size());
    if (static_cast<int>(layouts_.size()) < needed)
    {
        layouts_.resize(static_cast<std::size_t>(needed));
    }

    const float n      = static_cast<float>(options_.size());
    const float drop_h = n * kDropRowH;

    // Prefer below the button; flip above when the popup would overflow the
    // clip rect (set via set_popup_clip). If unconstrained, fall back to a
    // simple below-only placement.
    float drop_y = button_rect_.y + kBtnH;
    if (!popup_clip_.empty())
    {
        const float clip_bottom = popup_clip_.y + popup_clip_.h;
        if (drop_y + drop_h > clip_bottom)
        {
            drop_y = button_rect_.y - drop_h;
        }
        // Clamp so at least the top of the popup stays inside the clip.
        drop_y = std::clamp(drop_y, popup_clip_.y, clip_bottom - drop_h);
    }
    dropdown_rect_ = {button_rect_.x, drop_y, button_rect_.w, drop_h};

    const float reveal_t = dropdown_reveal_.step(kComboBoxHoverFadeMs);
    const bool revealing = reveal_t < 1.0f;
    if (revealing)
    {
        if (dropdown_reveal_.still_animating())
        {
            if (auto* h = host()) h->request_repaint();
        }
        ctx.canvas.push_opacity(reveal_t);
    }

    ctx.canvas.fill_rounded_rect(dropdown_rect_, kDropRadius, pal.chrome_bg);
    ctx.canvas.stroke_rounded_rect(dropdown_rect_, kDropRadius, pal.border, 1.0f);

    for (int i = 0; i < static_cast<int>(options_.size()); ++i)
    {
        const std::size_t ui = static_cast<std::size_t>(i);
        const float ry = dropdown_rect_.y + kDropRowH * static_cast<float>(i);
        const Rect  row{dropdown_rect_.x, ry, dropdown_rect_.w, kDropRowH};

        const bool is_hovered  = (i == hovered_option_);
        const bool is_selected = (options_[ui].value == selected_value_);

        if (is_hovered)
        {
            ctx.canvas.fill_rect(row, pal.subtle_hover);
        }

        // Build row label (layout index i+1). Fixed width — checkmark glyph
        // is drawn separately and doesn't affect the layout max_width here
        // because its slot (kChevronW) comes from the right edge.
        const std::size_t li = static_cast<std::size_t>(i + 1);
        if (!layouts_[li])
        {
            TextStyle st{};
            st.role      = FontRole::Body;
            st.halign    = TextHAlign::Leading;
            st.trim      = TextTrim::Ellipsis;
            st.max_width = row.w - kHPad - kChevronW;
            layouts_[li] = ctx.factory.build_text(options_[ui].label, st);
        }
        if (layouts_[li])
        {
            const Size  sz  = layouts_[li]->measure();
            const float ty  = ry + (kDropRowH - sz.h) * 0.5f;
            ctx.canvas.draw_text(*layouts_[li], {row.x + kHPad, ty},
                                 pal.text_primary);
        }

        // Checkmark on selected row.
        if (is_selected)
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

    if (revealing)
    {
        ctx.canvas.pop_opacity();
    }
}

// ── pointer events ────────────────────────────────────────────────────────────

bool ComboBox::on_pointer_down(Point local)
{
    if (!enabled_) return false;

    const Point w{local.x + bounds_.x, local.y + bounds_.y};

    if (expanded_)
    {
        const int idx = hit_dropdown_row(w);
        if (idx >= 0)
        {
            hovered_option_ = idx;
            return true; // row press; will commit on pointer_up
        }
        collapse(); // click outside dropdown
        return false;
    }

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
    const Point w{local.x + bounds_.x, local.y + bounds_.y};

    if (expanded_)
    {
        // Do not gate on inside_self: the host derives it from bounds_.h (32 px)
        // so rows below the button always get inside_self=false even when the
        // pointer is squarely inside the expanded dropdown.
        const int idx = hit_dropdown_row(w);
        if (idx >= 0)
        {
            selected_value_ = options_[static_cast<std::size_t>(idx)].value;
            layouts_[0].reset(); // invalidate button-label layout
            collapse();
            if (on_changed)
            {
                on_changed(selected_value_);
            }
        }
        else
        {
            collapse();
        }
        return;
    }

    if (button_pressed_)
    {
        button_pressed_ = false;
        const bool on_btn =
            inside_self && w.x >= button_rect_.x &&
            w.x < button_rect_.x + button_rect_.w &&
            w.y >= button_rect_.y &&
            w.y < button_rect_.y + button_rect_.h;
        if (on_btn)
        {
            expanded_       = !expanded_;
            hovered_option_ = -1;
        }
    }
}

bool ComboBox::on_pointer_move(Point local)
{
    if (!enabled_) return false;

    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    bool changed = false;

    if (expanded_)
    {
        const int prev  = hovered_option_;
        hovered_option_ = hit_dropdown_row(w);
        changed         = (hovered_option_ != prev);
    }
    else
    {
        const bool on_btn =
            w.x >= button_rect_.x && w.x < button_rect_.x + button_rect_.w &&
            w.y >= button_rect_.y && w.y < button_rect_.y + button_rect_.h;
        if (on_btn != button_hovered_)
        {
            button_hovered_ = on_btn;
            changed         = true;
        }
    }
    return changed;
}

void ComboBox::on_pointer_leave()
{
    button_hovered_ = false;
    button_pressed_ = false;
    hovered_option_ = -1;
}

// ── keyboard ──────────────────────────────────────────────────────────────────

bool ComboBox::on_key_down(const KeyEvent& e)
{
    if (!enabled_)
    {
        return false;
    }

    // Let Tab/Shift-Tab move focus elsewhere as usual, but collapse an open
    // dropdown first so it doesn't linger open on a control that no longer
    // has focus.
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
        // Enter/Space/Up/Down open the dropdown, highlighting the current
        // selection (or nothing, if the selected value isn't in options_).
        // Gated on has_focus(): while collapsed, this widget isn't
        // registered as Host's popup (see register_popup() in paint()
        // below), so it's reachable not only as the genuinely tk-focused
        // widget but also via Host::dispatch_key_down's root-wide broadcast
        // fallback (fired whenever the ACTUALLY focused widget doesn't
        // consume a key) — without this check, any other unfocused,
        // collapsed ComboBox in the tree would silently pop open on a
        // stray Up/Down/Enter/Space meant for something else entirely.
        if (has_focus() &&
            (e.key == Key::Enter || e.key == Key::Space || e.key == Key::Up ||
             e.key == Key::Down))
        {
            expanded_       = true;
            hovered_option_ = -1;
            for (int i = 0; i < static_cast<int>(options_.size()); ++i)
            {
                if (options_[static_cast<std::size_t>(i)].value == selected_value_)
                {
                    hovered_option_ = i;
                    break;
                }
            }
            return true;
        }
        return false;
    }

    // Expanded — reached either as the tk-focused widget (collapsed→just
    // opened) or, on subsequent keys, via Host's popup-first-refusal path
    // (this widget registers itself as the active popup in paint() while
    // expanded_), since both routes fall through to this same on_key_down
    // with no children to intercept it first.
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
            hovered_option_ =
                (hovered_option_ < 0)
                    ? 0
                    : std::clamp(hovered_option_ + (e.key == Key::Down ? 1 : -1),
                                 0, n - 1);
        }
        return true;
    }
    if (e.key == Key::Enter || e.key == Key::Space)
    {
        if (hovered_option_ >= 0 &&
            hovered_option_ < static_cast<int>(options_.size()))
        {
            selected_value_ =
                options_[static_cast<std::size_t>(hovered_option_)].value;
            layouts_[0].reset(); // invalidate button-label layout, matches
                                 // the existing mouse-click commit path
            collapse();
            if (on_changed)
            {
                on_changed(selected_value_);
            }
        }
        else
        {
            collapse();
        }
        return true;
    }
    return true; // swallow other keys while the dropdown is open, matching
                 // the "popup gets first refusal" contract everywhere else
                 // in this system
}

} // namespace tk
