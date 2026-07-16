#include "TabbedGridPicker.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kPadding = 8.0f;
constexpr float kSearchHeight = 32.0f;
} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Grid adapter — forwards count + cell paint to the picker subclass.
// ─────────────────────────────────────────────────────────────────────────

class TabbedGridPicker::GridAdapter : public tk::GridAdapter
{
public:
    explicit GridAdapter(TabbedGridPicker& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        return owner_.item_count();
    }

    void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool selected, bool hovered) override
    {
        owner_.paint_cell(index, ctx, bounds, selected, hovered);
    }

private:
    TabbedGridPicker& owner_;
};

// ─────────────────────────────────────────────────────────────────────────
//  TabbedGridPicker
// ─────────────────────────────────────────────────────────────────────────

TabbedGridPicker::~TabbedGridPicker() = default;

TabbedGridPicker::TabbedGridPicker()
    : grid_adapter_(std::make_unique<GridAdapter>(*this))
{
    if (host())
    {
        auto search = tk::create_widget<tk::TextField>(this, kSearchHeight);
        search->set_on_changed([this](const std::string& q)
                               { set_search_query(q); });
        search_field_ = add_child(std::move(search));
    }

    auto grid = tk::create_widget<tk::GridView>(this);
    grid->set_adapter(grid_adapter_.get());
    grid->on_cell_clicked = [this](int idx)
    {
        if (idx < 0)
        {
            return;
        }
        on_item_activated(idx);
    };
    grid_ = add_child(std::move(grid));
    // Cell/spacing/padding are applied in the first arrange(); virtuals can't
    // be called from the constructor.
}

void TabbedGridPicker::set_image_provider(ImageProvider p)
{
    provider_ = std::move(p);
    invalidate_image_cache();
}

void TabbedGridPicker::invalidate_image_cache()
{
    if (grid_)
    {
        grid_->invalidate_data();
    }
}

void TabbedGridPicker::refresh_grid()
{
    if (grid_)
    {
        grid_->set_selected_index(-1);
        grid_->invalidate_data();
        grid_->scroll_to_top();
    }
    // Tab clicks get a free repaint from the host's own pointer-dispatch
    // machinery; a search-query change originates from the native field's
    // own on_changed callback instead, which the host never sees, so it
    // has to be requested explicitly here (self-drive, same idiom as
    // EncryptionSetupOverlay's spinner). Harmless to call on every
    // refresh_grid() — the host coalesces repeat requests.
    if (host())
    {
        host()->request_repaint();
    }
}

void TabbedGridPicker::set_search_query(std::string query)
{
    query_ = std::move(query);
    on_search_query_changed(query_, query_.empty());
}

void TabbedGridPicker::set_search_placeholder(std::string text)
{
    if (search_field_)
        search_field_->set_placeholder(std::move(text));
}

void TabbedGridPicker::set_search_overlay_inset(float inset)
{
    if (search_field_)
        search_field_->set_overlay_inset(inset);
}

// ─────────────────────────────────────────────────────────────────────────
//  Layout
// ─────────────────────────────────────────────────────────────────────────

tk::Size TabbedGridPicker::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void TabbedGridPicker::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    const float th = tab_height();

    search_rect_ = {bounds.x + kPadding, bounds.y + kPadding,
                    std::max(0.0f, bounds.w - kPadding * 2), kSearchHeight};
    if (search_field_)
        search_field_->arrange(ctx, search_rect_);

    tab_rect_ = {bounds.x, bounds.y + bounds.h - th, bounds.w, th};

    grid_rect_ = {
        bounds.x, bounds.y + kPadding * 2 + kSearchHeight, bounds.w,
        std::max(0.0f, bounds.h - kPadding * 2 - kSearchHeight - th)};
    if (grid_)
    {
        const float cs = cell_size();
        grid_->set_cell_size(cs, cs);
        grid_->set_spacing(cell_gap(), cell_gap());
        grid_->set_padding(tk::Edges::all(grid_padding()));
        grid_->arrange(ctx, grid_rect_);
    }
}

void TabbedGridPicker::on_theme_changed(const tk::Theme& t)
{
    if (search_field_)
        search_field_->set_text_color(t.palette.text_primary);
}

void TabbedGridPicker::paint(tk::PaintCtx& ctx)
{
    // Backdrop.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    // Search-row affordance behind the host's NativeTextField overlay.
    ctx.canvas.fill_rounded_rect(search_rect_, 6.0f,
                                 ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(search_rect_, 6.0f, ctx.theme.palette.border,
                                   1.0f);

    if (grid_)
    {
        grid_->paint(ctx);
    }

    // Shortcode tooltip: shown when a grid cell is hovered. Re-evaluated every
    // frame (this widget doesn't get a hover-transition event of its own —
    // GridView tracks hovered_index_ itself), rendered by Host above
    // everything so it escapes this grid's own clip.
    if (host())
    {
        std::string sc;
        if (grid_ && grid_->hovered_index() >= 0)
            sc = cell_tooltip(grid_->hovered_index());
        if (!sc.empty())
            host()->show_tooltip(grid_, sc, grid_->rect_at(grid_->hovered_index()));
        else
            host()->hide_tooltip(grid_);
    }

    // ─── Tab strip ──────────────────────────────────────────────────────
    ctx.canvas.fill_rect(tab_rect_, ctx.theme.palette.chrome_bg);
    ctx.canvas.fill_rect({tab_rect_.x, tab_rect_.y, tab_rect_.w, 1.0f},
                         ctx.theme.palette.separator);

    int total = tab_count();
    if (total <= 0)
    {
        // Outer border still drawn so the card edge reads.
        ctx.canvas.stroke_rect(bounds_, ctx.theme.palette.popup_border, 1.0f);
        return;
    }
    float tab_w =
        std::max(tab_slot_min(), tab_rect_.w / static_cast<float>(total));
    int active = active_tab_index();
    ctx.canvas.push_clip_rect(tab_rect_);
    for (int i = 0; i < total; ++i)
    {
        tk::Rect tab{tab_rect_.x + i * tab_w - tab_scroll_offset_, tab_rect_.y,
                     tab_w, tab_rect_.h};
        if (i == active)
        {
            ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_pressed);
            tk::Rect underline{tab.x, tab.y + tab.h - 2.0f, tab.w, 2.0f};
            ctx.canvas.fill_rect(underline, ctx.theme.palette.accent);
        }
        else if (i == hovered_tab_idx_)
        {
            ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_hover);
        }

        paint_tab_content(i, ctx, tab);
    }
    ctx.canvas.pop_clip();
    // Outer border drawn last so nothing (grid fill, tab strip) paints over it.
    ctx.canvas.stroke_rect(bounds_, ctx.theme.palette.popup_border, 1.0f);
}

// ─────────────────────────────────────────────────────────────────────────
//  Input
// ─────────────────────────────────────────────────────────────────────────

int TabbedGridPicker::tab_at(tk::Point local) const
{
    float lx = local.x - (tab_rect_.x - bounds_.x);
    float ly = local.y - (tab_rect_.y - bounds_.y);
    if (lx < 0 || ly < 0 || lx >= tab_rect_.w || ly >= tab_rect_.h)
    {
        return -1;
    }
    int total = tab_count();
    if (total == 0)
    {
        return -1;
    }
    float tab_w =
        std::max(tab_slot_min(), tab_rect_.w / static_cast<float>(total));
    int idx = static_cast<int>((lx + tab_scroll_offset_) / tab_w);
    if (idx < 0 || idx >= total)
    {
        return -1;
    }
    return idx;
}

bool TabbedGridPicker::on_pointer_down(tk::Point local)
{
    int t = tab_at(local);
    if (t >= 0)
    {
        pressed_tab_idx_ = t;
        return true;
    }
    return false;
}

void TabbedGridPicker::on_pointer_up(tk::Point local, bool inside_self)
{
    if (pressed_tab_idx_ < 0)
    {
        return;
    }
    int t = inside_self ? tab_at(local) : -1;
    int hit = pressed_tab_idx_;
    pressed_tab_idx_ = -1;
    if (t != hit)
    {
        return;
    }
    on_tab_clicked(hit);
}

bool TabbedGridPicker::on_wheel(tk::Point local, float dx, float dy)
{
    // Only handle wheel events that land in the tab strip.
    float lx = local.x - (tab_rect_.x - bounds_.x);
    float ly = local.y - (tab_rect_.y - bounds_.y);
    if (lx < 0 || ly < 0 || lx >= tab_rect_.w || ly >= tab_rect_.h)
    {
        return false;
    }

    int total = tab_count();
    if (total == 0)
    {
        return false;
    }
    float tab_w =
        std::max(tab_slot_min(), tab_rect_.w / static_cast<float>(total));
    float total_content_w = tab_w * static_cast<float>(total);
    float max_offset = std::max(0.0f, total_content_w - tab_rect_.w);
    if (max_offset == 0.0f)
    {
        return false; // all tabs already visible
    }

    // Use horizontal delta when available; fall back to vertical so a plain
    // mouse wheel still scrolls the tab strip left/right.
    float delta = (dx != 0.0f) ? dx : dy;
    tab_scroll_offset_ += delta;
    if (tab_scroll_offset_ < 0.0f)
    {
        tab_scroll_offset_ = 0.0f;
    }
    if (tab_scroll_offset_ > max_offset)
    {
        tab_scroll_offset_ = max_offset;
    }
    return true; // host repaints on true
}

} // namespace tesseract::views
