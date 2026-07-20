#pragma once

// Virtualised vertical list. Backs the room list (a few hundred rows
// today, low-thousand-bound in the future) and the message list
// (thousands when fully paginated). Rows have variable height; heights
// are measured once and cached, then the viewport renders only the rows
// intersecting it.
//
// The model uses a borrowed `ListAdapter` rather than a tree of
// per-row Widgets — composing draws inline keeps the per-row cost to a
// single virtual call plus whatever the adapter chooses to paint.

#include "scrollable_base.h"
#include "widget.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace tk
{

class ListAdapter
{
public:
    virtual ~ListAdapter() = default;

    virtual std::size_t count() const = 0;

    // Return the row's pixel height given `available_width`.
    virtual float measure_row_height(std::size_t index, LayoutCtx& ctx,
                                     float available_width) = 0;

    // Paint a single row into `bounds`. `selected` and `hovered` reflect
    // user-interaction state owned by ListView.
    virtual void paint_row(std::size_t index, PaintCtx& ctx, Rect bounds,
                           bool selected, bool hovered) = 0;

    // Override to disable selection on individual rows (e.g. separators).
    virtual bool is_selectable(std::size_t /*index*/) const
    {
        return true;
    }

    // Stable identity for a row, used by ListView's scroll anchor to relocate
    // the anchored row after a height-changing mutation (prepend, async media
    // load) shifts indices. Empty means "no stable key" — the anchor falls
    // back to the captured index. Override to return e.g. an event id.
    virtual std::string row_key(std::size_t /*index*/) const
    {
        return {};
    }

    // The half-open range of rows whose measured height may change when row
    // `i` is inserted, updated, or removed. Used by ListView's targeted
    // (incremental) invalidation to re-measure only a bounded neighbourhood
    // instead of the whole list. The default — row `i` alone — suits adapters
    // whose rows are independent (e.g. the room list). Adapters whose heights
    // couple to neighbours (e.g. the message list's continuation grouping and
    // day-separator visibility) widen the span accordingly.
    virtual void height_dependency_span(std::size_t i, std::size_t& lo,
                                        std::size_t& hi) const
    {
        lo = i;
        hi = i + 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────
//  GridView — fixed-size cells arranged in rows that wrap to the
//  viewport's width. Used by the emoji picker (Step 5) and the future
//  sticker / image-pack pickers (Step 9). Wheel scrolling + selection
//  follow the same model as ListView; cell rendering is delegated to
//  the adapter.
// ─────────────────────────────────────────────────────────────────────────

class GridAdapter
{
public:
    virtual ~GridAdapter() = default;

    virtual std::size_t count() const = 0;

    // Paint one cell into `bounds`. The grid clamps the bounds to its
    // cell_size — the adapter never has to clip.
    virtual void paint_cell(std::size_t index, PaintCtx& ctx, Rect bounds,
                            bool selected, bool hovered) = 0;

    virtual bool is_selectable(std::size_t /*index*/) const
    {
        return true;
    }
};

class GridView : public ScrollableBase
{
protected:
    GridView() = default;
    TK_WIDGET_FACTORY_FRIEND(GridView)

public:
    void set_adapter(GridAdapter* adapter);
    GridAdapter* adapter() const
    {
        return adapter_;
    }

    void set_cell_size(float w, float h);
    void set_spacing(float h_spacing, float v_spacing);
    void set_padding(Edges padding);

    void set_selected_index(int idx);
    int selected_index() const
    {
        return selected_index_;
    }

    // Jump back to the top — callers use this after a content swap (tab
    // switch, search filter) where the old scroll offset no longer makes
    // sense against the new item count.
    void scroll_to_top()
    {
        scroll_y_ = 0;
    }

    std::function<void(int /*index*/)> on_cell_clicked;

    void invalidate_data();

    // Index of the cell at this widget-local point, or -1 if outside.
    int index_at(Point local) const;

    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void paint(PaintCtx&) override;
    bool on_wheel(Point local, float dx, float dy) override;
    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;
    void on_pointer_drag(Point local) override;
    bool on_pointer_move(Point local) override;
    void on_pointer_leave() override;

    // Keyboard-focusable whenever there's at least one cell. Left/Right
    // move by one cell, Up/Down by a full row; Enter/Space fires
    // on_cell_clicked for the current selection.
    bool focusable() const override
    {
        return enabled_ && adapter_ && adapter_->count() > 0;
    }
    bool on_key_down(const KeyEvent& e) override;

    // Index of the currently hovered cell (-1 when none).
    int hovered_index() const
    {
        return hovered_index_;
    }

    // Widget-local rect of cell `idx`, or a zero-area rect when out of bounds.
    tk::Rect rect_at(int idx) const;

private:
    int cols(float available_w) const;
    int rows(int n_cells, int cols_) const;
    float content_height() const override;

    // Scrolls scroll_y_ so cell `idx`'s row is fully visible, mirroring
    // ListView::scroll_to_index's minimal "ensure visible" clamp in this
    // grid's own row-of-cells space.
    void scroll_cell_into_view_(int idx);

    GridAdapter* adapter_ = nullptr;

    float cell_w_ = 32;
    float cell_h_ = 32;
    float h_spacing_ = 2;
    float v_spacing_ = 2;
    Edges padding_ = {};

    int selected_index_ = -1;
    int hovered_index_ = -1;
    int pressed_index_ = -1;
};

class ListView : public ScrollableBase
{
protected:
    ListView() = default;
    TK_WIDGET_FACTORY_FRIEND(ListView)

public:
    void set_adapter(ListAdapter* adapter);
    ListAdapter* adapter() const
    {
        return adapter_;
    }

    // Re-measure all row heights on the next arrange/paint.
    void invalidate_data();

    // Targeted (incremental) height invalidation. Instead of re-measuring
    // every row, these re-measure only the dependency span of the affected
    // row (see ListAdapter::height_dependency_span) and rewalk the row-offset
    // prefix sum from that point. The adapter's model must already reflect the
    // change before these are called (count() and row contents up to date).
    // They fall back to a full rebuild if the cached layout is structurally
    // out of step with the adapter, so callers can use them unconditionally.
    void invalidate_row(std::size_t index);
    void invalidate_rows(std::size_t lo, std::size_t hi);
    void insert_row(std::size_t index); // a row was inserted AT `index`
    void erase_row(std::size_t index);  // the row AT `index` was removed

    // Selection state, plumbed through `paint_row(... selected ...)`.
    void set_selected_index(int idx);
    int selected_index() const
    {
        return selected_index_;
    }

    // Click + hover hooks. The host's pointer-event pipeline calls into
    // on_pointer_down/up/move so these fire automatically.
    std::function<void(int /*index*/)> on_row_clicked;

    // Fired whenever scroll_y_ changes due to user input (wheel or scrollbar drag).
    std::function<void()> on_scroll;

    // Fired exactly once each time the viewport's top edge first enters the
    // `near_top_threshold_px` zone (default 200px). Re-arms when the user
    // scrolls back above the threshold, or when new rows are prepended (so
    // the scroll-up backfill trigger can fire again for the next page).
    // Not fired while `stick_to_bottom_` is true (initial state after
    // set_messages); the host must scroll up first.
    std::function<void()> on_near_top;
    void set_near_top_threshold_px(float px)
    {
        near_top_threshold_px_ = px;
    }
    float near_top_threshold_px() const
    {
        return near_top_threshold_px_;
    }
    // Re-arm the `on_near_top` latch. Call after data changes so the next
    // approach to the top fires again.
    void reset_near_top_latch()
    {
        was_near_top_ = false;
    }
    // When true, the arrange-time "content shorter than viewport" auto-fire of
    // on_near_top only triggers if the list is empty (0 rows) — never merely to
    // fill a partly-filled viewport. Mirrors Element X: a room switch shows the
    // cached tail immediately and back-paginates only on scroll. Enabled on the
    // message list; off elsewhere (thread lists keep fill-on-open).
    void set_autofill_only_when_empty(bool v)
    {
        autofill_only_when_empty_ = v;
    }
    // When true, content shorter than the viewport is anchored to the bottom
    // (empty space above), as a chat timeline expects, rather than the top.
    void set_anchor_content_bottom(bool v)
    {
        anchor_content_bottom_ = v;
    }

    // Fired exactly once each time the viewport's bottom edge first enters the
    // near_bottom_threshold_px zone (default 200px). Re-arms when content is
    // appended below or the user scrolls away from the bottom.
    // Not fired while stick_to_bottom_ is true.
    // Also not fired when content fits within the viewport.
    std::function<void()> on_near_bottom;
    void set_near_bottom_threshold_px(float px)
    {
        near_bottom_threshold_px_ = px;
    }
    float near_bottom_threshold_px() const
    {
        return near_bottom_threshold_px_;
    }

    // Run `mutate` (which is expected to change the adapter's row count
    // and call `invalidate_data`) while preserving the user's visual
    // position: if rows were added above the current viewport, the
    // numeric `scroll_y_` is bumped by the height delta so the row the
    // user is looking at stays under their cursor. No-op when
    // `stick_to_bottom_` is set (the user is reading the bottom and
    // doesn't care where the top is).
    void preserve_top_through(const std::function<void()>& mutate);

    // Scroll API. Vertical scrolling only; horizontal scroll is intentional
    // omitted — chat rows wrap their content to the list width.
    void scroll_to_top();
    void scroll_to_bottom();
    void scroll_to_index(int idx, bool align_top = false);
    // Like scroll_to_index, but deferred until heights are valid. Callers that
    // mutate data (which marks heights dirty) can't scroll_to_index immediately
    // because row_offsets_ isn't rebuilt until the next arrange()/paint. Stash
    // the request here; it is consumed once the rows are re-measured.
    void scroll_to_index_deferred(int idx, bool align_top = false)
    {
        pending_scroll_idx_ = idx;
        pending_scroll_align_top_ = align_top;
    }

    // Fractional scroll position [0,1] (0=top, 1=bottom).
    // Used by the tab system to save/restore position across tab switches.
    float scroll_fraction() const;
    void scroll_to_offset(float t);

    // Total content height (sum of all row heights). 0 if no adapter or
    // measure hasn't run yet.
    float content_height() const override;

    // Index of the row at this widget-local point, or -1 if outside any
    // row (e.g. below the last one when the list is shorter than the
    // viewport).
    int index_at(Point local) const;

    // First and last item indices currently intersecting the viewport,
    // inclusive. Returns {0, -1} when the list is empty or not yet laid out.
    std::pair<int, int> visible_range() const;

    // World-space rect for the row at `idx`. Returns an empty rect when `idx`
    // is out of range. The Y coordinate accounts for the current scroll offset
    // so it matches the position used by paint_row(). Used by subclasses and
    // by container views (e.g. a sticky-header overlay) to locate rows without
    // waiting for a paint pass to populate geometry caches.
    Rect row_world_rect(int idx) const
    {
        if (idx < 0 || static_cast<std::size_t>(idx) + 1 >= row_offsets_.size())
            return {};
        const float top = row_offsets_[static_cast<std::size_t>(idx)];
        const float bot = row_offsets_[static_cast<std::size_t>(idx) + 1];
        return {bounds_.x, bounds_.y + content_top_pad_() + top - scroll_y_,
                bounds_.w, bot - top};
    }

    // Force a height rebuild if dirty (and re-snap to bottom when sticking)
    // without painting any rows. Lets a subclass get a valid visible_range()
    // before it decides whether to paint. Safe to call from paint(); the
    // base paint() already calls this internally.
    void ensure_measured(PaintCtx& ctx);

    // Widget overrides
    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void paint(PaintCtx&) override;
    bool on_wheel(Point local, float dx, float dy) override;
    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;
    void on_pointer_drag(Point local) override;
    bool on_pointer_move(Point local) override;
    void on_pointer_leave() override;

    // Keyboard-focusable whenever there's at least one row. Up/Down move
    // the selection (skipping non-selectable rows, clamping rather than
    // wrapping at the ends); Enter/Space fires on_row_clicked for the
    // current selection.
    bool focusable() const override
    {
        return enabled_ && adapter_ && adapter_->count() > 0;
    }
    bool on_key_down(const KeyEvent& e) override;

    // See Widget::focus_on_click's doc comment. Defaults to true (an
    // ordinary click focuses the list, same as any other focusable
    // widget); a list that's purely mouse-driven at a given call site
    // (e.g. RoomListView's inner row list, whose click already performs a
    // complete row-selection action) can opt out with
    // set_focus_on_click(false) while remaining Tab/arrow-key-navigable.
    bool focus_on_click() const override
    {
        return focus_on_click_;
    }
    void set_focus_on_click(bool focus_on_click)
    {
        focus_on_click_ = focus_on_click;
    }

protected:
    int hovered_row_index() const
    {
        return hovered_index_;
    }

    // Called from arrange() right after an anchored relayout has repositioned
    // the viewport so the anchor row stays pixel-stable. Subclasses override
    // to re-resolve pointer-derived state (hover highlight, chip targets) that
    // may have gone stale because indices shifted or rows changed height.
    virtual void on_anchored_relayout_()
    {
    }

    // Background fill drawn behind the rows in paint(). Defaults to the
    // sidebar tint (room list); the message list overrides this to match
    // the room header background instead.
    virtual Color background_color(const Theme& theme) const
    {
        return theme.palette.sidebar_bg;
    }

    // Re-run the base hover hit-test from a widget-local pointer position.
    // Exposes the private update_hover so subclasses can refresh hovered_index_
    // after an anchored relayout without a fresh pointer event.
    void refresh_hover_at(Point local)
    {
        update_hover(local);
    }

    void clear_hover_()
    {
        hovered_index_ = -1;
    }

    // thumb_hit() is inherited from ScrollableBase (protected). Subclasses that
    // override on_pointer_down test it first so the scrollbar wins over any
    // message-content hit test underneath it.

private:
    // Next/previous selectable row index from `from`, stepping by `dir`
    // (±1), skipping rows where is_selectable() is false. Clamps (does not
    // wrap) at the ends, returning -1 once stepping would go out of range.
    // `from == -1` (cold start) picks the first (dir>0) or last (dir<0)
    // selectable row.
    int next_selectable_(int from, int dir) const;

    void rebuild_heights(LayoutCtx&, float width);
    // Re-measure only the accumulated dirty range and rewalk offsets from it.
    void rebuild_dirty_(LayoutCtx&, float width);
    // Widen the pending dirty range (no-op when lo >= hi).
    void mark_dirty_range_(std::size_t lo, std::size_t hi);
    // Widen the dirty range to the adapter's dependency span for `index`.
    void mark_dependency_span_(std::size_t index);
    // Re-anchor a pending dirty range to the same logical rows after an
    // insert/erase at `index` shifts everything at or after it.
    void shift_dirty_range_for_insert_(std::size_t index);
    void shift_dirty_range_for_erase_(std::size_t index);
    // Apply the captured scroll anchor after a rebuild (full or partial).
    void consume_scroll_anchor_();
    // Apply a pending scroll_to_index_deferred() request once heights are valid.
    void consume_pending_scroll_();
    void update_hover(Point local);
    void capture_anchor_();
    // Locate the anchored row by its stable key in the rebuilt layout.
    // Returns true and sets out_index when found; false when the key is empty
    // or the row is gone (caller uses the height-delta fallback).
    bool locate_anchor_(std::size_t& out_index) const;
    void maybe_fire_near_top();
    void maybe_fire_near_bottom();
    // Vertical offset (px) pushing rows down so short content sits at the bottom
    // of the viewport. Zero unless anchor_content_bottom_ is set and content is
    // shorter than the viewport. Must be applied consistently to every
    // screen<->content y mapping (paint, hit-test, visible range).
    float content_top_pad_() const;
    void fire_latch_(bool now_near, bool& was_near,
                     const std::function<void()>& callback);
    std::size_t first_visible_row_() const;

    // scroll_y_, the scrollbar drag state, clamp_scroll(), thumb_geom(), and
    // thumb_hit() are inherited from ScrollableBase.

    ListAdapter* adapter_ = nullptr;

    // See set_focus_on_click() above.
    bool focus_on_click_ = true;

    int selected_index_ = -1;
    int hovered_index_ = -1;
    int pressed_index_ = -1;

    float measured_width_ = 0;
    bool heights_dirty_ = true; // full rebuild pending (subsumes dirty range)
    bool stick_to_bottom_ = false; // re-snap to bottom on heights rebuild

    // Deferred scroll_to_index request (see scroll_to_index_deferred). Applied
    // by consume_pending_scroll_() once row_offsets_ is valid again, then reset.
    int  pending_scroll_idx_ = -1;
    bool pending_scroll_align_top_ = false;

    // Accumulated targeted-invalidation range [dirty_lo_, dirty_hi_). Active
    // only when has_dirty_range_ and heights_dirty_ is false. A full rebuild
    // (heights_dirty_ or width change) clears it.
    bool has_dirty_range_ = false;
    std::size_t dirty_lo_ = 0;
    std::size_t dirty_hi_ = 0;

    // `on_near_top` machinery — see public docs.
    float near_top_threshold_px_ = 200.0f;
    bool was_near_top_ = false;
    // See set_autofill_only_when_empty / set_anchor_content_bottom.
    bool autofill_only_when_empty_ = false;
    bool anchor_content_bottom_ = false;

    // `on_near_bottom` machinery — see public docs.
    float near_bottom_threshold_px_ = 200.0f;
    bool was_near_bottom_ = false;

    // `preserve_top_through` machinery. capture_anchor_() records an anchor
    // when a height-changing mutation begins; arrange() consumes it after
    // rebuild_heights. When the adapter supplies a stable row_key, we pin that
    // row's top to its pre-mutation screen position:
    //   scroll_y_ = row_offsets_[new_index] - offset
    // so the anchored row stays pixel-stable regardless of whether height
    // changed above, at, or below it (and regardless of index shifts from a
    // prepend). When the key is empty or the row can't be relocated, we fall
    // back to the legacy total-height delta (scroll_y_ += new_total - pre),
    // which is correct for pure top-prepends — the behaviour keyless adapters
    // (room list, thread list) have always relied on. `pending` stays false
    // across width-driven rebuilds so a resize never shifts the viewport.
    struct ScrollAnchor
    {
        bool        pending    = false;
        std::size_t index      = 0;   // captured row index (within old layout)
        std::string key;              // stable identity (preferred locator)
        float       offset     = 0.f; // row_offsets_[index] - scroll_y_
        float       pre_height = 0.f; // content_height() before the mutation
    };
    ScrollAnchor anchor_;
    bool anchored_relayout_pending_ = false;

    std::vector<float> row_heights_;
    std::vector<float> row_offsets_; // size = count + 1, last = total height
};

} // namespace tk
