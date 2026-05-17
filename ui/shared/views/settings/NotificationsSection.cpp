#include "NotificationsSection.h"

#include "tesseract/settings.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

// Visual constants.
constexpr float kPadX       = 24.0f;  // horizontal outer padding
constexpr float kPadY       = 14.0f;  // vertical outer padding
constexpr float kBoxSize    = 16.0f;  // checkbox square side length
constexpr float kBoxRadius  =  3.0f;  // checkbox corner radius
constexpr float kBoxBorder  =  1.5f;  // stroke width of unchecked outline
constexpr float kLabelGap   = 10.0f;  // gap between checkbox and label text
constexpr float kRowRadius  =  6.0f;  // hover background corner radius

// Approximate glyph height for Body role, used in measure().
constexpr float kGlyphH = 17.0f;

// Row height = max(checkbox, glyph) + 2 × vertical padding.
constexpr float kRowH = std::max(kBoxSize, kGlyphH) + kPadY * 2.0f;

} // namespace

// ---------------------------------------------------------------------------

NotificationsSection::NotificationsSection()
    : checked_(tesseract::Settings::instance().notifications_enabled)
{}

void NotificationsSection::set_checked(bool enabled)
{
    checked_ = enabled;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size NotificationsSection::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    return { w, kRowH };
}

void NotificationsSection::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
    // Drop cached label so it gets rebuilt with the new available width.
    label_layout_.reset();
}

// ---------------------------------------------------------------------------
// Paint helpers
// ---------------------------------------------------------------------------

// Draw a simple checkmark inside `box` using two filled rectangles.
//
// The tick is an axis-aligned "L" shape that reads as a check at small sizes:
//
//   ┌──────────┐
//   │          │   ←  box
//   │   ┃      │
//   │   ┣━━━━┓ │
//   │         ┃ │
//   └──────────┘
//
// Left arm: a narrow vertical rect from the mid-left down to the base.
// Right arm: a narrow horizontal rect from the base-centre to the right wall.
// At 16 × 16 px this renders as a recognisable tick in all four backends
// without requiring line-drawing primitives.
void NotificationsSection::draw_checkmark(tk::Canvas& canvas,
                                           tk::Rect box,
                                           tk::Color ink) const
{
    // Inset from the box edges so the tick has breathing room.
    const float margin = box.w * 0.18f;
    const float ox     = box.x + margin;
    const float oy     = box.y + margin;
    const float iw     = box.w - margin * 2.0f;
    const float ih     = box.h - margin * 2.0f;

    // Stroke thickness: ~2 px for a 16 px box.
    const float thick  = std::max(2.0f, box.w * 0.13f);

    // Pivot: where the two arms of the tick meet.
    const float pivot_x = ox + iw * 0.38f;
    const float pivot_y = oy + ih * 0.62f;

    // Left (short, vertical) arm: top → pivot.
    canvas.fill_rect({ ox, oy + ih * 0.30f,
                       thick, pivot_y - (oy + ih * 0.30f) + thick }, ink);

    // Base connector (short horizontal segment at the bottom of the left arm).
    canvas.fill_rect({ ox, pivot_y, pivot_x - ox + thick, thick }, ink);

    // Right (long, vertical) arm: pivot → top-right.
    canvas.fill_rect({ pivot_x, oy, thick, pivot_y - oy + thick }, ink);
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void NotificationsSection::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // -------- Hover backdrop ------------------------------------------------
    if (hovered_ || pressed_)
    {
        tk::Rect hover_r = bounds_;
        hover_r.x += 4.0f;
        hover_r.y += 4.0f;
        hover_r.w -= 8.0f;
        hover_r.h -= 8.0f;
        const tk::Color bg = pressed_ ? pal.subtle_pressed : pal.subtle_hover;
        ctx.canvas.fill_rounded_rect(hover_r, kRowRadius, bg);
    }

    // -------- Checkbox ------------------------------------------------------
    const float box_y = bounds_.y + (bounds_.h - kBoxSize) * 0.5f;
    const tk::Rect box { bounds_.x + kPadX, box_y, kBoxSize, kBoxSize };

    if (checked_)
    {
        ctx.canvas.fill_rounded_rect(box, kBoxRadius, pal.accent);
        draw_checkmark(ctx.canvas, box, pal.text_on_accent);
    }
    else
    {
        // Transparent fill + visible border.
        ctx.canvas.fill_rounded_rect(box, kBoxRadius,
                                     tk::Color::rgba(0, 0, 0, 0));
        ctx.canvas.stroke_rounded_rect(box, kBoxRadius, pal.border, kBoxBorder);
    }

    // -------- Label ---------------------------------------------------------
    const float label_x = box.x + kBoxSize + kLabelGap;
    const float label_w = std::max(0.0f,
        bounds_.x + bounds_.w - kPadX - label_x);

    if (!label_layout_)
    {
        tk::TextStyle st;
        st.role      = tk::FontRole::Body;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = label_w;
        label_layout_ = ctx.factory.build_text(
            "Enable notifications on this device", st);
    }

    if (label_layout_)
    {
        const tk::Size sz = label_layout_->measure();
        const float    ty = bounds_.y + (bounds_.h - sz.h) * 0.5f;
        ctx.canvas.draw_text(*label_layout_, { label_x, ty }, pal.text_primary);
    }
}

// ---------------------------------------------------------------------------
// Pointer handling
// ---------------------------------------------------------------------------

bool NotificationsSection::hit_row(tk::Point local) const
{
    // Treat the entire row (excluding horizontal outer padding) as the hit
    // target — the user should be able to click the label text too.
    return local.x >= kPadX && local.x < bounds_.w - kPadX &&
           local.y >= 0     && local.y < bounds_.h;
}

bool NotificationsSection::on_pointer_down(tk::Point local)
{
    if (!hit_row(local)) { return false; }
    pressed_ = true;
    return true;
}

void NotificationsSection::on_pointer_up(tk::Point local, bool inside_self)
{
    const bool was_pressed = pressed_;
    pressed_ = false;

    if (!inside_self || !was_pressed) { return; }
    if (!hit_row(local)) { return; }

    checked_ = !checked_;
    if (on_notifications_changed) { on_notifications_changed(checked_); }
}

void NotificationsSection::on_pointer_move(tk::Point local)
{
    hovered_ = hit_row(local);
}

void NotificationsSection::on_pointer_leave()
{
    hovered_ = false;
    pressed_ = false;
}

} // namespace tesseract::views
