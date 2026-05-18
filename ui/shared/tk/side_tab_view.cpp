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

    // Sidebar height is at least the number of tabs × tab height.
    const float sidebar_h = static_cast<float>(tabs_.size()) * kTabHeight;
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

    // Draw each tab button.
    const float btn_x = bounds_.x + kTabInset;
    const float btn_w = kSidebarWidth - kTabInset * 2;
    const float label_w = btn_w - kTabHPad * 2;

    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    {
        auto& t = tabs_[i];

        Rect btn{btn_x,
                 bounds_.y + static_cast<float>(i) * kTabHeight +
                     kTabVPad * 0.5f,
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
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    {
        tabs_[i].hovered = (i == hit);
    }
    return true;
}

void SideTabView::on_pointer_leave()
{
    for (auto& t : tabs_)
    {
        t.hovered = false;
    }
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
    int idx = static_cast<int>(y / kTabHeight);
    if (idx < 0 || idx >= static_cast<int>(tabs_.size()))
    {
        return -1;
    }
    return idx;
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
