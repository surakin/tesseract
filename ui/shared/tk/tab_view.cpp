#include "tab_view.h"

#include "theme.h"

#include <algorithm>
#include <utility>

namespace tk
{

void TabView::set_items(std::vector<std::string> labels)
{
    items_.clear();
    items_.reserve(labels.size());
    for (auto& label : labels)
    {
        items_.push_back(Item{std::move(label), {}, nullptr});
    }
    const int n = static_cast<int>(items_.size());
    if (n == 0)
    {
        selected_idx_ = 0;
    }
    else if (selected_idx_ >= n)
    {
        selected_idx_ = n - 1;
    }
    else if (selected_idx_ < 0)
    {
        selected_idx_ = 0;
    }
}

void TabView::set_selected_index(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(items_.size()))
        return;
    if (idx == selected_idx_)
        return;
    selected_idx_ = idx;
    if (on_selected)
        on_selected(selected_idx_);
}

Size TabView::measure(LayoutCtx&, Size constraints)
{
    return {constraints.w, kMinH};
}

void TabView::arrange(LayoutCtx& ctx, Rect bounds)
{
    bounds_ = bounds;

    const int n = static_cast<int>(items_.size());
    if (n == 0)
        return;

    // Measure every label to find the widest, so all segments share one
    // natural width — mirrors AppearanceSection::ThemePicker's approach
    // (appropriate for a small fixed set of short labels, unlike TabBar's
    // clamped-per-item width for many room tabs).
    float max_text_w = 0.0f;
    for (auto& item : items_)
    {
        TextStyle st;
        st.role = FontRole::Body;
        st.max_width = -1.0f;
        if (auto lo = ctx.factory.build_text(item.label, st))
            max_text_w = std::max(max_text_w, lo->measure().w);
    }
    const float content_seg_w = max_text_w + kHPad * 2.0f;

    // Segments stretch to fill the incoming bounds when it's wider than
    // the content-driven minimum — AddRoomView's header always wants the
    // two segments to fill the full card width; a segmented header
    // spanning a fixed-width container is expected to fill it.
    const float total_gap = kGap * static_cast<float>(n - 1);
    const float fill_seg_w = (bounds.w - total_gap) / static_cast<float>(n);
    const float seg_w = std::max(content_seg_w, fill_seg_w);

    float x = bounds.x;
    for (auto& item : items_)
    {
        item.bounds = {x, bounds.y, seg_w, bounds.h};
        item.layout.reset();
        x += seg_w + kGap;
    }
}

void TabView::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
    {
        auto& item = items_[i];
        const bool is_selected = (i == selected_idx_);
        const bool is_hovered = (i == hovered_idx_);
        const bool is_pressed = (i == pressed_idx_);

        // No filled/bordered pill — that's what made this read as a button
        // group instead of a tab strip. The active tab is distinguished
        // purely by an accent underline + accent label colour (below); an
        // unselected tab only gets a faint hover/press wash, no persistent
        // background, so nothing ever looks "pressed in" except via the
        // underline moving.
        if (!is_selected)
        {
            const Color wash = is_pressed ? pal.subtle_pressed
                                : is_hovered ? pal.subtle_hover
                                             : Color::rgba(0, 0, 0, 0);
            if (wash.a != 0)
                ctx.canvas.fill_rect(item.bounds, wash);
        }

        if (!item.layout)
        {
            // halign stays Leading (the TextStyle default), NOT Center —
            // Center here would hit the Qt/Direct2D canvas backends'
            // unset-max_width "8192px sentinel" bug (they'd centre the
            // glyph inside an effectively-unbounded box, landing it
            // thousands of pixels outside item.bounds). Centering is done
            // manually below via the measured size instead, mirroring
            // tk::Button::paint()'s identical workaround.
            TextStyle st;
            st.role = FontRole::UiSemibold;
            st.max_width = -1.0f;
            item.layout = ctx.factory.build_text(item.label, st);
        }

        if (item.layout)
        {
            const Size sz = item.layout->measure();
            const float tx = item.bounds.x + (item.bounds.w - sz.w) * 0.5f;
            const float ty = item.bounds.y + (item.bounds.h - sz.h) * 0.5f;
            const Color ink = is_selected ? pal.accent
                               : is_hovered ? pal.text_primary
                                            : pal.text_secondary;
            ctx.canvas.draw_text(*item.layout, {tx, ty}, ink);
        }

        if (is_selected)
        {
            const Rect underline{item.bounds.x,
                                 item.bounds.y + item.bounds.h - kUnderlineH,
                                 item.bounds.w, kUnderlineH};
            ctx.canvas.fill_rect(underline, pal.accent);
        }
    }
}

int TabView::hit_test_(Point local) const
{
    const Point world{local.x + bounds_.x, local.y + bounds_.y};
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
    {
        const Rect& r = items_[i].bounds;
        if (world.x >= r.x && world.x < r.x + r.w && world.y >= r.y &&
            world.y < r.y + r.h)
            return i;
    }
    return -1;
}

bool TabView::on_pointer_down(Point local)
{
    const int idx = hit_test_(local);
    if (idx < 0)
        return false;
    pressed_idx_ = idx;
    return true;
}

void TabView::on_pointer_up(Point local, bool inside_self)
{
    const int was_pressed = pressed_idx_;
    pressed_idx_ = -1;
    if (!inside_self || was_pressed < 0)
        return;
    if (hit_test_(local) != was_pressed)
        return;
    set_selected_index(was_pressed);
}

bool TabView::on_pointer_move(Point local)
{
    const int prev = hovered_idx_;
    hovered_idx_ = hit_test_(local);
    return hovered_idx_ != prev;
}

void TabView::on_pointer_leave()
{
    hovered_idx_ = -1;
    pressed_idx_ = -1;
}

bool TabView::on_key_down(const KeyEvent& e)
{
    if (!has_focus())
        return false;
    if (e.key != Key::Left && e.key != Key::Right)
        return false;
    if (e.ctrl || e.alt || e.meta)
        return false;
    const int n = static_cast<int>(items_.size());
    if (n == 0)
        return false;
    const int delta = (e.key == Key::Right) ? 1 : -1;
    const int next = (selected_idx_ + delta + n) % n;
    set_selected_index(next);
    return true;
}

void TabView::paint_own_focus_ring(PaintCtx& ctx)
{
    if (items_.empty())
    {
        paint_focus_ring(ctx, bounds());
        return;
    }
    const Rect& first = items_.front().bounds;
    const Rect& last = items_.back().bounds;
    const Rect ring{
        first.x - kFocusRingInset,
        first.y - kFocusRingInset,
        (last.x + last.w - first.x) + kFocusRingInset * 2.0f,
        first.h + kFocusRingInset * 2.0f,
    };
    paint_focus_ring(ctx, ring);
}

} // namespace tk
