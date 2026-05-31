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

#include "widget.h"

#include <cstddef>
#include <functional>
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

class GridView : public Widget
{
public:
    GridView() = default;

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
    void clamp_scroll();
    float content_height() const;

    struct ThumbGeom
    {
        float track_top, track_h, thumb_h, thumb_top;
    };
    ThumbGeom thumb_geom() const;
    bool thumb_hit(Point) const;

    GridAdapter* adapter_ = nullptr;

    float cell_w_ = 32;
    float cell_h_ = 32;
    float h_spacing_ = 2;
    float v_spacing_ = 2;
    Edges padding_ = {};

    int selected_index_ = -1;
    int hovered_index_ = -1;
    int pressed_index_ = -1;

    bool scrollbar_drag_ = false;
    float drag_anchor_y_ = 0;

    float scroll_y_ = 0;
};

class ListView : public Widget
{
public:
    ListView() = default;

    void set_adapter(ListAdapter* adapter);
    ListAdapter* adapter() const
    {
        return adapter_;
    }

    // Re-measure all row heights on the next arrange/paint.
    void invalidate_data();

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
    void reset_near_bottom_latch()
    {
        was_near_bottom_ = false;
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
    float scroll_y() const
    {
        return scroll_y_;
    }

    // Fractional scroll position [0,1] (0=top, 1=bottom).
    // Used by the tab system to save/restore position across tab switches.
    float scroll_fraction() const;
    void scroll_to_offset(float t);

    // Total content height (sum of all row heights). 0 if no adapter or
    // measure hasn't run yet.
    float content_height() const;

    // Index of the row at this widget-local point, or -1 if outside any
    // row (e.g. below the last one when the list is shorter than the
    // viewport).
    int index_at(Point local) const;

    // First and last item indices currently intersecting the viewport,
    // inclusive. Returns {0, -1} when the list is empty or not yet laid out.
    std::pair<int, int> visible_range() const;

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

protected:
    // True if row i lies strictly above the current viewport. When heights
    // are dirty, visible_range() returns {0,-1}; falls back to the stale
    // row_offsets_ so the check stays reliable even after a concurrent
    // invalidate_data() call made without an anchor.
    bool row_above_viewport(std::size_t i) const;

    int hovered_row_index() const
    {
        return hovered_index_;
    }

    // World-space rect for the row at `idx`. Returns an empty rect when `idx`
    // is out of range. The Y coordinate accounts for the current scroll offset
    // so it matches the position used by paint_row(). Subclasses can use this
    // in on_pointer_down to hit-test rows without waiting for a paint pass to
    // populate geometry caches.
    Rect row_world_rect(int idx) const
    {
        if (idx < 0 || static_cast<std::size_t>(idx) + 1 >= row_offsets_.size())
            return {};
        const float top = row_offsets_[static_cast<std::size_t>(idx)];
        const float bot = row_offsets_[static_cast<std::size_t>(idx) + 1];
        return {bounds_.x, bounds_.y + top - scroll_y_, bounds_.w, bot - top};
    }

    // Returns true when `local` (widget-local coords) is over the scrollbar
    // thumb. Subclasses that override on_pointer_down should test this first
    // so the scrollbar wins over any message-content hit test underneath it.
    bool thumb_hit(Point local) const;

private:
    void rebuild_heights(LayoutCtx&, float width);
    void clamp_scroll();
    void update_hover(Point local);
    void maybe_fire_near_top();
    void maybe_fire_near_bottom();
    void fire_latch_(bool now_near, bool& was_near,
                     const std::function<void()>& callback);
    std::size_t first_visible_row_() const;

    struct ThumbGeom
    {
        float track_top, track_h, thumb_h, thumb_top;
    };
    ThumbGeom thumb_geom() const;

    ListAdapter* adapter_ = nullptr;

    int selected_index_ = -1;
    int hovered_index_ = -1;
    int pressed_index_ = -1;

    // Scrollbar drag state. When `scrollbar_drag_` is true, the
    // pointer-down hit the thumb and `drag_anchor_y_` records the local
    // y-offset within the thumb that should stay under the cursor for
    // the duration of the drag.
    bool scrollbar_drag_ = false;
    float drag_anchor_y_ = 0;

    float scroll_y_ = 0;
    float measured_width_ = 0;
    bool heights_dirty_ = true;
    bool stick_to_bottom_ = false; // re-snap to bottom on heights rebuild

    // `on_near_top` machinery — see public docs.
    float near_top_threshold_px_ = 200.0f;
    bool was_near_top_ = false;

    // `on_near_bottom` machinery — see public docs.
    float near_bottom_threshold_px_ = 200.0f;
    bool was_near_bottom_ = false;

    // `preserve_top_through` machinery. Sentinel < 0 means "no anchor
    // pending". Set by `preserve_top_through`, consumed by `arrange` once
    // heights are rebuilt and a fresh `content_height()` is known. Stays
    // < 0 across normal arranges so width-driven rebuilds don't shift the
    // viewport.
    float anchor_pre_height_ = -1.0f;

    std::vector<float> row_heights_;
    std::vector<float> row_offsets_; // size = count + 1, last = total height
};

} // namespace tk
