#include "NotificationsSection.h"

#include "tesseract/settings.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

// Visual constants.
constexpr float kPadX = 24.0f;     // horizontal outer padding
constexpr float kPadY = 14.0f;     // vertical outer padding
constexpr float kBoxSize = 16.0f;  // checkbox square side length
constexpr float kBoxRadius = 3.0f; // checkbox corner radius
constexpr float kBoxBorder = 1.5f; // stroke width of unchecked outline
constexpr float kLabelGap = 10.0f; // gap between checkbox and label text
constexpr float kRowRadius = 6.0f; // hover background corner radius

// Approximate glyph height for Body role, used in measure().
constexpr float kGlyphH = 17.0f;

// Row height = max(checkbox, glyph) + 2 × vertical padding.
constexpr float kRowH = std::max(kBoxSize, kGlyphH) + kPadY * 2.0f;

constexpr const char* kLabelNotif = "Enable notifications on this device";
constexpr const char* kLabelPreviews =
    "Show image & sticker previews in notifications";

} // namespace

// ---------------------------------------------------------------------------

NotificationsSection::NotificationsSection()
    : checked_(tesseract::Settings::instance().notifications_enabled),
      previews_checked_(
          tesseract::Settings::instance().notification_image_previews)
{
}

void NotificationsSection::set_checked(bool enabled)
{
    checked_ = enabled;
}

void NotificationsSection::set_image_previews_checked(bool enabled)
{
    previews_checked_ = enabled;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size NotificationsSection::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    return {w, kRowH * 2.0f};
}

void NotificationsSection::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
    // Drop cached labels so they get rebuilt with the new available width.
    label_layout_.reset();
    previews_label_layout_.reset();
}

// ---------------------------------------------------------------------------
// Paint helpers
// ---------------------------------------------------------------------------

// Draw a simple checkmark inside `box`: an axis-aligned "L"-shaped tick
// built from filled rects so it renders identically on all four backends
// without line primitives.
void NotificationsSection::draw_checkmark(tk::Canvas& canvas, tk::Rect box,
                                          tk::Color ink) const
{
    const float margin = box.w * 0.18f;
    const float ox = box.x + margin;
    const float oy = box.y + margin;
    const float iw = box.w - margin * 2.0f;
    const float ih = box.h - margin * 2.0f;
    const float thick = std::max(2.0f, box.w * 0.13f);

    const float pivot_x = ox + iw * 0.38f;
    const float pivot_y = oy + ih * 0.62f;

    canvas.fill_rect(
        {ox, oy + ih * 0.30f, thick, pivot_y - (oy + ih * 0.30f) + thick}, ink);
    canvas.fill_rect({ox, pivot_y, pivot_x - ox + thick, thick}, ink);
    canvas.fill_rect({pivot_x, oy, thick, pivot_y - oy + thick}, ink);
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void NotificationsSection::paint_row(tk::PaintCtx& ctx, int row, float y,
                                     bool checked, const std::string& label,
                                     std::unique_ptr<tk::TextLayout>& cache)
{
    const auto& pal = ctx.theme.palette;
    const tk::Rect row_r{bounds_.x, y, bounds_.w, kRowH};

    // -------- Hover backdrop ------------------------------------------------
    if (hovered_row_ == row || pressed_row_ == row)
    {
        tk::Rect hover_r = row_r;
        hover_r.x += 4.0f;
        hover_r.y += 4.0f;
        hover_r.w -= 8.0f;
        hover_r.h -= 8.0f;
        const tk::Color bg =
            (pressed_row_ == row) ? pal.subtle_pressed : pal.subtle_hover;
        ctx.canvas.fill_rounded_rect(hover_r, kRowRadius, bg);
    }

    // -------- Checkbox ------------------------------------------------------
    const float box_y = row_r.y + (row_r.h - kBoxSize) * 0.5f;
    const tk::Rect box{row_r.x + kPadX, box_y, kBoxSize, kBoxSize};

    if (checked)
    {
        ctx.canvas.fill_rounded_rect(box, kBoxRadius, pal.accent);
        draw_checkmark(ctx.canvas, box, pal.text_on_accent);
    }
    else
    {
        ctx.canvas.fill_rounded_rect(box, kBoxRadius,
                                     tk::Color::rgba(0, 0, 0, 0));
        ctx.canvas.stroke_rounded_rect(box, kBoxRadius, pal.border, kBoxBorder);
    }

    // -------- Label ---------------------------------------------------------
    const float label_x = box.x + kBoxSize + kLabelGap;
    const float label_w =
        std::max(0.0f, bounds_.x + bounds_.w - kPadX - label_x);

    if (!cache)
    {
        tk::TextStyle st;
        st.role = tk::FontRole::Body;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = label_w;
        cache = ctx.factory.build_text(label, st);
    }

    if (cache)
    {
        const tk::Size sz = cache->measure();
        const float ty = row_r.y + (row_r.h - sz.h) * 0.5f;
        ctx.canvas.draw_text(*cache, {label_x, ty}, pal.text_primary);
    }
}

void NotificationsSection::paint(tk::PaintCtx& ctx)
{
    paint_row(ctx, 0, bounds_.y, checked_, tk::tr(kLabelNotif), label_layout_);
    paint_row(ctx, 1, bounds_.y + kRowH, previews_checked_,
              tk::tr(kLabelPreviews), previews_label_layout_);
}

// ---------------------------------------------------------------------------
// Pointer handling
// ---------------------------------------------------------------------------

int NotificationsSection::row_at(tk::Point local) const
{
    // Exclude horizontal outer padding; the whole row (incl. label) is the
    // hit target so the user can click the text too.
    if (local.x < kPadX || local.x >= bounds_.w - kPadX)
    {
        return -1;
    }
    if (local.y < 0 || local.y >= kRowH * 2.0f)
    {
        return -1;
    }
    return local.y < kRowH ? 0 : 1;
}

bool NotificationsSection::on_pointer_down(tk::Point local)
{
    const int row = row_at(local);
    if (row < 0)
    {
        return false;
    }
    pressed_row_ = row;
    return true;
}

void NotificationsSection::on_pointer_up(tk::Point local, bool inside_self)
{
    const int was_pressed = pressed_row_;
    pressed_row_ = -1;

    if (!inside_self || was_pressed < 0)
    {
        return;
    }
    if (row_at(local) != was_pressed)
    {
        return;
    }

    if (was_pressed == 0)
    {
        checked_ = !checked_;
        if (on_notifications_changed)
        {
            on_notifications_changed(checked_);
        }
    }
    else
    {
        previews_checked_ = !previews_checked_;
        if (on_image_previews_changed)
        {
            on_image_previews_changed(previews_checked_);
        }
    }
}

bool NotificationsSection::on_pointer_move(tk::Point local)
{
    int prev = hovered_row_;
    hovered_row_ = row_at(local);
    return hovered_row_ != prev;
}

void NotificationsSection::on_pointer_leave()
{
    hovered_row_ = -1;
    pressed_row_ = -1;
}

} // namespace tesseract::views
