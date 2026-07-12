#include "side_tab_view.h"

#include <algorithm>

namespace tk
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SideTabView::SideTabView() = default;

void SideTabView::add_tab(std::string label, std::unique_ptr<Widget> content)
{
    Tab t;
    t.label = std::move(label);
    t.content = add_child(std::move(content));

    const bool first = tabs_.empty();
    tabs_.push_back(std::move(t));

    // Hide all content initially; the auto-select below will show the first tab.
    tabs_.back().content->set_visible(false);

    if (first)
    {
        // Auto-select the first tab without firing the callback (no prior
        // listener could have been set yet, and the index goes from -1 → 0
        // which is the natural construction order).
        selected_idx_ = 0;
        tabs_[0].content->set_visible(true);
    }
}

void SideTabView::add_bottom_tab(std::string label,
                                 std::unique_ptr<Widget> content)
{
    Tab t;
    t.label = std::move(label);
    t.content = add_child(std::move(content));
    t.bottom = true;

    const bool first = tabs_.empty();
    tabs_.push_back(std::move(t));
    tabs_.back().content->set_visible(false);

    // A bottom tab can be the very first tab added; in that case auto-select
    // it so SideTabView always has something visible.
    if (first)
    {
        selected_idx_ = 0;
        tabs_[0].content->set_visible(true);
    }
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

bool SideTabView::tab_visible(int idx) const
{
    if (idx < 0 || idx >= static_cast<int>(tabs_.size()))
    {
        return false;
    }
    return tabs_[idx].visible;
}

void SideTabView::set_tab_visible(int idx, bool visible)
{
    if (idx < 0 || idx >= static_cast<int>(tabs_.size()))
    {
        return;
    }
    if (tabs_[idx].visible == visible)
    {
        return;
    }
    tabs_[idx].visible = visible;

    if (!visible)
    {
        tabs_[idx].content->set_visible(false);
        if (idx == selected_idx_)
        {
            selected_idx_ = -1;
            for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
            {
                if (tabs_[i].visible)
                {
                    select(i);
                    break;
                }
            }
        }
    }
    else if (selected_idx_ < 0)
    {
        // Every tab had been hidden; adopt this one now that it's back.
        select(idx);
    }
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void SideTabView::select(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(tabs_.size()))
    {
        return;
    }
    if (idx == selected_idx_)
    {
        return;
    }

    // Hide old content.
    if (selected_idx_ >= 0 && selected_idx_ < static_cast<int>(tabs_.size()))
    {
        tabs_[selected_idx_].content->set_visible(false);
    }

    selected_idx_ = idx;
    tabs_[selected_idx_].content->set_visible(true);

    if (on_tab_selected)
    {
        on_tab_selected(selected_idx_);
    }
}

// ---------------------------------------------------------------------------
// Layout — measure
// ---------------------------------------------------------------------------

Size SideTabView::measure(LayoutCtx& ctx, Size constraints)
{
    // Left column is fixed width; right content takes the remaining space.
    const float avail_w = constraints.w > 0 ? constraints.w : kSidebarWidth;
    const float avail_h = constraints.h > 0 ? constraints.h : 0.0f;

    const float content_w = std::max(0.0f, avail_w - kSidebarWidth);

    // Measure the selected content widget so we report a sensible height.
    float content_h = 0.0f;
    if (selected_idx_ >= 0 && selected_idx_ < static_cast<int>(tabs_.size()))
    {
        Widget* w = tabs_[selected_idx_].content;
        Size cs = w->measure(ctx, {content_w, avail_h});
        content_h = cs.h;
    }

    // Sidebar height is at least the number of visible tabs × tab height.
    const float sidebar_h =
        static_cast<float>(num_visible_top_tabs_() + num_visible_bottom_tabs_()) *
        kTabHeight;
    const float h = avail_h > 0 ? avail_h : std::max(sidebar_h, content_h);

    return {avail_w, h};
}

// ---------------------------------------------------------------------------
// Layout — arrange
// ---------------------------------------------------------------------------

void SideTabView::arrange(LayoutCtx& ctx, Rect bounds)
{
    bounds_ = bounds;

    const float content_x = bounds.x + kSidebarWidth;
    const float content_w = std::max(0.0f, bounds.w - kSidebarWidth);

    // Ensure TextLayout caches are ready for the sidebar labels.
    const float label_max_w = kSidebarWidth - kTabHPad * 2 - kTabInset * 2;
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    {
        ensure_layout_(ctx, i, label_max_w);
    }

    // Arrange every content widget into the right pane, but only the
    // selected one is visible (set_visible controls paint + hit-test).
    Rect content_bounds{content_x, bounds.y, content_w, bounds.h};
    for (auto& t : tabs_)
    {
        t.content->arrange(ctx, content_bounds);
    }
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void SideTabView::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // Sidebar background.
    Rect sidebar{bounds_.x, bounds_.y, kSidebarWidth, bounds_.h};
    ctx.canvas.fill_rect(sidebar, pal.sidebar_bg);

    const float btn_x = bounds_.x + kTabInset;
    const float btn_w = kSidebarWidth - kTabInset * 2;
    const float label_w = btn_w - kTabHPad * 2;

    const int n_vis_top = num_visible_top_tabs_();
    const int n_vis_bot = num_visible_bottom_tabs_();
    const int n_total = static_cast<int>(tabs_.size());

    auto button_y = [&](int i) -> float {
        const int slot = visible_slot_(i);
        if (!tabs_[i].bottom)
        {
            return bounds_.y + static_cast<float>(slot) * kTabHeight;
        }
        // Bottom group: lay out from the bottom of the sidebar upwards.
        const float group_top =
            bounds_.y + bounds_.h - static_cast<float>(n_vis_bot) * kTabHeight;
        return group_top + static_cast<float>(slot) * kTabHeight;
    };

    for (int i = 0; i < n_total; ++i)
    {
        auto& t = tabs_[i];
        if (!t.visible)
        {
            continue;
        }

        Rect btn{btn_x,
                 button_y(i) + kTabVPad * 0.5f,
                 btn_w, kTabHeight - kTabVPad};

        // Choose fill colour based on state.
        Color fill = Color::rgba(0, 0, 0, 0); // transparent default
        if (i == selected_idx_)
        {
            fill = pal.sidebar_selected;
        }
        else if (t.hovered)
        {
            fill = pal.sidebar_hover;
        }

        if (fill.a > 0)
        {
            ctx.canvas.fill_rounded_rect(btn, kTabRadius, fill);
        }

        // Draw label text.
        ensure_layout_(ctx, i, label_w);
        if (t.layout)
        {
            Size ts = t.layout->measure();
            float tx = btn.x + kTabHPad;
            float ty = btn.y + (btn.h - ts.h) * 0.5f;
            Color tc =
                (i == selected_idx_) ? pal.text_primary : pal.text_secondary;
            ctx.canvas.draw_text(*t.layout, {tx, ty}, tc);
        }
    }

    // Separator line above the bottom-tab group (when both groups exist and
    // there is real space between them).
    if (n_vis_bot > 0 && n_vis_top > 0)
    {
        const float group_top =
            bounds_.y + bounds_.h - static_cast<float>(n_vis_bot) * kTabHeight;
        const float top_stack_bottom =
            bounds_.y + static_cast<float>(n_vis_top) * kTabHeight;
        if (group_top > top_stack_bottom + kTabVPad)
        {
            Rect divider{bounds_.x + kTabInset,
                         group_top - kTabVPad * 0.5f - 0.5f,
                         kSidebarWidth - kTabInset * 2,
                         1.0f};
            ctx.canvas.fill_rect(divider, pal.separator);
        }
    }

    // Vertical separator between sidebar and content area.
    Rect sep{bounds_.x + kSidebarWidth - 1.0f, bounds_.y, 1.0f, bounds_.h};
    ctx.canvas.fill_rect(sep, pal.separator);

    // Paint the selected content widget.
    if (selected_idx_ >= 0 && selected_idx_ < static_cast<int>(tabs_.size()))
    {
        Widget* w = tabs_[selected_idx_].content;
        if (w->visible())
        {
            w->paint(ctx);
        }
    }
}

// ---------------------------------------------------------------------------
// Pointer input
// ---------------------------------------------------------------------------

bool SideTabView::on_pointer_down(Point local)
{
    // Only claim presses within the sidebar column.
    if (local.x < 0 || local.x >= kSidebarWidth)
    {
        return false;
    }
    int idx = tab_at_y(local.y);
    if (idx < 0)
    {
        return false;
    }
    pressed_idx_ = idx;
    return true;
}

void SideTabView::on_pointer_up(Point local, bool inside_self)
{
    const int pressed = pressed_idx_;
    pressed_idx_ = -1;
    if (!inside_self)
    {
        return;
    }
    // Only act if the release is still over the sidebar.
    if (local.x < 0 || local.x >= kSidebarWidth)
    {
        return;
    }
    int idx = tab_at_y(local.y);
    if (idx >= 0 && idx == pressed)
    {
        select(idx);
    }
}

bool SideTabView::on_pointer_move(Point local)
{
    const int hit =
        (local.x >= 0 && local.x < kSidebarWidth) ? tab_at_y(local.y) : -1;
    if (hit == hovered_idx_)
    {
        return false;
    }
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    {
        tabs_[i].hovered = (i == hit);
    }
    hovered_idx_ = hit;
    return true;
}

void SideTabView::on_pointer_leave()
{
    for (auto& t : tabs_)
    {
        t.hovered = false;
    }
    hovered_idx_ = -1;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int SideTabView::tab_at_y(float y) const
{
    // y is already widget-local (pointer coordinates are always local in tk).
    if (y < 0)
    {
        return -1;
    }

    const int n_vis_top = num_visible_top_tabs_();
    const int n_vis_bot = num_visible_bottom_tabs_();

    // Top group: occupies [0, n_vis_top * kTabHeight).
    const float top_extent = static_cast<float>(n_vis_top) * kTabHeight;
    if (y < top_extent)
    {
        const int slot = static_cast<int>(y / kTabHeight);
        return tab_at_visible_slot_(slot, /*bottom=*/false);
    }

    // Bottom group: anchored at the bottom of the column.
    if (n_vis_bot > 0)
    {
        const float group_top =
            bounds_.h - static_cast<float>(n_vis_bot) * kTabHeight;
        if (y >= group_top && y < bounds_.h)
        {
            const int slot = static_cast<int>((y - group_top) / kTabHeight);
            return tab_at_visible_slot_(slot, /*bottom=*/true);
        }
    }

    return -1;
}

int SideTabView::num_top_tabs_() const
{
    int n = 0;
    for (const auto& t : tabs_)
    {
        if (t.bottom)
        {
            break;
        }
        ++n;
    }
    return n;
}

int SideTabView::num_bottom_tabs_() const
{
    return static_cast<int>(tabs_.size()) - num_top_tabs_();
}

int SideTabView::num_visible_top_tabs_() const
{
    int n = 0;
    for (const auto& t : tabs_)
    {
        if (t.bottom)
        {
            break;
        }
        if (t.visible)
        {
            ++n;
        }
    }
    return n;
}

int SideTabView::num_visible_bottom_tabs_() const
{
    int n = 0;
    for (const auto& t : tabs_)
    {
        if (t.bottom && t.visible)
        {
            ++n;
        }
    }
    return n;
}

int SideTabView::visible_slot_(int idx) const
{
    if (idx < 0 || idx >= static_cast<int>(tabs_.size()) || !tabs_[idx].visible)
    {
        return -1;
    }
    const bool bottom_group = tabs_[idx].bottom;
    int slot = 0;
    for (int j = 0; j < idx; ++j)
    {
        if (tabs_[j].bottom == bottom_group && tabs_[j].visible)
        {
            ++slot;
        }
    }
    return slot;
}

int SideTabView::tab_at_visible_slot_(int slot, bool bottom) const
{
    int seen = 0;
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    {
        if (tabs_[i].bottom != bottom || !tabs_[i].visible)
        {
            continue;
        }
        if (seen == slot)
        {
            return i;
        }
        ++seen;
    }
    return -1;
}

template <typename Ctx>
void SideTabView::ensure_layout_(Ctx& ctx, int i, float max_w)
{
    auto& t = tabs_[i];
    if (t.layout && t.layout_max_w == max_w)
    {
        return;
    }
    TextStyle st{};
    st.role = FontRole::UiSemibold;
    st.halign = TextHAlign::Leading;
    st.trim = TextTrim::Ellipsis;
    st.max_width = max_w;
    t.layout = ctx.factory.build_text(t.label, st);
    t.layout_max_w = max_w;
}

template void SideTabView::ensure_layout_(LayoutCtx&, int, float);
template void SideTabView::ensure_layout_(PaintCtx&, int, float);

} // namespace tk
