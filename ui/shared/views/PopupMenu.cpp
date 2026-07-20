#include "views/PopupMenu.h"
#include "tk/canvas.h"
#include "tk/svg.h"
#include "tk/theme.h"
#include "views/media_utils.h"
#include <algorithm>
#include <cmath>

namespace tesseract::views
{

void PopupMenu::open(std::vector<Item> items, tk::Rect anchor_world)
{
    items_        = std::move(items);
    anchor_world_ = anchor_world;
    icon_cache_.clear();
    icon_cache_.resize(items_.size()); // null entries; rebuilt lazily in paint()
    open_         = true;
    hovered_index_  = -1;
    pressed_index_  = -1;
    press_backdrop_ = false;
    reveal_.reset(0.0f);
    reveal_.set_target(1.0f);
    if (on_layout_changed)
        on_layout_changed();
}

void PopupMenu::close()
{
    if (!open_)
        return;
    open_          = false;
    hovered_index_ = -1;
    pressed_index_ = -1;
    if (on_layout_changed)
        on_layout_changed();
}

tk::Size PopupMenu::measure(tk::LayoutCtx&, tk::Size)
{
    return {0.0f, 0.0f};
}

void PopupMenu::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    if (!open_ || items_.empty())
    {
        bounds_    = {};
        menu_rect_ = {};
        return;
    }
    bounds_ = bounds;

    const float popup_h = kRowHeight * float(items_.size());

    // Compute menu world position: right-align to anchor's right edge, open below.
    float x_world = anchor_world_.x + anchor_world_.w - kWidth;
    float y_world = anchor_world_.y + anchor_world_.h + 2.0f;

    // Flip above the anchor if clipped at the bottom.
    if (y_world + popup_h > bounds.y + bounds.h)
        y_world = anchor_world_.y - popup_h - 2.0f;

    // Clamp horizontally inside the parent.
    x_world = std::clamp(x_world, bounds.x, bounds.x + bounds.w - kWidth);

    // Store in LOCAL coords (relative to bounds_.origin) so pointer handlers —
    // which receive local-space points — can compare directly without conversion.
    menu_rect_ = {x_world - bounds.x, y_world - bounds.y, kWidth, popup_h};
}

void PopupMenu::paint(tk::PaintCtx& ctx)
{
    if (!open_ || items_.empty())
        return;

    const auto& pal = ctx.theme.palette;
    const int n = int(items_.size());

    // Invalidate rasterized icons when the DPI scale changes so they stay crisp.
    const float scale = ctx.canvas.scale_factor();
    if (scale != icon_scale_)
    {
        icon_scale_ = scale;
        for (auto& ic : icon_cache_)
            ic.reset();
    }
    constexpr float kMenuIconPx = 18.0f;

    // Offset local menu_rect to world coords for drawing (canvas is world-space).
    const float wx = bounds_.x + menu_rect_.x;
    const float wy = bounds_.y + menu_rect_.y;
    const tk::Rect card{wx, wy, menu_rect_.w, menu_rect_.h};

    constexpr float kRevealMs = 110.0f;
    const float reveal_t = reveal_.step(kRevealMs);
    const bool revealing = reveal_t < 1.0f;
    if (revealing)
    {
        if (reveal_.still_animating())
        {
            if (auto* h = host()) h->request_repaint();
        }
        ctx.canvas.push_opacity(reveal_t);
    }

    // Opaque background card.
    ctx.canvas.fill_rect(card, pal.bg);

    tk::TextStyle glyph_st{};
    glyph_st.role = tk::FontRole::Title; // matches action-pill glyph style

    tk::TextStyle label_st{};
    label_st.role   = tk::FontRole::Body;
    label_st.halign = tk::TextHAlign::Leading;
    label_st.valign = tk::TextVAlign::Top;

    for (int i = 0; i < n; ++i)
    {
        // Convert local item rect to world for drawing.
        const tk::Rect local_row = item_rect(i);
        const tk::Rect row{bounds_.x + local_row.x, bounds_.y + local_row.y,
                           local_row.w, local_row.h};

        if (i == hovered_index_)
            ctx.canvas.fill_rect(row, pal.subtle_hover);

        const tk::Color text_col =
            items_[std::size_t(i)].destructive ? pal.destructive
                                               : pal.text_primary;

        // Icon (left-aligned, vertically centred). Prefer an SVG icon tinted to
        // the row colour; fall back to the Unicode glyph.
        const auto& item = items_[std::size_t(i)];
        float label_x = row.x + kTextXNoIcon;
        if (!item.svg_icon.empty())
        {
            auto& cached = icon_cache_[std::size_t(i)];
            if (!cached)
                cached = tk::rasterize_svg(
                    ctx.factory, item.svg_icon,
                    std::max(1, int(std::lround(kMenuIconPx * scale))),
                    text_col);
            if (cached)
                ctx.canvas.draw_image(
                    *cached,
                    {row.x + kGlyphX, row.y + (row.h - kMenuIconPx) * 0.5f,
                     kMenuIconPx, kMenuIconPx});
            label_x = row.x + kTextX;
        }
        else if (!item.glyph.empty())
        {
            auto gl = ctx.factory.build_text(item.glyph, glyph_st);
            if (gl)
            {
                float gy = row.y + (row.h - gl->ascent()) * 0.5f;
                ctx.canvas.draw_text(*gl, {row.x + kGlyphX, gy},
                                     pal.text_secondary);
            }
            label_x = row.x + kTextX;
        }

        // Label text (vertically centred).
        auto ll = ctx.factory.build_text(item.label, label_st);
        if (ll)
        {
            tk::Size sz = ll->measure();
            float ly    = row.y + (row.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*ll, {label_x, ly}, text_col);
        }

        // Row separator (except after last row).
        if (i < n - 1)
        {
            ctx.canvas.fill_rect(
                {row.x, row.y + row.h - 1.0f, row.w, 1.0f}, pal.separator);
        }
    }

    // 1px border around the card.
    ctx.canvas.fill_rect({wx, wy, card.w, 1.0f}, pal.popup_border);
    ctx.canvas.fill_rect({wx, wy + card.h - 1.0f, card.w, 1.0f}, pal.popup_border);
    ctx.canvas.fill_rect({wx, wy, 1.0f, card.h}, pal.popup_border);
    ctx.canvas.fill_rect({wx + card.w - 1.0f, wy, 1.0f, card.h}, pal.popup_border);

    if (revealing)
    {
        ctx.canvas.pop_opacity();
    }
}

bool PopupMenu::on_pointer_down(tk::Point local)
{
    if (!open_)
        return false;
    int r = row_at(local);
    if (r >= 0)
    {
        pressed_index_  = r;
        press_backdrop_ = false;
        return true;
    }
    // Click outside the menu card — backdrop. Consume to prevent pass-through.
    pressed_index_  = -1;
    press_backdrop_ = true;
    return true;
}

void PopupMenu::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!open_)
        return;
    if (press_backdrop_)
    {
        press_backdrop_ = false;
        pressed_index_  = -1;
        if (on_dismissed)
            on_dismissed();
        return;
    }
    if (pressed_index_ >= 0)
    {
        int was = pressed_index_;
        pressed_index_ = -1;
        if (!inside_self)
            return;
        int r = row_at(local);
        if (r == was && r < int(items_.size()))
        {
            auto cb = items_[std::size_t(r)].on_selected;
            if (on_dismissed)
                on_dismissed();
            if (cb)
                cb();
        }
    }
}

bool PopupMenu::on_pointer_move(tk::Point local)
{
    if (!open_)
        return false;
    int prev       = hovered_index_;
    hovered_index_ = row_at(local);
    return hovered_index_ != prev;
}

void PopupMenu::on_pointer_leave()
{
    hovered_index_ = -1;
}

int PopupMenu::row_at(tk::Point local) const
{
    if (!open_ || menu_rect_.w <= 0)
        return -1;
    const int n = int(items_.size());
    for (int i = 0; i < n; ++i)
    {
        if (rect_contains(item_rect(i), local))
            return i;
    }
    return -1;
}

} // namespace tesseract::views
