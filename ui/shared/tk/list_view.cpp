#include "list_view.h"

#include <algorithm>
#include <cmath>

namespace tk
{

namespace
{

constexpr int kInvalidIndex = -1;

} // namespace

void ListView::set_adapter(ListAdapter* adapter)
{
    adapter_ = adapter;
    heights_dirty_ = true;
    has_dirty_range_ = false;
    selected_index_ = kInvalidIndex;
    hovered_index_ = kInvalidIndex;
    pressed_index_ = kInvalidIndex;
    scroll_y_ = 0;
}

void ListView::invalidate_data()
{
    heights_dirty_ = true;
    has_dirty_range_ = false;
}

void ListView::mark_dirty_range_(std::size_t lo, std::size_t hi)
{
    if (lo >= hi)
    {
        return;
    }
    if (!has_dirty_range_)
    {
        dirty_lo_ = lo;
        dirty_hi_ = hi;
        has_dirty_range_ = true;
    }
    else
    {
        dirty_lo_ = std::min(dirty_lo_, lo);
        dirty_hi_ = std::max(dirty_hi_, hi);
    }
}

void ListView::mark_dependency_span_(std::size_t index)
{
    if (!adapter_)
    {
        return;
    }
    std::size_t lo = index;
    std::size_t hi = index + 1;
    adapter_->height_dependency_span(index, lo, hi);
    const std::size_t n = adapter_->count();
    mark_dirty_range_(std::min(lo, n), std::min(hi, n));
}

void ListView::invalidate_row(std::size_t index)
{
    if (heights_dirty_)
    {
        return; // a full rebuild already subsumes this
    }
    if (index >= row_heights_.size())
    {
        invalidate_data();
        return;
    }
    mark_dependency_span_(index);
}

void ListView::invalidate_rows(std::size_t lo, std::size_t hi)
{
    if (heights_dirty_)
    {
        return;
    }
    const std::size_t n = adapter_ ? adapter_->count() : 0;
    if (row_heights_.size() != n)
    {
        invalidate_data();
        return;
    }
    mark_dirty_range_(std::min(lo, n), std::min(hi, n));
}

void ListView::insert_row(std::size_t index)
{
    // The adapter's model already reflects the insertion, so count() is the
    // post-insert size; the cached vectors are still pre-insert.
    const std::size_t n = adapter_ ? adapter_->count() : 0;
    if (heights_dirty_)
    {
        return; // full rebuild will pick up the new row
    }
    if (n == 0 || row_heights_.size() + 1 != n || index > row_heights_.size())
    {
        invalidate_data();
        return;
    }
    // Splice a placeholder; rebuild_dirty_ measures it and rewalks offsets.
    // Copy the offset to a local first: passing row_offsets_[index] directly
    // would be a reference into the same vector, which is UB if insert
    // reallocates (the common at-capacity tail-append path).
    const float offset_at_index = row_offsets_[index];
    row_heights_.insert(row_heights_.begin() + index, 0.0f);
    row_offsets_.insert(row_offsets_.begin() + index, offset_at_index);
    mark_dependency_span_(index);
    was_near_bottom_ = false; // content appeared — re-arm the backfill latch
}

void ListView::erase_row(std::size_t index)
{
    // count() is the post-erase size; the cached vectors are still pre-erase.
    const std::size_t n = adapter_ ? adapter_->count() : 0;
    if (heights_dirty_)
    {
        return;
    }
    if (index >= row_heights_.size() || row_heights_.size() != n + 1)
    {
        invalidate_data();
        return;
    }
    row_heights_.erase(row_heights_.begin() + index);
    // Drop the boundary offset *after* the erased row so the prefix sum up to
    // `index` stays correct; rebuild_dirty_ rewalks everything from there on.
    row_offsets_.erase(row_offsets_.begin() + index + 1);
    if (n == 0)
    {
        return; // nothing left to measure; offsets are already {0}
    }
    // Re-measure the neighbourhood the now-removed row coupled to.
    mark_dependency_span_(std::min(index, n - 1));
}

void ListView::rebuild_dirty_(LayoutCtx& ctx, float width)
{
    has_dirty_range_ = false;
    const std::size_t n = adapter_ ? adapter_->count() : 0;
    if (row_heights_.size() != n || row_offsets_.size() != n + 1)
    {
        rebuild_heights(ctx, width); // structural drift — rebuild fully
        return;
    }
    const std::size_t lo = std::min(dirty_lo_, n);
    const std::size_t hi = std::min(dirty_hi_, n);
    for (std::size_t i = lo; i < hi; ++i)
    {
        row_heights_[i] = adapter_->measure_row_height(i, ctx, width);
    }
    // Rewalk the prefix sum from the first dirty row to the end. Rows above lo
    // are untouched, so row_offsets_[lo] is already correct.
    float cursor = row_offsets_[lo];
    for (std::size_t i = lo; i < n; ++i)
    {
        row_offsets_[i] = cursor;
        cursor += row_heights_[i];
    }
    row_offsets_[n] = cursor;
}

void ListView::consume_scroll_anchor_()
{
    if (!anchor_.pending)
    {
        return;
    }
    std::size_t new_idx = 0;
    if (locate_anchor_(new_idx) && new_idx + 1 < row_offsets_.size())
    {
        // Pin the anchored row's top to the screen Y it had pre-mutation.
        scroll_y_ = row_offsets_[new_idx] - anchor_.offset;
    }
    else
    {
        // No stable key / row vanished: shift by the total height delta.
        scroll_y_ += content_height() - anchor_.pre_height;
    }
    anchor_.pending = false;
    was_near_top_ = false;
    anchored_relayout_pending_ = true;
}

void ListView::set_selected_index(int idx)
{
    if (idx == selected_index_)
    {
        return;
    }
    selected_index_ = idx;
}

void ListView::scroll_to_top()
{
    scroll_y_ = 0;
    stick_to_bottom_ = false;
}
void ListView::scroll_to_bottom()
{
    stick_to_bottom_ = true;
    // Snap eagerly: hosts repaint without re-running arrange, so deferring
    // the snap to the next arrange() means the click does nothing visible
    // until the window is resized or another data mutation triggers a
    // relayout. `content_height()` is already valid here because the pill
    // is only ever drawn after a frame that has measured the rows.
    scroll_y_ = std::max(0.0f, content_height() - bounds_.h);
    clamp_scroll();
}

void ListView::scroll_to_index(int idx, bool align_top)
{
    if (!adapter_ || idx < 0 ||
        static_cast<std::size_t>(idx) >= adapter_->count())
    {
        return;
    }
    // row_offsets_ is sized (count+1) as of the last rebuild_heights(); the
    // adapter count can have grown since (heights dirty), so bound against
    // row_offsets_ itself, not the live adapter count, to avoid OOB.
    if (static_cast<std::size_t>(idx) + 1 >= row_offsets_.size())
    {
        return;
    }
    if (align_top)
    {
        scroll_y_ = row_offsets_[idx];
    }
    else
    {
        // Bring into view: scroll only enough that the row sits inside
        // [scroll_y, scroll_y + viewport_h].
        float top = row_offsets_[idx];
        float bot = row_offsets_[idx + 1];
        float viewport = bounds_.h;
        if (top < scroll_y_)
        {
            scroll_y_ = top;
        }
        else if (bot > scroll_y_ + viewport)
        {
            scroll_y_ = bot - viewport;
        }
    }
    stick_to_bottom_ = false;
    clamp_scroll();
}

void ListView::consume_pending_scroll_()
{
    if (pending_scroll_idx_ < 0)
    {
        return;
    }
    const int idx = pending_scroll_idx_;
    const bool align_top = pending_scroll_align_top_;
    pending_scroll_idx_ = -1;
    scroll_to_index(idx, align_top);
}

float ListView::content_height() const
{
    return row_offsets_.empty() ? 0.0f : row_offsets_.back();
}

std::size_t ListView::first_visible_row_() const
{
    auto it = std::upper_bound(row_offsets_.begin(), row_offsets_.end(), scroll_y_);
    return it == row_offsets_.begin()
               ? 0
               : static_cast<std::size_t>(it - row_offsets_.begin() - 1);
}

std::pair<int, int> ListView::visible_range() const
{
    const std::size_t n = adapter_ ? adapter_->count() : 0;
    if (n == 0 || heights_dirty_ || row_offsets_.size() < n + 1)
    {
        return {0, -1};
    }

    float viewport_bot = scroll_y_ + bounds_.h;
    std::size_t first = first_visible_row_();

    int last = -1;
    for (std::size_t i = first; i < n; ++i)
    {
        if (row_offsets_[i] >= viewport_bot)
        {
            break;
        }
        last = static_cast<int>(i);
    }
    return {static_cast<int>(first), last};
}

int ListView::index_at(Point local) const
{
    if (!adapter_ || row_offsets_.empty())
    {
        return kInvalidIndex;
    }
    float content_y = local.y + scroll_y_;
    if (content_y < 0 || content_y >= row_offsets_.back())
    {
        return kInvalidIndex;
    }
    auto it =
        std::upper_bound(row_offsets_.begin(), row_offsets_.end(), content_y);
    int idx = static_cast<int>(it - row_offsets_.begin()) - 1;
    if (idx < 0)
    {
        idx = 0;
    }
    if (static_cast<std::size_t>(idx) >= adapter_->count())
    {
        idx = kInvalidIndex;
    }
    return idx;
}

Size ListView::measure(LayoutCtx&, Size constraints)
{
    return constraints;
}

void ListView::arrange(LayoutCtx& ctx, Rect bounds)
{
    bounds_ = bounds;
    if (!adapter_)
    {
        row_heights_.clear();
        row_offsets_.clear();
        anchor_.pending = false;
        return;
    }
    const bool width_changed = measured_width_ != bounds.w;
    if (heights_dirty_ || width_changed)
    {
        // Snapshot the last-measured row count (from row_heights_, which
        // rebuild_heights will overwrite) rather than querying the adapter,
        // which may already reflect additions made before this arrange call.
        std::size_t prev_count = row_heights_.size();
        rebuild_heights(ctx, bounds.w);
        has_dirty_range_ = false; // subsumed by the full rebuild
        if (anchor_.pending)
        {
            // Preserve the user's visual position by pinning the anchored row
            // (see consume_scroll_anchor_). row_offsets_[idx] is the prefix sum
            // of heights above the anchor, so growth/prepend above it moves
            // prefix and scroll together (screen Y unchanged), growth of the
            // anchor row only moves its bottom, and growth strictly below
            // leaves the prefix — hence scroll_y_ — untouched.
            consume_scroll_anchor_();
        }
        else
        {
            // Re-arm the near-bottom trigger only when rows were actually
            // appended. A pure resize (same row count, different width)
            // must not spuriously re-fire on_near_bottom.
            std::size_t new_count = adapter_ ? adapter_->count() : 0;
            if (new_count > prev_count)
            {
                was_near_bottom_ = false;
            }
        }
    }
    else if (has_dirty_range_)
    {
        // Targeted path: re-measure only the dirty span and rewalk offsets.
        rebuild_dirty_(ctx, bounds.w);
        // insert_row/erase_row already re-armed the near-bottom latch; here we
        // only need to settle the scroll anchor a preserve_top_through set up.
        consume_scroll_anchor_();
    }
    if (stick_to_bottom_)
    {
        scroll_y_ = std::max(0.0f, content_height() - bounds.h);
    }
    clamp_scroll();
    consume_pending_scroll_();
    if (anchored_relayout_pending_)
    {
        anchored_relayout_pending_ = false;
        on_anchored_relayout_();
    }
    if (on_near_top && adapter_->count() > 0 && content_height() < bounds_.h)
    {
        // Loaded content doesn't fill the viewport — request the next page
        // immediately rather than waiting for a scroll gesture. Safe to call
        // on every arrange: the consumer's own in-flight/reached-start gate
        // (see ShellBase::pagination_) makes redundant calls harmless no-ops,
        // and it naturally stops once the room's real history is exhausted.
        on_near_top();
    }
}

void ListView::preserve_top_through(const std::function<void()>& mutate)
{
    if (stick_to_bottom_)
    {
        // The user is reading the latest message — the top edge is off-
        // screen and irrelevant. Just mutate without anchoring.
        if (mutate)
        {
            mutate();
        }
        return;
    }
    // When the user is already at the very top of the content (no rows
    // hidden above the viewport), don't anchor: the natural behaviour is
    // for the newly-loaded older rows to appear *in* the viewport, not
    // above it. Without this special case the visual content is unchanged
    // — the new rows land off-screen above and the only signal is a
    // shrinking scrollbar thumb, which reads to users as "nothing
    // happened". Re-arm the near-top latch directly because arrange()
    // only does so when it applies an anchor delta.
    constexpr float kAtTopEpsilon = 1.0f;
    if (scroll_y_ <= kAtTopEpsilon)
    {
        if (mutate)
        {
            mutate();
        }
        was_near_top_ = false;
        return;
    }
    // Capture the row anchor once; if multiple height-changing mutations stack
    // up before the next arrange, they all share this single snapshot so the
    // anchored row stays consistent.
    if (!anchor_.pending)
    {
        capture_anchor_();
    }
    if (mutate)
    {
        mutate();
    }
}

void ListView::capture_anchor_()
{
    // Always record the pre-mutation total height so the height-delta fallback
    // is available even before any per-row layout exists.
    anchor_.key.clear();
    anchor_.index      = 0;
    anchor_.offset     = 0.0f;
    anchor_.pre_height = content_height();
    anchor_.pending    = true;

    // `measured` is the row count of the *current* (pre-mutation) layout. It
    // may differ from adapter_->count() when the adapter swapped its backing
    // model just before calling preserve_top_through; we anchor against the
    // measured layout the user is actually looking at.
    const std::size_t measured =
        row_offsets_.empty() ? 0 : row_offsets_.size() - 1;
    if (measured == 0)
    {
        return; // no per-row data yet — height-delta fallback only
    }
    // Anchor the row under the mouse so it stays pixel-stable; with no pointer
    // over the list, fall back to the top-of-viewport row.
    std::size_t idx;
    if (hovered_index_ >= 0 && static_cast<std::size_t>(hovered_index_) < measured)
    {
        idx = static_cast<std::size_t>(hovered_index_);
    }
    else
    {
        idx = std::min(first_visible_row_(), measured - 1);
    }
    anchor_.index  = idx;
    anchor_.offset = row_offsets_[idx] - scroll_y_;
    // row_key must be read against the OLD model. Adapters that swap their
    // model before preserve_top_through (e.g. the thread list) leave the key
    // empty and rely on the height-delta fallback; adapters that want precise
    // row anchoring swap inside the mutate lambda so the OLD row is still here.
    const std::size_t n = adapter_ ? adapter_->count() : 0;
    if (idx < n)
    {
        anchor_.key = adapter_->row_key(idx);
    }
}

bool ListView::locate_anchor_(std::size_t& out_index) const
{
    if (anchor_.key.empty())
    {
        return false;
    }
    const std::size_t n = adapter_ ? adapter_->count() : 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (adapter_->row_key(i) == anchor_.key)
        {
            out_index = i;
            return true;
        }
    }
    return false; // anchored row removed by a concurrent update
}

void ListView::fire_latch_(bool now_near, bool& was_near,
                           const std::function<void()>& callback)
{
    if (now_near && !was_near)
    {
        was_near = true;
        if (callback)
            callback();
    }
    else if (!now_near && was_near)
    {
        was_near = false;
    }
}

void ListView::maybe_fire_near_top()
{
    if (!adapter_ || adapter_->count() == 0 || stick_to_bottom_ || scrollbar_drag_)
        return;
    fire_latch_(scroll_y_ < near_top_threshold_px_, was_near_top_, on_near_top);
}

void ListView::maybe_fire_near_bottom()
{
    if (!adapter_ || adapter_->count() == 0 || stick_to_bottom_ || scrollbar_drag_)
        return;
    float total = content_height();
    if (total <= bounds_.h)
        return;
    fire_latch_((total - (scroll_y_ + bounds_.h)) < near_bottom_threshold_px_,
                was_near_bottom_, on_near_bottom);
}

void ListView::rebuild_heights(LayoutCtx& ctx, float width)
{
    measured_width_ = width;
    heights_dirty_ = false;
    std::size_t n = adapter_ ? adapter_->count() : 0;
    row_heights_.resize(n);
    row_offsets_.assign(n + 1, 0.0f);
    float cursor = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        row_heights_[i] = adapter_->measure_row_height(i, ctx, width);
        row_offsets_[i] = cursor;
        cursor += row_heights_[i];
    }
    row_offsets_[n] = cursor;
}

void ListView::ensure_measured(PaintCtx& ctx)
{
    if (!adapter_)
    {
        return;
    }
    // Heights may be dirty when data changed since the last arrange — e.g. a
    // collapse toggle calls invalidate_data() then triggers a repaint without
    // an intervening layout pass. Rebuild now so paint_row receives correct
    // bounds rather than stale row heights from the previous item set.
    if (heights_dirty_ || has_dirty_range_)
    {
        LayoutCtx lctx{ctx.factory, ctx.theme};
        if (heights_dirty_)
        {
            rebuild_heights(lctx, measured_width_);
            has_dirty_range_ = false;
        }
        else
        {
            rebuild_dirty_(lctx, measured_width_);
        }
        // A paint can land before the deferred arrange that a relayout was
        // scheduled for. Consume any pending scroll anchor here too, otherwise
        // clearing the dirty state above would strand it (the later arrange
        // sees nothing dirty and never pins the anchored row).
        if (anchor_.pending)
        {
            consume_scroll_anchor_();
        }
        if (stick_to_bottom_)
        {
            scroll_y_ = std::max(0.0f, content_height() - bounds_.h);
        }
        clamp_scroll();
        consume_pending_scroll_();
    }
}

void ListView::paint(PaintCtx& ctx)
{
    // Background.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.sidebar_bg);

    if (!adapter_ || adapter_->count() == 0)
    {
        return;
    }

    ensure_measured(ctx);

    ctx.canvas.push_clip_rect(bounds_);

    // Find the first row whose end is past the viewport top.
    float viewport_bot = scroll_y_ + bounds_.h;
    std::size_t first = first_visible_row_();

    // Snapshot the canvas clip once (after push_clip_rect above). For partial
    // repaints such as animated-image ticks the clip is a small rect around the
    // GIF; skipping rows that fall entirely outside it avoids all the per-row
    // work (build_text, span iteration, image lookups) that QPainter would
    // otherwise silently discard at pixel-write time.
    const Rect clip = ctx.canvas.clip_rect();

    // Bound by row_offsets_ (size = count+1 as of the last rebuild), not the
    // live adapter count: if rows were appended without invalidate_data()
    // since the last rebuild, the two diverge and row_offsets_[i+1] would be
    // out of bounds. The next layout pass reconciles them.
    std::size_t row_limit = row_offsets_.empty() ? 0 : row_offsets_.size() - 1;
    std::size_t paint_end = std::min(adapter_->count(), row_limit);
    for (std::size_t i = first; i < paint_end; ++i)
    {
        float row_top = row_offsets_[i];
        float row_bottom = row_offsets_[i + 1];
        if (row_top >= viewport_bot)
        {
            break;
        }
        Rect row_bounds{bounds_.x, bounds_.y + (row_top - scroll_y_), bounds_.w,
                        row_bottom - row_top};
        if (row_bounds.bottom() <= clip.y || row_bounds.y >= clip.bottom())
        {
            continue;
        }
        bool selected = (static_cast<int>(i) == selected_index_);
        bool hovered = (static_cast<int>(i) == hovered_index_);
        adapter_->paint_row(i, ctx, row_bounds, selected, hovered);
    }

    ctx.canvas.pop_clip();

    // Scrollbar overlay — only if content overflows the viewport.
    paint_scrollbar(ctx);
}

bool ListView::on_wheel(Point /*local*/, float /*dx*/, float dy)
{
    if (!adapter_ || adapter_->count() == 0)
    {
        return false;
    }
    float prev = scroll_y_;
    scroll_y_ += dy;
    stick_to_bottom_ = false;
    clamp_scroll();
    maybe_fire_near_top();
    maybe_fire_near_bottom();
    bool changed = (scroll_y_ != prev);
    if (changed && on_scroll)
    {
        on_scroll();
    }
    return changed;
}

bool ListView::on_pointer_down(Point local)
{
    // Scrollbar thumb first — it sits on top of the rows visually, so
    // it should win the press regardless of which row is underneath.
    if (scrollbar_on_pointer_down(local))
    {
        stick_to_bottom_ = false;
        return true;
    }
    if (!adapter_)
    {
        return false;
    }
    int idx = index_at(local);
    if (idx == kInvalidIndex)
    {
        return false;
    }
    if (!adapter_->is_selectable(idx))
    {
        return false;
    }
    pressed_index_ = idx;
    return true;
}

void ListView::on_pointer_drag(Point local)
{
    float prev = scroll_y_;
    if (!scrollbar_on_pointer_drag(local))
    {
        return;
    }
    maybe_fire_near_top();
    maybe_fire_near_bottom();
    if (scroll_y_ != prev && on_scroll)
    {
        on_scroll();
    }
}

void ListView::on_pointer_up(Point local, bool inside_self)
{
    if (scrollbar_on_pointer_up())
    {
        maybe_fire_near_top();    // check now that the drag guard is lifted
        maybe_fire_near_bottom(); // check now that the drag guard is lifted
        return;                   // drag releases never select a row
    }
    int idx = inside_self ? index_at(local) : kInvalidIndex;
    if (pressed_index_ != kInvalidIndex && pressed_index_ == idx)
    {
        selected_index_ = idx;
        if (on_row_clicked)
        {
            on_row_clicked(idx);
        }
    }
    pressed_index_ = kInvalidIndex;
}

float ListView::scroll_fraction() const
{
    float max_s = std::max(0.f, content_height() - bounds_.h);
    return (max_s > 0.f) ? scroll_y_ / max_s : 0.f;
}

void ListView::scroll_to_offset(float t)
{
    float max_s = std::max(0.f, content_height() - bounds_.h);
    scroll_y_ = t * max_s;
    clamp_scroll();
    stick_to_bottom_ = false;
}

// ─────────────────────────────────────────────────────────────────────────
//  GridView
// ─────────────────────────────────────────────────────────────────────────

void GridView::set_adapter(GridAdapter* adapter)
{
    adapter_ = adapter;
    selected_index_ = kInvalidIndex;
    hovered_index_ = kInvalidIndex;
    pressed_index_ = kInvalidIndex;
    scroll_y_ = 0;
}

void GridView::set_cell_size(float w, float h)
{
    cell_w_ = w;
    cell_h_ = h;
}

void GridView::set_spacing(float h_spacing, float v_spacing)
{
    h_spacing_ = h_spacing;
    v_spacing_ = v_spacing;
}

void GridView::set_padding(Edges padding)
{
    padding_ = padding;
}

void GridView::set_selected_index(int idx)
{
    selected_index_ = idx;
}

void GridView::invalidate_data()
{ /* nothing cached — paint reads on demand */
}

int GridView::cols(float available_w) const
{
    if (cell_w_ <= 0)
    {
        return 1;
    }
    float inner = std::max(0.0f, available_w - padding_.horizontal());
    int c = static_cast<int>((inner + h_spacing_) / (cell_w_ + h_spacing_));
    return std::max(1, c);
}

int GridView::rows(int n_cells, int cols_) const
{
    if (cols_ <= 0 || n_cells <= 0)
    {
        return 0;
    }
    return (n_cells + cols_ - 1) / cols_;
}

int GridView::index_at(Point local) const
{
    if (!adapter_ || adapter_->count() == 0)
    {
        return kInvalidIndex;
    }
    int c = cols(bounds_.w);
    float x = local.x - padding_.left;
    float y = local.y + scroll_y_ - padding_.top;
    if (x < 0 || y < 0)
    {
        return kInvalidIndex;
    }
    int col = static_cast<int>(x / (cell_w_ + h_spacing_));
    int row = static_cast<int>(y / (cell_h_ + v_spacing_));
    // Reject the gap between cells.
    float cell_local_x = x - col * (cell_w_ + h_spacing_);
    float cell_local_y = y - row * (cell_h_ + v_spacing_);
    if (cell_local_x >= cell_w_ || cell_local_y >= cell_h_)
    {
        return kInvalidIndex;
    }
    if (col >= c)
    {
        return kInvalidIndex;
    }
    int idx = row * c + col;
    if (idx < 0 || static_cast<std::size_t>(idx) >= adapter_->count())
    {
        return kInvalidIndex;
    }
    return idx;
}

bool GridView::on_pointer_move(Point local)
{
    int idx = index_at(local);
    if (idx == hovered_index_)
    {
        return false;
    }
    hovered_index_ = idx;
    invalidate_data();
    return true;
}

void GridView::on_pointer_leave()
{
    if (hovered_index_ == -1)
    {
        return;
    }
    hovered_index_ = -1;
    invalidate_data();
}

tk::Rect GridView::rect_at(int idx) const
{
    if (!adapter_ || idx < 0 ||
        static_cast<std::size_t>(idx) >= adapter_->count())
    {
        return {};
    }
    int c = cols(bounds_.w);
    if (c <= 0)
    {
        return {};
    }
    int row = idx / c;
    int col = idx % c;
    float x = bounds_.x + padding_.left + col * (cell_w_ + h_spacing_);
    float y =
        bounds_.y + padding_.top + row * (cell_h_ + v_spacing_) - scroll_y_;
    return {x, y, cell_w_, cell_h_};
}

Size GridView::measure(LayoutCtx&, Size constraints)
{
    return constraints;
}

void GridView::arrange(LayoutCtx&, Rect bounds)
{
    bounds_ = bounds;
    clamp_scroll();
}

float GridView::content_height() const
{
    if (!adapter_)
    {
        return 0;
    }
    int c = cols(bounds_.w);
    int r = rows(static_cast<int>(adapter_->count()), c);
    return padding_.vertical() +
           (r > 0 ? r * cell_h_ + (r - 1) * v_spacing_ : 0);
}

void GridView::paint(PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);
    if (!adapter_ || adapter_->count() == 0)
    {
        return;
    }

    ctx.canvas.push_clip_rect(bounds_);

    int c = cols(bounds_.w);
    int total = static_cast<int>(adapter_->count());
    float row_h = cell_h_ + v_spacing_;
    float origin_x = bounds_.x + padding_.left;
    float origin_y = bounds_.y + padding_.top - scroll_y_;

    // Skip rows above the viewport.
    int first_row = 0;
    if (row_h > 0 && origin_y < bounds_.y)
    {
        first_row = static_cast<int>((bounds_.y - origin_y) / row_h);
        if (first_row < 0)
        {
            first_row = 0;
        }
    }

    for (int row = first_row; row * c < total; ++row)
    {
        float row_top = origin_y + row * row_h;
        if (row_top >= bounds_.y + bounds_.h)
        {
            break;
        }
        for (int col = 0; col < c; ++col)
        {
            int idx = row * c + col;
            if (idx >= total)
            {
                break;
            }
            Rect cell_bounds{origin_x + col * (cell_w_ + h_spacing_), row_top,
                             cell_w_, cell_h_};
            bool selected = (idx == selected_index_);
            bool hovered = (idx == hovered_index_);
            adapter_->paint_cell(static_cast<std::size_t>(idx), ctx,
                                 cell_bounds, selected, hovered);
        }
    }

    ctx.canvas.pop_clip();

    // Scrollbar overlay — only when content overflows the viewport.
    paint_scrollbar(ctx);
}

bool GridView::on_wheel(Point /*local*/, float /*dx*/, float dy)
{
    if (!adapter_)
    {
        return false;
    }
    float prev = scroll_y_;
    scroll_y_ += dy;
    clamp_scroll();
    return scroll_y_ != prev;
}

bool GridView::on_pointer_down(Point local)
{
    if (scrollbar_on_pointer_down(local))
    {
        return true;
    }
    if (!adapter_)
    {
        return false;
    }
    int idx = index_at(local);
    if (idx == kInvalidIndex)
    {
        return false;
    }
    if (!adapter_->is_selectable(idx))
    {
        return false;
    }
    pressed_index_ = idx;
    return true;
}

void GridView::on_pointer_drag(Point local)
{
    scrollbar_on_pointer_drag(local);
}

void GridView::on_pointer_up(Point local, bool inside_self)
{
    if (scrollbar_on_pointer_up())
    {
        return;
    }
    int idx = inside_self ? index_at(local) : kInvalidIndex;
    if (pressed_index_ != kInvalidIndex && pressed_index_ == idx)
    {
        selected_index_ = idx;
        if (on_cell_clicked)
        {
            on_cell_clicked(idx);
        }
    }
    pressed_index_ = kInvalidIndex;
}

// ─────────────────────────────────────────────────────────────────────────

void ListView::update_hover(Point local)
{
    int idx = index_at(local);
    if (idx == hovered_index_)
    {
        return;
    }
    hovered_index_ = idx;
}

bool ListView::on_pointer_move(Point local)
{
    update_hover(local);
    return true;
}

void ListView::on_pointer_leave()
{
    hovered_index_ = kInvalidIndex;
}

} // namespace tk
