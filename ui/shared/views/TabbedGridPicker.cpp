#include "TabbedGridPicker.h"

#include "tk/access_tree.h"
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

class TabbedGridPicker::GridAdapter : public tk::GridAdapter,
                                      public tk::GridAdapterAccessibility
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

    // ── tk::GridAdapterAccessibility ─────────────────────────────────────
    // Shared here (rather than in EmojiPicker/StickerPicker individually)
    // since both subclasses already implement cell_tooltip() with exactly
    // the right content — a shortcode — for an accessible name; this is
    // the one place that needs to know that.

    tk::Role access_role_for_cell(std::size_t index) const override
    {
        return owner_.cell_tooltip(static_cast<int>(index)).empty() ? tk::Role::None
                                                                    : tk::Role::GridCell;
    }
    std::string access_name_for_cell(std::size_t index) const override
    {
        // cell_tooltip() returns ":shortcode:" — colon-wrapped chat-typing
        // syntax, correct for the sighted hover tooltip (its only other
        // caller — see paint()'s tooltip dispatch) but not for a screen
        // reader, which would otherwise literally announce "colon thumbs
        // underscore up colon". Strip the wrapping colons for the
        // accessible name only; cell_tooltip() itself is untouched.
        std::string name = owner_.cell_tooltip(static_cast<int>(index));
        if (name.size() >= 2 && name.front() == ':' && name.back() == ':')
            name = name.substr(1, name.size() - 2);
        return name;
    }
    // Mirrors on_cell_clicked's identical dispatch, so the mouse and
    // AT-client activation paths can't drift apart.
    bool access_activate_cell(std::size_t index) override
    {
        if (index >= owner_.item_count())
            return false;
        owner_.on_item_activated(static_cast<int>(index));
        return true;
    }

private:
    TabbedGridPicker& owner_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Tab strip — the bottom category bar. Like the grid, the individual tabs
//  are paint-time data (tab_count()/paint_tab_content()), not child
//  widgets — but the strip AS A WHOLE needs to be a real child widget so it
//  can be its own Tab-traversal stop, independent of the search field and
//  the grid. Mirrors tk::TabView/tk::TabBar/tk::SideTabView's "one
//  focusable container, internal index" shape (see their own doc
//  comments), but with GridView's two-step activation model instead of
//  TabView's immediate-commit one: Left/Right move a keyboard cursor
//  (nav_index_) without switching category, and only Enter/Space commits
//  it via owner_.on_tab_clicked() — arrowing straight through every
//  category would otherwise re-render the grid's content on every
//  keypress.
// ─────────────────────────────────────────────────────────────────────────

class TabbedGridPicker::TabStrip : public tk::Widget,
                                   public tk::WidgetRowAccessibility
{
protected:
    explicit TabStrip(TabbedGridPicker& owner) : owner_(owner)
    {
    }
    TK_WIDGET_FACTORY_FRIEND(TabStrip)

public:
    // Index of the tab at `local` (widget-local), or -1.
    int tab_at(tk::Point local) const
    {
        if (local.x < 0 || local.y < 0 || local.x >= bounds_.w ||
            local.y >= bounds_.h)
        {
            return -1;
        }
        int total = owner_.tab_count();
        if (total == 0)
        {
            return -1;
        }
        float tab_w = std::max(owner_.tab_slot_min(),
                               bounds_.w / static_cast<float>(total));
        int idx = static_cast<int>((local.x + scroll_offset_) / tab_w);
        if (idx < 0 || idx >= total)
        {
            return -1;
        }
        return idx;
    }

    // Clears press/hover/scroll/keyboard-cursor state — called from
    // TabbedGridPicker::open_at()/set_visible(false)/on_popup_dismiss(),
    // mirroring the transient state those already reset for the rest of
    // the picker.
    void reset_interaction()
    {
        pressed_idx_ = -1;
        hovered_idx_ = -1;
        scroll_offset_ = 0.0f;
        nav_index_ = -1;
    }
    // Narrower reset for a full pack/category reload — see
    // TabbedGridPicker::reset_tab_scroll()'s own doc comment.
    void reset_scroll()
    {
        scroll_offset_ = 0.0f;
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return constraints;
    }
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        bounds_ = bounds;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        ctx.canvas.fill_rect(bounds_, ctx.theme.palette.chrome_bg);
        ctx.canvas.fill_rect({bounds_.x, bounds_.y, bounds_.w, 1.0f},
                             ctx.theme.palette.separator);

        int total = owner_.tab_count();
        if (total <= 0)
        {
            return;
        }
        float tab_w = std::max(owner_.tab_slot_min(),
                               bounds_.w / static_cast<float>(total));
        int active = owner_.active_tab_index();
        ctx.canvas.push_clip_rect(bounds_);
        for (int i = 0; i < total; ++i)
        {
            tk::Rect tab{bounds_.x + i * tab_w - scroll_offset_, bounds_.y,
                        tab_w, bounds_.h};
            if (i == active)
            {
                ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_pressed);
                tk::Rect underline{tab.x, tab.y + tab.h - 2.0f, tab.w, 2.0f};
                ctx.canvas.fill_rect(underline, ctx.theme.palette.accent);
            }
            else if (i == hovered_idx_)
            {
                ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_hover);
            }
            if (i == nav_index_ && i != active)
            {
                // Keyboard cursor: distinct from hover/active so Enter's
                // target is unambiguous even while the pointer hovers a
                // different tab. Skipped when it coincides with the active
                // tab — the underline above already marks that one, and
                // paint_own_focus_ring() below still rings it either way.
                ctx.canvas.stroke_rect(tab, ctx.theme.palette.border_strong,
                                       1.0f);
            }

            owner_.paint_tab_content(i, ctx, tab);
        }
        ctx.canvas.pop_clip();
    }

    bool on_pointer_down(tk::Point local) override
    {
        int t = tab_at(local);
        if (t >= 0)
        {
            pressed_idx_ = t;
            return true;
        }
        return false;
    }
    void on_pointer_up(tk::Point local, bool inside_self) override
    {
        if (pressed_idx_ < 0)
        {
            return;
        }
        int t = inside_self ? tab_at(local) : -1;
        int hit = pressed_idx_;
        pressed_idx_ = -1;
        if (t != hit)
        {
            return;
        }
        owner_.on_tab_clicked(hit);
    }
    bool on_wheel(tk::Point local, float dx, float dy, bool /*is_touchpad*/) override
    {
        if (local.x < 0 || local.y < 0 || local.x >= bounds_.w ||
            local.y >= bounds_.h)
        {
            return false;
        }
        int total = owner_.tab_count();
        if (total == 0)
        {
            return false;
        }
        float tab_w = std::max(owner_.tab_slot_min(),
                               bounds_.w / static_cast<float>(total));
        float total_content_w = tab_w * static_cast<float>(total);
        float max_offset = std::max(0.0f, total_content_w - bounds_.w);
        if (max_offset == 0.0f)
        {
            return false; // all tabs already visible
        }
        float delta = (dx != 0.0f) ? dx : dy;
        scroll_offset_ += delta;
        if (scroll_offset_ < 0.0f)
        {
            scroll_offset_ = 0.0f;
        }
        if (scroll_offset_ > max_offset)
        {
            scroll_offset_ = max_offset;
        }
        return true; // host repaints on true
    }

    // A single tab has nothing to Tab-stop for — mirrors
    // tk::TabView::focusable()/tk::TabBar's identical convention.
    bool focusable() const override
    {
        return owner_.tab_count() > 1;
    }

    // Seeds the keyboard cursor from whatever's currently shown, so the
    // first Left/Right moves relative to it rather than from a stale
    // position. Cleared on blur so a later re-focus always re-syncs
    // instead of resuming wherever it was left.
    void on_focus_gained() override
    {
        int active = owner_.active_tab_index();
        int total = owner_.tab_count();
        nav_index_ = (active >= 0 && active < total) ? active
                    : (total > 0 ? 0 : -1);
    }
    void on_focus_lost() override
    {
        nav_index_ = -1;
    }

    // Gated on has_focus() first — mandatory: without it this is reachable
    // not only as the genuinely tk-focused widget but also via Host::
    // dispatch_key_down's root-wide broadcast fallback (fired whenever the
    // actually-focused widget doesn't consume a key), and an unfocused
    // strip elsewhere in the tree would otherwise react to a stray
    // Left/Right/Enter (mirrors tk::GridView::on_key_down's and
    // tk::TabView::on_key_down's identical comment).
    bool on_key_down(const tk::KeyEvent& e) override
    {
        if (!has_focus())
        {
            return false;
        }
        int total = owner_.tab_count();
        if (total == 0)
        {
            return false;
        }
        if (e.key == tk::Key::Left || e.key == tk::Key::Right)
        {
            if (e.ctrl || e.alt || e.meta)
            {
                return false;
            }
            int delta = (e.key == tk::Key::Right) ? 1 : -1;
            int base = (nav_index_ < 0) ? 0 : nav_index_;
            nav_index_ = (base + delta + total) % total;
            return true;
        }
        if (e.key == tk::Key::Enter || e.key == tk::Key::Space)
        {
            if (nav_index_ < 0 || nav_index_ >= total)
            {
                return false;
            }
            owner_.on_tab_clicked(nav_index_);
            return true;
        }
        return false;
    }

    // Rings just the keyboard-cursor tab, not the whole strip: the active
    // tab already has its own permanent underline/fill (see paint()
    // above), and a whole-strip ring would give no clue which of several
    // tabs Enter would actually pick — mirrors tk::TabView::
    // paint_own_focus_ring's "ring a specific rect, not bounds()"
    // technique, just scoped to one tab instead of the whole span since
    // here the cursor and the active tab are different things. Falls back
    // to the default whole-bounds ring when there's no cursor position.
    void paint_own_focus_ring(tk::PaintCtx& ctx) override
    {
        int total = owner_.tab_count();
        if (nav_index_ < 0 || nav_index_ >= total)
        {
            tk::paint_focus_ring(ctx, bounds_);
            return;
        }
        float tab_w = std::max(owner_.tab_slot_min(),
                               bounds_.w / static_cast<float>(total));
        tk::Rect tab{bounds_.x + nav_index_ * tab_w - scroll_offset_,
                    bounds_.y, tab_w, bounds_.h};
        tk::paint_focus_ring(ctx, tab);
    }

    // Container role; the tabs map through WidgetRowAccessibility below
    // (paint-time data, not tk::Widget children) — mirrors
    // tk::TabView's identical split.
    tk::Role access_role() const override
    {
        return tk::Role::TabList;
    }

    // ── tk::WidgetRowAccessibility ────────────────────────────────────────
    std::size_t access_row_count() const override
    {
        return static_cast<std::size_t>(std::max(0, owner_.tab_count()));
    }
    tk::Role access_role_for_widget_row(std::size_t) const override
    {
        return tk::Role::Tab;
    }
    std::string access_name_for_widget_row(std::size_t index) const override
    {
        return owner_.tab_label(static_cast<int>(index));
    }
    tk::AccessState access_state_for_widget_row(std::size_t index) const override
    {
        tk::AccessState s;
        s.selected = static_cast<int>(index) == owner_.active_tab_index();
        return s;
    }
    bool access_activate_widget_row(std::size_t index) override
    {
        int total = owner_.tab_count();
        if (total <= 0 || index >= static_cast<std::size_t>(total))
        {
            return false;
        }
        owner_.on_tab_clicked(static_cast<int>(index));
        return true;
    }
    tk::Rect access_rect_for_widget_row(std::size_t index) const override
    {
        int total = owner_.tab_count();
        if (total <= 0 || index >= static_cast<std::size_t>(total))
        {
            return {};
        }
        float tab_w = std::max(owner_.tab_slot_min(),
                               bounds_.w / static_cast<float>(total));
        return {bounds_.x + static_cast<float>(index) * tab_w - scroll_offset_,
                bounds_.y, tab_w, bounds_.h};
    }

private:
    TabbedGridPicker& owner_;
    int pressed_idx_ = -1;
    int hovered_idx_ = -1;
    int nav_index_ = -1;
    float scroll_offset_ = 0.0f;
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

    auto strip = tk::create_widget<TabStrip>(this, *this);
    tab_strip_ = add_child(std::move(strip));

    // Hidden until the owning RoomView's first show_*_picker_() call — see
    // set_visible()'s doc comment.
    set_visible(false);
}

void TabbedGridPicker::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (search_field_)
        search_field_->set_visible(v);

    if (!v)
    {
        // The shortcode tooltip is (re)issued and withdrawn only from
        // paint(), which stops running once hidden — so a cell hovered at
        // dismiss time (e.g. the one just picked, pointer never moved) would
        // keep its tooltip on screen. Clear the hover + any live tooltip now.
        if (tab_strip_)
            tab_strip_->reset_interaction();
        if (grid_)
        {
            grid_->on_pointer_leave();
            if (host())
                host()->hide_tooltip(grid_);
        }
    }
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

void TabbedGridPicker::reset_tab_scroll()
{
    if (tab_strip_)
        tab_strip_->reset_scroll();
}

void TabbedGridPicker::open_at(tk::Rect world_rect)
{
    bounds_ = world_rect;
    needs_arrange_ = true;

    if (tab_strip_)
        tab_strip_->reset_interaction();
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
    if (tab_strip_)
        tab_strip_->arrange(ctx, tab_rect_);

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
    // search_field_ sits on search_rect_'s fill — see paint() below and
    // Widget::background_color()'s doc comment.
    set_background_color(t.palette.chrome_bg);
    if (search_field_)
        search_field_->set_text_color(t.palette.text_primary);
}

void TabbedGridPicker::paint(tk::PaintCtx& ctx)
{
    // Backdrop.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    // Search-row affordance behind the search field.
    ctx.canvas.fill_rounded_rect(search_rect_, 6.0f,
                                 ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(search_rect_, 6.0f,
                                   ctx.theme.palette.border_strong, 1.0f);
    if (search_field_ && search_field_->visible())
        search_field_->paint(ctx);

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
            // from_popup=true — this picker is itself the currently
            // registered popup (see Host::show_tooltip's doc comment), so
            // its own tooltip must bypass the "no tooltip while a popup is
            // open" gate that suppresses unrelated background content.
            host()->show_tooltip(grid_, sc, grid_->rect_at(grid_->hovered_index()),
                                 /*from_popup=*/true);
        else
            host()->hide_tooltip(grid_);
    }

    // ─── Tab strip ──────────────────────────────────────────────────────
    if (tab_strip_)
        tab_strip_->paint(ctx);

    // Outer border drawn last so nothing (grid fill, tab strip) paints over
    // it. Unconditional — TabStrip::paint() already handles the zero-tabs
    // case internally (background + separator, no segments).
    ctx.canvas.stroke_rect(bounds_, ctx.theme.palette.popup_border, 1.0f);
}

void TabbedGridPicker::paint_overlay(tk::PaintCtx& ctx)
{
    if (bounds_.w <= 0.0f || bounds_.h <= 0.0f)
        return;
    if (needs_arrange_)
    {
        tk::LayoutCtx lctx{ctx.factory, ctx.theme};
        arrange(lctx, bounds_);
        needs_arrange_ = false;
    }
    paint(ctx);
}

// ─────────────────────────────────────────────────────────────────────────
//  Input
// ─────────────────────────────────────────────────────────────────────────
//
// Tab-strip hit-testing/press/wheel-scroll now live entirely on TabStrip
// (a real add_child'd child positioned at tab_rect_) — the generic
// Widget::dispatch_pointer_down/dispatch_pointer_up recursion (which the
// popup-click path in Host::dispatch_pointer_down already uses; see its own
// doc comment) reaches it exactly like it already does for search_field_/
// grid_, so this widget no longer needs its own on_pointer_down/
// on_pointer_up/on_wheel overrides at all.

bool TabbedGridPicker::on_key_down(const tk::KeyEvent& event)
{
    if (event.key == tk::Key::Escape)
    {
        on_popup_dismiss();
        return true;
    }
    return false;
}

void TabbedGridPicker::on_popup_dismiss()
{
    if (tab_strip_)
        tab_strip_->reset_interaction();
    if (on_dismiss)
        on_dismiss();
}

} // namespace tesseract::views
