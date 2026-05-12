#include "list_view.h"

#include <algorithm>
#include <cmath>

namespace tk {

namespace {

constexpr float kScrollbarWidth   = 6.0f;
constexpr float kScrollbarRadius  = 3.0f;
constexpr float kScrollbarMinLen  = 24.0f;
constexpr float kScrollbarInset   = 2.0f;
constexpr int   kInvalidIndex     = -1;

} // namespace

void ListView::set_adapter(ListAdapter* adapter) {
    adapter_ = adapter;
    heights_dirty_ = true;
    selected_index_ = kInvalidIndex;
    hovered_index_  = kInvalidIndex;
    pressed_index_  = kInvalidIndex;
    scroll_y_ = 0;
}

void ListView::invalidate_data() {
    heights_dirty_ = true;
}

void ListView::set_selected_index(int idx) {
    if (idx == selected_index_) return;
    selected_index_ = idx;
}

void ListView::scroll_to_top()    { scroll_y_ = 0; stick_to_bottom_ = false; }
void ListView::scroll_to_bottom() { stick_to_bottom_ = true; clamp_scroll(); }

void ListView::scroll_to_index(int idx, bool align_top) {
    if (!adapter_ || idx < 0 ||
        static_cast<std::size_t>(idx) >= adapter_->count()) return;
    if (row_offsets_.empty()) return;
    if (align_top) {
        scroll_y_ = row_offsets_[idx];
    } else {
        // Bring into view: scroll only enough that the row sits inside
        // [scroll_y, scroll_y + viewport_h].
        float top = row_offsets_[idx];
        float bot = row_offsets_[idx + 1];
        float viewport = bounds_.h;
        if (top < scroll_y_) scroll_y_ = top;
        else if (bot > scroll_y_ + viewport) scroll_y_ = bot - viewport;
    }
    stick_to_bottom_ = false;
    clamp_scroll();
}

float ListView::content_height() const {
    return row_offsets_.empty() ? 0.0f : row_offsets_.back();
}

int ListView::index_at(Point local) const {
    if (!adapter_ || row_offsets_.empty()) return kInvalidIndex;
    float content_y = local.y + scroll_y_;
    if (content_y < 0 || content_y >= row_offsets_.back()) return kInvalidIndex;
    auto it = std::upper_bound(row_offsets_.begin(), row_offsets_.end(),
                                content_y);
    int idx = static_cast<int>(it - row_offsets_.begin()) - 1;
    if (idx < 0) idx = 0;
    if (static_cast<std::size_t>(idx) >= adapter_->count())
        idx = kInvalidIndex;
    return idx;
}

Size ListView::measure(LayoutCtx&, Size constraints) {
    return constraints;
}

void ListView::arrange(LayoutCtx& ctx, Rect bounds) {
    bounds_ = bounds;
    if (!adapter_) {
        row_heights_.clear();
        row_offsets_.clear();
        return;
    }
    if (heights_dirty_ || measured_width_ != bounds.w) {
        rebuild_heights(ctx, bounds.w);
    }
    if (stick_to_bottom_) {
        scroll_y_ = std::max(0.0f, content_height() - bounds.h);
    }
    clamp_scroll();
}

void ListView::rebuild_heights(LayoutCtx& ctx, float width) {
    measured_width_ = width;
    heights_dirty_  = false;
    std::size_t n = adapter_ ? adapter_->count() : 0;
    row_heights_.resize(n);
    row_offsets_.assign(n + 1, 0.0f);
    float cursor = 0;
    for (std::size_t i = 0; i < n; ++i) {
        row_heights_[i]  = adapter_->measure_row_height(i, ctx, width);
        row_offsets_[i]  = cursor;
        cursor          += row_heights_[i];
    }
    row_offsets_[n] = cursor;
}

void ListView::paint(PaintCtx& ctx) {
    // Background.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.sidebar_bg);

    if (!adapter_ || adapter_->count() == 0) return;

    ctx.canvas.push_clip_rect(bounds_);

    // Find the first row whose end is past the viewport top.
    float viewport_top = scroll_y_;
    float viewport_bot = scroll_y_ + bounds_.h;
    auto first_it = std::upper_bound(row_offsets_.begin(),
                                      row_offsets_.end(), viewport_top);
    std::size_t first = first_it == row_offsets_.begin()
        ? 0
        : static_cast<std::size_t>(first_it - row_offsets_.begin() - 1);

    for (std::size_t i = first; i < adapter_->count(); ++i) {
        float row_top    = row_offsets_[i];
        float row_bottom = row_offsets_[i + 1];
        if (row_top >= viewport_bot) break;
        Rect row_bounds{
            bounds_.x,
            bounds_.y + (row_top - scroll_y_),
            bounds_.w,
            row_bottom - row_top
        };
        bool selected = (static_cast<int>(i) == selected_index_);
        bool hovered  = (static_cast<int>(i) == hovered_index_);
        adapter_->paint_row(i, ctx, row_bounds, selected, hovered);
    }

    ctx.canvas.pop_clip();

    // Scrollbar overlay — only if content overflows the viewport.
    float total = content_height();
    if (total > bounds_.h && bounds_.h > 0) {
        float track_h = bounds_.h - kScrollbarInset * 2;
        float thumb_h = std::max(kScrollbarMinLen,
                                  track_h * (bounds_.h / total));
        float thumb_y = bounds_.y + kScrollbarInset
                        + (track_h - thumb_h)
                            * (scroll_y_ / std::max(1.0f, total - bounds_.h));
        Rect thumb{
            bounds_.x + bounds_.w - kScrollbarWidth - kScrollbarInset,
            thumb_y,
            kScrollbarWidth,
            thumb_h
        };
        Color c = ctx.theme.palette.text_muted.with_alpha(128);
        ctx.canvas.fill_rounded_rect(thumb, kScrollbarRadius, c);
    }
}

bool ListView::on_wheel(Point /*local*/, float /*dx*/, float dy) {
    if (!adapter_ || adapter_->count() == 0) return false;
    float prev = scroll_y_;
    scroll_y_ += dy;
    stick_to_bottom_ = false;
    clamp_scroll();
    return scroll_y_ != prev;
}

bool ListView::on_pointer_down(Point local) {
    int idx = index_at(local);
    if (idx == kInvalidIndex) return false;
    if (!adapter_->is_selectable(idx)) return false;
    pressed_index_ = idx;
    return true;
}

void ListView::on_pointer_up(Point local, bool inside_self) {
    int idx = inside_self ? index_at(local) : kInvalidIndex;
    if (pressed_index_ != kInvalidIndex && pressed_index_ == idx) {
        selected_index_ = idx;
        if (on_row_clicked) on_row_clicked(idx);
    }
    pressed_index_ = kInvalidIndex;
}

void ListView::clamp_scroll() {
    float max_scroll = std::max(0.0f, content_height() - bounds_.h);
    if (scroll_y_ < 0)          scroll_y_ = 0;
    if (scroll_y_ > max_scroll) scroll_y_ = max_scroll;
}

// ─────────────────────────────────────────────────────────────────────────
//  GridView
// ─────────────────────────────────────────────────────────────────────────

void GridView::set_adapter(GridAdapter* adapter) {
    adapter_       = adapter;
    selected_index_ = kInvalidIndex;
    hovered_index_  = kInvalidIndex;
    pressed_index_  = kInvalidIndex;
    scroll_y_       = 0;
}

void GridView::set_cell_size(float w, float h) {
    cell_w_ = w; cell_h_ = h;
}

void GridView::set_spacing(float h_spacing, float v_spacing) {
    h_spacing_ = h_spacing; v_spacing_ = v_spacing;
}

void GridView::set_padding(Edges padding) { padding_ = padding; }

void GridView::set_selected_index(int idx) { selected_index_ = idx; }

void GridView::invalidate_data() { /* nothing cached — paint reads on demand */ }

int GridView::cols(float available_w) const {
    if (cell_w_ <= 0) return 1;
    float inner = std::max(0.0f, available_w - padding_.horizontal());
    int c = static_cast<int>(
        (inner + h_spacing_) / (cell_w_ + h_spacing_));
    return std::max(1, c);
}

int GridView::rows(int n_cells, int cols_) const {
    if (cols_ <= 0 || n_cells <= 0) return 0;
    return (n_cells + cols_ - 1) / cols_;
}

int GridView::index_at(Point local) const {
    if (!adapter_ || adapter_->count() == 0) return kInvalidIndex;
    int c = cols(bounds_.w);
    float x = local.x - padding_.left;
    float y = local.y + scroll_y_ - padding_.top;
    if (x < 0 || y < 0) return kInvalidIndex;
    int col = static_cast<int>(x / (cell_w_ + h_spacing_));
    int row = static_cast<int>(y / (cell_h_ + v_spacing_));
    // Reject the gap between cells.
    float cell_local_x = x - col * (cell_w_ + h_spacing_);
    float cell_local_y = y - row * (cell_h_ + v_spacing_);
    if (cell_local_x >= cell_w_ || cell_local_y >= cell_h_) return kInvalidIndex;
    if (col >= c) return kInvalidIndex;
    int idx = row * c + col;
    if (idx < 0 || static_cast<std::size_t>(idx) >= adapter_->count())
        return kInvalidIndex;
    return idx;
}

Size GridView::measure(LayoutCtx&, Size constraints) { return constraints; }

void GridView::arrange(LayoutCtx&, Rect bounds) {
    bounds_ = bounds;
    clamp_scroll();
}

void GridView::clamp_scroll() {
    if (!adapter_) { scroll_y_ = 0; return; }
    int c = cols(bounds_.w);
    int r = rows(static_cast<int>(adapter_->count()), c);
    float content_h = padding_.vertical()
        + (r > 0 ? r * cell_h_ + (r - 1) * v_spacing_ : 0);
    float max_scroll = std::max(0.0f, content_h - bounds_.h);
    if (scroll_y_ < 0)          scroll_y_ = 0;
    if (scroll_y_ > max_scroll) scroll_y_ = max_scroll;
}

void GridView::paint(PaintCtx& ctx) {
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);
    if (!adapter_ || adapter_->count() == 0) return;

    ctx.canvas.push_clip_rect(bounds_);

    int c = cols(bounds_.w);
    int total = static_cast<int>(adapter_->count());
    float row_h = cell_h_ + v_spacing_;
    float origin_x = bounds_.x + padding_.left;
    float origin_y = bounds_.y + padding_.top - scroll_y_;

    // Skip rows above the viewport.
    int first_row = 0;
    if (row_h > 0 && origin_y < bounds_.y) {
        first_row = static_cast<int>(
            (bounds_.y - origin_y) / row_h);
        if (first_row < 0) first_row = 0;
    }

    for (int row = first_row; row * c < total; ++row) {
        float row_top = origin_y + row * row_h;
        if (row_top >= bounds_.y + bounds_.h) break;
        for (int col = 0; col < c; ++col) {
            int idx = row * c + col;
            if (idx >= total) break;
            Rect cell_bounds{
                origin_x + col * (cell_w_ + h_spacing_),
                row_top,
                cell_w_,
                cell_h_
            };
            bool selected = (idx == selected_index_);
            bool hovered  = (idx == hovered_index_);
            adapter_->paint_cell(static_cast<std::size_t>(idx), ctx,
                                  cell_bounds, selected, hovered);
        }
    }

    ctx.canvas.pop_clip();
}

bool GridView::on_wheel(Point /*local*/, float /*dx*/, float dy) {
    if (!adapter_) return false;
    float prev = scroll_y_;
    scroll_y_ += dy;
    clamp_scroll();
    return scroll_y_ != prev;
}

bool GridView::on_pointer_down(Point local) {
    int idx = index_at(local);
    if (idx == kInvalidIndex) return false;
    if (!adapter_->is_selectable(idx)) return false;
    pressed_index_ = idx;
    return true;
}

void GridView::on_pointer_up(Point local, bool inside_self) {
    int idx = inside_self ? index_at(local) : kInvalidIndex;
    if (pressed_index_ != kInvalidIndex && pressed_index_ == idx) {
        selected_index_ = idx;
        if (on_cell_clicked) on_cell_clicked(idx);
    }
    pressed_index_ = kInvalidIndex;
}

// ─────────────────────────────────────────────────────────────────────────

void ListView::update_hover(Point local) {
    int idx = index_at(local);
    if (idx == hovered_index_) return;
    hovered_index_ = idx;
}

} // namespace tk
