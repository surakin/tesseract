#include "tab_bar.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
namespace tk {

// ── Construction ───────────────────────────────────────────────────────────

TabBar::TabBar()
{
    set_visible(false);  // hidden until 2+ tabs exist
}

// ── Public mutators ────────────────────────────────────────────────────────

void TabBar::add_tab(std::string room_id, std::string display_name,
                     const Image* avatar)
{
    TabItem item;
    item.room_id      = std::move(room_id);
    item.display_name = std::move(display_name);
    item.avatar       = avatar;
    items_.push_back(std::move(item));
    if (static_cast<int>(items_.size()) > 1)
        set_visible(true);
    if (active_idx_ < 0)
        active_idx_ = 0;
}

void TabBar::remove_tab(const std::string& room_id)
{
    auto it = std::find_if(items_.begin(), items_.end(),
        [&](const TabItem& t) { return t.room_id == room_id; });
    if (it == items_.end()) return;
    int idx = static_cast<int>(it - items_.begin());
    items_.erase(it);
    // Clamp active_idx_ after removal.
    if (active_idx_ >= static_cast<int>(items_.size()))
        active_idx_ = static_cast<int>(items_.size()) - 1;
    if (idx < active_idx_)
        --active_idx_;
    if (static_cast<int>(items_.size()) <= 1)
        set_visible(false);
    scroll_x_ = 0.f;
}

void TabBar::set_active(const std::string& room_id)
{
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (items_[i].room_id == room_id) {
            active_idx_ = i;
            return;
        }
    }
}

void TabBar::update_tab(const std::string& room_id,
                        std::string display_name, const Image* avatar)
{
    for (auto& t : items_) {
        if (t.room_id == room_id) {
            t.display_name = std::move(display_name);
            t.avatar       = avatar;
            t.layout.reset();  // force TextLayout rebuild
            return;
        }
    }
}

void TabBar::clear()
{
    items_.clear();
    active_idx_  = -1;
    scroll_x_    = 0.f;
    total_width_ = 0.f;
    set_visible(false);
}

// ── Geometry helpers ───────────────────────────────────────────────────────

Rect TabBar::close_scroll_rect_(int i) const
{
    const auto& t = items_[i];
    float cx = t.x + t.width - kPadOuter - kCloseSz;
    float cy = bounds_.y + (kHeight - kCloseSz) * 0.5f;
    return { cx, cy, kCloseSz, kCloseSz };
}

int TabBar::tab_at_(float scroll_local) const
{
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (scroll_local >= items_[i].x &&
            scroll_local <  items_[i].x + items_[i].width)
            return i;
    }
    return -1;
}

void TabBar::clamp_scroll_()
{
    float max_s = std::max(0.f, total_width_ - bounds_.w);
    if (scroll_x_ < 0.f)    scroll_x_ = 0.f;
    if (scroll_x_ > max_s)  scroll_x_ = max_s;
}

// ── Layout ─────────────────────────────────────────────────────────────────

template <typename Ctx>
void TabBar::ensure_layout_(Ctx& ctx, TabItem& t, float max_w)
{
    if (t.layout
            && std::abs(t.layout_max_w - max_w) < 0.5f
            && t.layout_name == t.display_name)
        return;
    TextStyle style;
    style.role      = FontRole::SidebarName;
    style.trim      = TextTrim::Ellipsis;
    style.max_width = max_w;
    t.layout      = ctx.factory.build_text(t.display_name, style);
    t.layout_max_w = max_w;
    t.layout_name  = t.display_name;
}

Size TabBar::measure(LayoutCtx&, Size constraints)
{
    return { constraints.w, kHeight };
}

void TabBar::arrange(LayoutCtx& ctx, Rect bounds)
{
    bounds_ = bounds;
    if (items_.empty()) {
        total_width_ = 0.f;
        return;
    }

    const int n = static_cast<int>(items_.size());

    // All tabs the same width: divide bar evenly, clamp to [kTabMin, kTabMax].
    const float tab_w = std::clamp(bounds.w / static_cast<float>(n),
                                   kTabMin, kTabMax);

    // Position tabs and build name layouts.
    float x = 0.f;
    for (int i = 0; i < n; ++i) {
        items_[i].x     = x;
        items_[i].width = tab_w;
        ensure_layout_(ctx, items_[i], tab_w - kChrome);
        x += tab_w;
    }
    total_width_ = x;
    clamp_scroll_();

    // Build (or reuse) the "×" close-button layout.
    if (!close_layout_) {
        TextStyle xs;
        xs.role = FontRole::Body;
        close_layout_ = ctx.factory.build_text("\xc3\x97", xs);  // UTF-8 "×"
    }
}

// ── Paint ──────────────────────────────────────────────────────────────────

void TabBar::paint(PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    Canvas&     c   = ctx.canvas;

    // Tab bar background.
    c.fill_rect(bounds_, pal.chrome_bg);
    // 1-px bottom border.
    c.fill_rect({ bounds_.x, bounds_.y + bounds_.h - 1.f,
                  bounds_.w, 1.f }, pal.separator);

    c.push_clip_rect(bounds_);

    const bool show_close = (static_cast<int>(items_.size()) > 1);

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        auto& t = items_[i];

        // Tab bounds in widget (world) space.
        float tx = bounds_.x + t.x - scroll_x_;

        // Skip entirely outside clip region.
        if (tx + t.width <= bounds_.x || tx >= bounds_.x + bounds_.w)
            continue;

        Rect tab_rect{ tx, bounds_.y, t.width, bounds_.h - 1.f };

        // Background.
        const bool is_active = (i == active_idx_);
        Color bg = is_active    ? pal.sidebar_selected
                 : t.hovered    ? pal.sidebar_hover
                                : pal.chrome_bg;
        c.fill_rounded_rect(tab_rect, kRadius, bg);

        // Avatar or initials fallback.
        Point av_centre{
            tx + kPadOuter + kAvatarSz * 0.5f,
            bounds_.y + kHeight * 0.5f
        };
        if (t.avatar) {
            c.draw_circle_image(*t.avatar, av_centre, kAvatarSz);
        } else {
            c.draw_initials_circle(t.display_name, av_centre, kAvatarSz,
                                   pal.sidebar_selected, pal.text_primary);
        }

        // Display name.
        if (t.layout) {
            // ensure_layout_ is not callable from paint without ctx.factory — but
            // PaintCtx carries factory, so rebuild if somehow stale.
            float name_w = t.width - kChrome;
            ensure_layout_(ctx, t, name_w);

            Size name_sz = t.layout->measure();
            Point name_origin{
                tx + kPadOuter + kAvatarSz + kPadInner,
                bounds_.y + (kHeight - name_sz.h) * 0.5f
            };
            c.draw_text(*t.layout, name_origin, pal.text_primary);
        }

        // × close button.
        if (show_close && close_layout_) {
            Rect cr = close_scroll_rect_(i);
            cr.x = tx + t.width - kPadOuter - kCloseSz;
            Color close_col = t.close_hovered ? pal.text_primary
                                              : pal.text_secondary;
            Size x_sz = close_layout_->measure();
            Point x_origin{
                cr.x + (kCloseSz - x_sz.w) * 0.5f,
                cr.y + (kCloseSz - x_sz.h) * 0.5f
            };
            c.draw_text(*close_layout_, x_origin, close_col);
        }
    }

    c.pop_clip();
}

// ── Pointer events ─────────────────────────────────────────────────────────

bool TabBar::on_pointer_down(Point local)
{
    float sl = local.x + scroll_x_;
    int idx = tab_at_(sl);
    if (idx < 0) return false;
    pressed_idx_   = idx;
    // Determine if the press is inside the × rect.
    pressed_close_ = false;
    if (static_cast<int>(items_.size()) > 1) {
        Rect cr = close_scroll_rect_(idx);
        // cr.x is in scroll-space; convert to widget-local:
        float widget_close_x = cr.x - scroll_x_;
        pressed_close_ = (local.x >= widget_close_x
                       && local.x <  widget_close_x + kCloseSz
                       && local.y >= cr.y - bounds_.y
                       && local.y <  cr.y - bounds_.y + kCloseSz);
    }
    return true;
}

void TabBar::on_pointer_up(Point local, bool inside_self)
{
    if (pressed_idx_ < 0) return;
    float sl = local.x + scroll_x_;
    int idx = inside_self ? tab_at_(sl) : -1;
    if (idx == pressed_idx_) {
        if (pressed_close_) {
            // Verify pointer is still in the close rect.
            Rect cr = close_scroll_rect_(idx);
            float widget_close_x = cr.x - scroll_x_;
            bool still_on_close =
                (local.x >= widget_close_x
              && local.x <  widget_close_x + kCloseSz
              && local.y >= cr.y - bounds_.y
              && local.y <  cr.y - bounds_.y + kCloseSz);
            if (still_on_close && on_tab_closed)
                on_tab_closed(items_[idx].room_id);
        } else {
            if (on_tab_selected)
                on_tab_selected(items_[idx].room_id);
        }
    }
    pressed_idx_   = -1;
    pressed_close_ = false;
}

bool TabBar::on_pointer_move(Point local)
{
    float sl = local.x + scroll_x_;
    int hover_idx = tab_at_(sl);
    const bool show_close = (static_cast<int>(items_.size()) > 1);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        items_[i].hovered       = (i == hover_idx);
        items_[i].close_hovered = false;
        if (i == hover_idx && show_close) {
            Rect cr = close_scroll_rect_(i);
            float widget_close_x = cr.x - scroll_x_;
            items_[i].close_hovered =
                (local.x >= widget_close_x
              && local.x <  widget_close_x + kCloseSz
              && local.y >= cr.y - bounds_.y
              && local.y <  cr.y - bounds_.y + kCloseSz);
        }
    }
    return true;
}

void TabBar::on_pointer_leave()
{
    for (auto& t : items_) {
        t.hovered       = false;
        t.close_hovered = false;
    }
}

bool TabBar::on_wheel(Point /*local*/, float dx, float dy)
{
    // Prefer horizontal delta; remap vertical for scroll-wheel mice.
    float delta = (std::abs(dx) > std::abs(dy)) ? dx : dy;
    scroll_x_ += delta * 40.f;
    clamp_scroll_();
    return true;
}

// Explicit instantiations so the template body doesn't need to live in the header.
template void TabBar::ensure_layout_<LayoutCtx>(LayoutCtx&, TabItem&, float);
template void TabBar::ensure_layout_<PaintCtx> (PaintCtx&,  TabItem&, float);

} // namespace tk
