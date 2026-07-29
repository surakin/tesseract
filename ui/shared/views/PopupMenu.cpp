#include "views/PopupMenu.h"
#include "tk/canvas.h"
#include "tk/svg.h"
#include "tk/theme.h"
#include "views/media_utils.h"
#include <algorithm>
#include <cmath>

namespace tesseract::views
{

// ─────────────────────────────────────────────────────────────────────────
//  PopupMenu::MenuList — the popup surface's root widget. Owns row painting
// + hit-testing/hover/press; mirrors tk::ComboBox::DropdownList. There is no
// "backdrop" concept here: the native popup window's client area *is* the
// menu card, pixel for pixel, so every point inside this widget is a real
// row (or separator) — outside clicks never reach it at all (the OS routes
// them to whatever window they actually landed on).
// ─────────────────────────────────────────────────────────────────────────

class PopupMenu::MenuList : public tk::Widget
{
public:
    // Public, plain-constructible (like ComboBox::DropdownList) — always a
    // popup surface's root widget, mounted via PopupSurfaceHandle::set_root().
    MenuList() = default;

    void set_items(const std::vector<Item>& items)
    {
        items_ = items;
        icon_cache_.clear();
        icon_cache_.resize(items_.size()); // null entries; rebuilt lazily in paint()
        hovered_index_ = -1;
        pressed_index_ = -1;
    }

    std::function<void(std::size_t index)> on_row_activated;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        float h = 0.0f;
        for (int i = 0; i < int(items_.size()); ++i)
            h += row_height_(i);
        return {constraints.w > 0 ? constraints.w : kWidth, h};
    }

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        bounds_ = bounds;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        if (items_.empty())
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

        const tk::Rect card = bounds_;

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
            const auto& item = items_[std::size_t(i)];
            const tk::Rect row = item_rect_(i);

            if (item.is_separator)
            {
                // Thin centered rule; no hover, no label/icon, and no additional
                // per-row divider below (this row already draws the visual break
                // — see the "i < n - 1" check at the end of the loop).
                const float ry = row.y + row.h * 0.5f;
                ctx.canvas.fill_rect({row.x, ry, row.w, 1.0f}, pal.separator);
                continue;
            }

            if (i == hovered_index_)
                ctx.canvas.fill_rect(row, pal.subtle_hover);

            const tk::Color text_col =
                !item.enabled       ? pal.text_secondary // dimmed
                : item.destructive  ? pal.destructive
                                     : pal.text_primary;

            // Icon (left-aligned, vertically centred). Prefer an SVG icon tinted to
            // the row colour; fall back to the Unicode glyph.
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

            // Row separator (except after last row, or when the next row is
            // itself a dedicated separator item — that row already draws its
            // own rule, so this would otherwise double up).
            if (i < n - 1 && !items_[std::size_t(i + 1)].is_separator)
            {
                ctx.canvas.fill_rect(
                    {row.x, row.y + row.h - 1.0f, row.w, 1.0f}, pal.separator);
            }
        }

        // 1px border around the card.
        ctx.canvas.fill_rect({card.x, card.y, card.w, 1.0f}, pal.popup_border);
        ctx.canvas.fill_rect({card.x, card.y + card.h - 1.0f, card.w, 1.0f}, pal.popup_border);
        ctx.canvas.fill_rect({card.x, card.y, 1.0f, card.h}, pal.popup_border);
        ctx.canvas.fill_rect({card.x + card.w - 1.0f, card.y, 1.0f, card.h}, pal.popup_border);
    }

    bool on_pointer_down(tk::Point local) override
    {
        int r = row_at_(local);
        pressed_index_ = r;
        return true; // this widget's bounds ARE the whole menu card
    }

    void on_pointer_up(tk::Point local, bool inside_self) override
    {
        int was = pressed_index_;
        pressed_index_ = -1;
        if (!inside_self)
            return;
        int r = row_at_(local);
        if (r == was && r >= 0 && r < int(items_.size()))
        {
            const auto& item = items_[std::size_t(r)];
            // A separator or disabled item: released inside the menu, so
            // don't act on it — matches native disabled-menu-item behavior
            // (menu stays open, click is absorbed).
            if (item.is_separator || !item.enabled)
                return;
            if (on_row_activated)
                on_row_activated(std::size_t(r));
        }
    }

    bool on_pointer_move(tk::Point local) override
    {
        int prev = hovered_index_;
        int r    = row_at_(local);
        // Separators and disabled items never show a hover highlight.
        hovered_index_ =
            (r >= 0 && !items_[std::size_t(r)].is_separator && items_[std::size_t(r)].enabled)
                ? r
                : -1;
        return hovered_index_ != prev;
    }

    void on_pointer_leave() override
    {
        hovered_index_ = -1;
    }

private:
    float row_height_(int i) const
    {
        return items_[std::size_t(i)].is_separator ? kSeparatorHeight : kRowHeight;
    }

    tk::Rect item_rect_(int i) const
    {
        float y = bounds_.y;
        for (int j = 0; j < i; ++j)
            y += row_height_(j);
        return {bounds_.x, y, bounds_.w, row_height_(i)};
    }

    int row_at_(tk::Point local) const
    {
        const int n = int(items_.size());
        for (int i = 0; i < n; ++i)
        {
            if (rect_contains(item_rect_(i), local))
                return i;
        }
        return -1;
    }

    std::vector<Item> items_;
    int  hovered_index_  = -1;
    int  pressed_index_  = -1;

    // Per-item rasterized SVG icons (Item::svg_icon), tinted to the row colour.
    // Rebuilt when items change (set_items) or the canvas DPI scale changes.
    std::vector<std::unique_ptr<tk::Image>> icon_cache_;
    float icon_scale_ = -1.0f;
};

// ─────────────────────────────────────────────────────────────────────────
//  PopupMenu
// ─────────────────────────────────────────────────────────────────────────

void PopupMenu::open(std::vector<Item> items, tk::Rect anchor_world)
{
    items_        = std::move(items);
    anchor_world_ = anchor_world;
    open_         = true;

    if (!popup_surface_)
    {
        if (auto* h = host())
        {
            popup_surface_ = h->make_popup_surface();
            if (popup_surface_)
            {
                auto list = std::make_unique<MenuList>();
                list_ = list.get();
                list_->on_row_activated = [this](std::size_t idx)
                {
                    if (idx >= items_.size())
                        return;
                    auto cb = items_[idx].on_selected; // copy before firing on_dismissed
                    if (on_dismissed)
                        on_dismissed(); // owner's handler typically calls close()
                    if (cb)
                        cb();
                };
                popup_surface_->set_root(std::move(list));
            }
        }
    }

    if (list_)
        list_->set_items(items_);
    reposition_();
    if (popup_surface_)
        popup_surface_->set_visible(true);

    if (on_layout_changed)
        on_layout_changed();
}

void PopupMenu::close()
{
    if (!open_)
        return;
    open_ = false;
    if (popup_surface_)
        popup_surface_->set_visible(false);
    if (on_layout_changed)
        on_layout_changed();
}

tk::Size PopupMenu::measure(tk::LayoutCtx&, tk::Size)
{
    return {0.0f, 0.0f};
}

void PopupMenu::arrange(tk::LayoutCtx&, tk::Rect /*bounds*/)
{
    reposition_();
}

void PopupMenu::reposition_()
{
    if (!open_ || items_.empty() || !popup_surface_)
        return;

    float popup_h = 0.0f;
    for (int i = 0; i < int(items_.size()); ++i)
        popup_h += row_height(i);

    // PopupSurfaceHandle::set_rect() aligns the popup's LEFT edge straight to
    // anchor_world_rect.x (no built-in right-align mode) — pre-adjust the
    // anchor's x so the result still right-aligns to the ORIGINAL anchor's
    // right edge, matching this menu's established on-screen position.
    tk::Rect adjusted = anchor_world_;
    adjusted.x = anchor_world_.x + anchor_world_.w - kWidth;
    adjusted.w = 0.0f; // set_rect ignores width for x; only .x/.h matter now

    popup_surface_->set_rect(adjusted, {kWidth, popup_h},
                             tk::PopupPlacement::PreferBelow);
}

void PopupMenu::paint(tk::PaintCtx& ctx)
{
    // Never draws here — drawing happens inside the native popup surface's
    // own separate paint pass (see MenuList::paint()). Just register as the
    // active popup (click-anywhere-dismiss input priority) each frame it's
    // open.
    if (open_ && ctx.host)
        ctx.host->register_popup(this);
}

void PopupMenu::on_theme_changed(const tk::Theme& t)
{
    if (popup_surface_)
        popup_surface_->set_theme(t);
}

void PopupMenu::on_popup_dismiss()
{
    if (!open_)
        return;
    if (on_dismissed)
        on_dismissed();
    else
        close(); // no owner handler wired — fall back to closing directly
}

float PopupMenu::row_height(int i) const
{
    return items_[std::size_t(i)].is_separator ? kSeparatorHeight : kRowHeight;
}

} // namespace tesseract::views
