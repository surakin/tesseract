#include "LanguageSection.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

// Visual constants — match AppearanceSection layout for visual consistency.
constexpr float kPadX = 24.0f;         // horizontal outer padding
constexpr float kPadY = 16.0f;         // vertical outer padding
constexpr float kBtnHPad = 20.0f;      // text → button-edge horizontal inset
constexpr float kBtnVPad = 8.0f;       // text → button-edge vertical inset
constexpr float kBtnSpacing = 8.0f;    // gap between adjacent buttons
constexpr float kBtnMinHeight = 36.0f; // minimum button height
constexpr float kBtnRadius = 6.0f;     // corner radius

// Approximate height of a single UiSemibold glyph (used in measure()).
constexpr float kGlyphH = 16.0f;
// Approximate height of a single Body glyph (for the note line).
constexpr float kNoteGlyphH = 14.0f;
constexpr float kNoteTopGap = 8.0f; // gap between buttons and note text

} // namespace

// ---------------------------------------------------------------------------

LanguageSection::LanguageSection() = default;

void LanguageSection::set_selected(const std::string& lang)
{
    selected_ = lang;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size LanguageSection::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    const float btn_h = std::max(kGlyphH + kBtnVPad * 2.0f, kBtnMinHeight);
    const float note_h = kNoteGlyphH;
    const float h = kPadY + btn_h + kNoteTopGap + note_h + kPadY;
    return {w, h};
}

void LanguageSection::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // Distribute the three buttons evenly across the available width.
    const float total_w = bounds.w - kPadX * 2.0f;
    const float gap_total = kBtnSpacing * (kButtonCount - 1);
    const float btn_w = std::max(0.0f, (total_w - gap_total) / kButtonCount);
    const float btn_h = std::max(kGlyphH + kBtnVPad * 2.0f, kBtnMinHeight);
    const float btn_y = bounds.y + kPadY;

    for (int i = 0; i < kButtonCount; ++i)
    {
        buttons_[i].bounds = {
            bounds.x + kPadX + (btn_w + kBtnSpacing) * i,
            btn_y,
            btn_w,
            btn_h,
        };
        // Invalidate text layouts when the available width changes so they
        // are rebuilt with the correct max_width on the next paint.
        buttons_[i].layout.reset();
    }

    // Invalidate note layout too.
    note_layout_.reset();
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void LanguageSection::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    for (int i = 0; i < kButtonCount; ++i)
    {
        const auto& btn = buttons_[i];
        const bool is_selected = (btn.lang_code == selected_);
        const bool is_hovered = (i == hovered_idx_);
        const bool is_pressed = (i == pressed_idx_);

        // -------- Background fill ----------------------------------------
        tk::Color fill;
        if (is_selected)
        {
            if (is_pressed)
            {
                fill = pal.accent_pressed;
            }
            else if (is_hovered)
            {
                fill = pal.accent_hover;
            }
            else
            {
                fill = pal.accent;
            }
        }
        else
        {
            if (is_pressed)
            {
                fill = pal.subtle_pressed;
            }
            else if (is_hovered)
            {
                fill = pal.subtle_hover;
            }
            else
            {
                fill = tk::Color::rgba(0, 0, 0, 0);
            }
        }

        ctx.canvas.fill_rounded_rect(btn.bounds, kBtnRadius, fill);

        // Stroke for unselected buttons to give them a clear border.
        if (!is_selected)
        {
            ctx.canvas.stroke_rounded_rect(btn.bounds, kBtnRadius, pal.border);
        }

        // -------- Label --------------------------------------------------
        if (!buttons_[i].layout)
        {
            tk::TextStyle st;
            st.role = tk::FontRole::UiSemibold;
            st.halign = tk::TextHAlign::Leading;
            st.valign = tk::TextVAlign::Top;
            st.trim = tk::TextTrim::None;
            st.max_width = -1.0f;
            buttons_[i].layout = ctx.factory.build_text(tk::tr(btn.label), st);
        }

        if (buttons_[i].layout)
        {
            const tk::Size sz = buttons_[i].layout->measure();
            const float tx = btn.bounds.x + (btn.bounds.w - sz.w) * 0.5f;
            const float ty = btn.bounds.y + (btn.bounds.h - sz.h) * 0.5f;
            const tk::Color ink =
                is_selected ? pal.text_on_accent : pal.text_primary;
            ctx.canvas.draw_text(*buttons_[i].layout, {tx, ty}, ink);
        }
    }

    // -------- Note text ("Changes take effect after restart.") ----------
    if (!note_layout_)
    {
        tk::TextStyle st;
        st.role = tk::FontRole::Body;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::None;
        st.max_width = bounds_.w - kPadX * 2.0f;
        note_layout_ =
            ctx.factory.build_text(tk::tr("Changes take effect after restart."), st);
    }

    if (note_layout_)
    {
        const float btn_h = std::max(kGlyphH + kBtnVPad * 2.0f, kBtnMinHeight);
        const float note_y = bounds_.y + kPadY + btn_h + kNoteTopGap;
        ctx.canvas.draw_text(*note_layout_, {bounds_.x + kPadX, note_y},
                             pal.text_muted);
    }
}

// ---------------------------------------------------------------------------
// Pointer handling
// ---------------------------------------------------------------------------

int LanguageSection::hit_button(tk::Point local) const
{
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
    for (int i = 0; i < kButtonCount; ++i)
    {
        const tk::Rect& r = buttons_[i].bounds;
        if (world.x >= r.x && world.x < r.x + r.w && world.y >= r.y &&
            world.y < r.y + r.h)
        {
            return i;
        }
    }
    return -1;
}

bool LanguageSection::on_pointer_down(tk::Point local)
{
    int idx = hit_button(local);
    if (idx < 0)
    {
        return false;
    }
    pressed_idx_ = idx;
    return true;
}

void LanguageSection::on_pointer_up(tk::Point local, bool inside_self)
{
    const int was_pressed = pressed_idx_;
    pressed_idx_ = -1;

    if (!inside_self || was_pressed < 0)
    {
        return;
    }

    int idx = hit_button(local);
    if (idx != was_pressed)
    {
        return;
    }

    // Selection changed?
    if (buttons_[idx].lang_code != selected_)
    {
        selected_ = buttons_[idx].lang_code;
        if (on_language_changed)
        {
            on_language_changed(selected_);
        }
    }
}

bool LanguageSection::on_pointer_move(tk::Point local)
{
    int prev = hovered_idx_;
    hovered_idx_ = hit_button(local);
    return hovered_idx_ != prev;
}

void LanguageSection::on_pointer_leave()
{
    hovered_idx_ = -1;
    pressed_idx_ = -1;
}

} // namespace tesseract::views
