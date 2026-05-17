# Multi-Room Tab View Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a horizontal tab bar that lets users open multiple rooms simultaneously via Ctrl+click, with condensed room header in multi-tab mode and per-tab scroll/draft state.

**Architecture:** Single `RoomView` instance reused; `ShellBase` gains `tabs_` state and virtual scroll/draft hooks; new `tk::TabBar` widget lives in `ui/shared/tk/`; `RoomHeader` gains a condensed mode. Ctrl+click is detected at the shell level (platform modifier APIs) and routed to ShellBase tab methods that manage `tabs_` then call `on_tab_state_changed_ui_()`. All shells implement this virtual to sync the TabBar and call their existing room-switch method.

**Tech Stack:** C++20, the existing `tk` widget toolkit (Canvas, Widget, LayoutCtx/PaintCtx), ShellBase virtual dispatch.

**Branch:** `feat/multi-room-tabs` — all work lands in **one commit** at the end.

---

## File Map

| Action | Path | Purpose |
|---|---|---|
| Create | `ui/shared/tk/tab_bar.h` | `tk::TabBar` widget declaration |
| Create | `ui/shared/tk/tab_bar.cpp` | `tk::TabBar` widget implementation |
| Modify | `ui/shared/app/ShellBase.h` | `TabState`, `tabs_`, tab management methods, virtual hooks |
| Modify | `ui/shared/app/ShellBase.cpp` | Implement `tab_open_room`, `tab_select_room`, `tab_navigate_room`, `tab_close` |
| Modify | `ui/shared/views/MessageListView.h` | Add `scroll_to_offset(float)`, `scroll_fraction()` |
| Modify | `ui/shared/views/MessageListView.cpp` | Implement both |
| Modify | `ui/shared/views/RoomHeader.h` | Add `set_condensed(bool)`, `condensed_` field |
| Modify | `ui/shared/views/RoomHeader.cpp` | Implement condensed `measure()` and `arrange()` |
| Modify | `ui/shared/views/MainAppWidget.h` | Add `tab_bar_` member and accessor |
| Modify | `ui/shared/views/MainAppWidget.cpp` | Wire `TabBar` into constructor, `arrange()`, `paint()` |
| Modify | `ui/linux-qt/src/MainWindow.h` | Declare `on_tab_state_changed_ui_()` + virtual hooks |
| Modify | `ui/linux-qt/src/MainWindow.cpp` | Implement tab UI, Ctrl+click, scroll restore, `navigate_to_room` update |
| Modify | `ui/linux-gtk/src/MainWindow.h` | Declare `on_tab_state_changed_ui_()` + virtual hooks |
| Modify | `ui/linux-gtk/src/MainWindow.cpp` | Implement + wire Ctrl+click |
| Modify | `ui/windows/src/MainWindow.h` | Declare `on_tab_state_changed_ui_()` + virtual hooks |
| Modify | `ui/windows/src/MainWindow.cpp` | Implement + wire Ctrl+click |
| Modify | `ui/macos/src/MainWindowController.mm` | Implement in `MacShell` + wire Cmd+click |

---

## Task 1: Create a git branch

- [ ] **Step 1: Create branch**

```bash
git checkout -b feat/multi-room-tabs
```

---

## Task 2: Create `tk::TabBar` widget header

**Files:**
- Create: `ui/shared/tk/tab_bar.h`

- [ ] **Step 1: Write the header**

```cpp
// ui/shared/tk/tab_bar.h
#pragma once

// Horizontal room-tab bar. Shows one tab per open room (avatar + display name
// + close button). Self-hides when only one tab is present. Supports
// horizontal scroll when tabs overflow the available width.
//
// Consumers:
//   tab_bar->add_tab(room_id, display_name, avatar);
//   tab_bar->set_active(room_id);
//   tab_bar->on_tab_selected = [](const std::string& id) { … };
//   tab_bar->on_tab_closed   = [](const std::string& id) { … };

#include "widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk {

class TabBar : public Widget {
public:
    TabBar();
    ~TabBar() override = default;

    // Add a tab. If this is the first tab, the bar becomes visible.
    // The new tab is NOT automatically made active.
    void add_tab(std::string room_id, std::string display_name, Image avatar);

    // Remove the tab with the given room_id. If items drop to 1, hides itself.
    // No-op if room_id is not found.
    void remove_tab(const std::string& room_id);

    // Make the tab with room_id the active (highlighted) one. No-op if absent.
    void set_active(const std::string& room_id);

    // Update display_name and avatar for an existing tab. No-op if absent.
    void update_tab(const std::string& room_id,
                    std::string display_name, Image avatar);

    // Fires when the user clicks a tab body (not the × button).
    std::function<void(const std::string& room_id)> on_tab_selected;

    // Fires when the user clicks the × button on a tab.
    // Only wired when items_.size() > 1 (last tab cannot be closed).
    std::function<void(const std::string& room_id)> on_tab_closed;

    // tk::Widget overrides.
    Size    measure(LayoutCtx&, Size constraints) override;
    void    arrange(LayoutCtx&, Rect bounds)      override;
    void    paint  (PaintCtx&)                    override;
    bool    on_pointer_down(Point local)           override;
    void    on_pointer_up  (Point local, bool inside_self) override;
    void    on_pointer_move(Point local)           override;
    void    on_pointer_leave()                     override;
    bool    on_wheel(Point local, float dx, float dy) override;

private:
    static constexpr float kHeight    = 40.0f;
    static constexpr float kAvatarSz  = 20.0f;
    static constexpr float kCloseSz   = 16.0f;
    static constexpr float kPadOuter  =  4.0f;
    static constexpr float kPadInner  =  8.0f;
    static constexpr float kTabMin    = 120.0f;
    static constexpr float kTabMax    = 240.0f;
    static constexpr float kRadius    =  6.0f;

    struct TabItem {
        std::string room_id;
        std::string display_name;
        Image       avatar;
        float       x     = 0.f;  // left edge in scroll-space (set by arrange)
        float       width = 0.f;  // set by arrange
        bool        hovered       = false;
        bool        close_hovered = false;
        // Cached TextLayout for display_name; rebuilt when name or width changes.
        std::unique_ptr<TextLayout> layout;
        float                       layout_max_w = -1.f;
        std::string                 layout_name;  // name used to build layout
    };

    // Returns the natural content width needed for tab `i` (avatar + name + chrome).
    float natural_width_(int i) const;

    // Rect of the × button for tab `i`, in scroll-space x.
    Rect close_rect_(int i) const;

    // Index of the tab whose scroll-space x range contains `scroll_x_local`
    // (-1 if none). `scroll_x_local` = pointer x + scroll_x_.
    int tab_at_(float scroll_x_local) const;

    // Clamp scroll_x_ to valid range.
    void clamp_scroll_();

    // Ensure layout for tab `i` is built at width `max_w`.
    template <typename Ctx>
    void ensure_layout_(Ctx& ctx, int i, float max_w);

    std::vector<TabItem> items_;
    int    active_idx_   = -1;
    float  scroll_x_     = 0.f;   // horizontal scroll offset (pixels)
    float  total_width_  = 0.f;   // sum of all tab widths (set by arrange)
    int    pressed_idx_  = -1;
    bool   pressed_close_ = false;
};

} // namespace tk
```

---

## Task 3: Implement `tk::TabBar`

**Files:**
- Create: `ui/shared/tk/tab_bar.cpp`

- [ ] **Step 1: Write the implementation**

```cpp
// ui/shared/tk/tab_bar.cpp
#include "tab_bar.h"
#include "theme.h"

#include <algorithm>
#include <cmath>

namespace tk {

TabBar::TabBar() {
    set_visible(false);  // hidden until 2+ tabs exist
}

// ── Public mutators ────────────────────────────────────────────────────────

void TabBar::add_tab(std::string room_id, std::string display_name, Image avatar) {
    TabItem item;
    item.room_id      = std::move(room_id);
    item.display_name = std::move(display_name);
    item.avatar       = std::move(avatar);
    items_.push_back(std::move(item));
    if (items_.size() > 1) set_visible(true);
    if (active_idx_ < 0) active_idx_ = 0;
}

void TabBar::remove_tab(const std::string& room_id) {
    auto it = std::find_if(items_.begin(), items_.end(),
        [&](const TabItem& t) { return t.room_id == room_id; });
    if (it == items_.end()) return;
    int idx = static_cast<int>(it - items_.begin());
    items_.erase(it);
    if (active_idx_ >= static_cast<int>(items_.size()))
        active_idx_ = static_cast<int>(items_.size()) - 1;
    if (idx < active_idx_) active_idx_--;
    if (items_.size() <= 1) set_visible(false);
    scroll_x_ = 0.f;
}

void TabBar::set_active(const std::string& room_id) {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (items_[i].room_id == room_id) {
            active_idx_ = i;
            return;
        }
    }
}

void TabBar::update_tab(const std::string& room_id,
                        std::string display_name, Image avatar) {
    for (auto& item : items_) {
        if (item.room_id == room_id) {
            item.display_name = std::move(display_name);
            item.avatar       = std::move(avatar);
            item.layout.reset();  // force rebuild
            return;
        }
    }
}

// ── Layout ─────────────────────────────────────────────────────────────────

float TabBar::natural_width_(int i) const {
    // kPadOuter + avatar + kPadInner + name_est + kPadInner + close + kPadOuter
    // Name estimate: clamp actual to [kTabMin, kTabMax] minus chrome.
    constexpr float kChrome = kPadOuter + kAvatarSz + kPadInner + kPadInner + kCloseSz + kPadOuter;
    // Approximation: we don't have a canvas here so we use kTabMin/kTabMax bounds.
    return kTabMin;  // actual distribution happens in arrange() with canvas access
}

Rect TabBar::close_rect_(int i) const {
    const auto& t = items_[i];
    float close_x = t.x + t.width - kPadOuter - kCloseSz;
    float close_y = bounds_.y + (kHeight - kCloseSz) * 0.5f;
    return { close_x, close_y, kCloseSz, kCloseSz };
}

int TabBar::tab_at_(float scroll_local) const {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (scroll_local >= items_[i].x && scroll_local < items_[i].x + items_[i].width)
            return i;
    }
    return -1;
}

void TabBar::clamp_scroll_() {
    float max_s = std::max(0.f, total_width_ - bounds_.w);
    if (scroll_x_ < 0.f)    scroll_x_ = 0.f;
    if (scroll_x_ > max_s)  scroll_x_ = max_s;
}

Size TabBar::measure(LayoutCtx&, Size constraints) {
    return { constraints.w, kHeight };
}

template <typename Ctx>
void TabBar::ensure_layout_(Ctx& ctx, int i, float max_w) {
    auto& t = items_[i];
    if (t.layout && std::abs(t.layout_max_w - max_w) < 0.5f
            && t.layout_name == t.display_name)
        return;
    TextStyle style;
    style.role    = FontRole::SidebarName;
    style.trim    = TextTrim::Ellipsis;
    style.max_width = max_w;
    t.layout      = ctx.factory.make_text_layout(t.display_name, style);
    t.layout_max_w = max_w;
    t.layout_name  = t.display_name;
}

void TabBar::arrange(LayoutCtx& ctx, Rect bounds) {
    bounds_ = bounds;
    if (items_.empty()) return;

    const int n = static_cast<int>(items_.size());
    constexpr float kChrome = kPadOuter + kAvatarSz + kPadInner
                            + kPadInner + kCloseSz + kPadOuter;
    const float name_min = kTabMin - kChrome;
    const float name_max = kTabMax - kChrome;

    // Measure each tab's preferred name width using a temporary text layout.
    std::vector<float> name_widths(n);
    for (int i = 0; i < n; ++i) {
        TextStyle style;
        style.role = FontRole::SidebarName;
        style.trim = TextTrim::None;
        auto layout = ctx.factory.make_text_layout(items_[i].display_name, style);
        name_widths[i] = std::clamp(layout->measure().w, name_min, name_max);
    }

    // Compute natural tab widths.
    std::vector<float> tab_widths(n);
    float natural_total = 0.f;
    for (int i = 0; i < n; ++i) {
        tab_widths[i] = kChrome + name_widths[i];
        natural_total += tab_widths[i];
    }

    // If content fits, expand tabs proportionally to fill the panel width.
    if (natural_total < bounds.w) {
        float extra = bounds.w - natural_total;
        float ratio = extra / natural_total;
        for (int i = 0; i < n; ++i)
            tab_widths[i] += tab_widths[i] * ratio;
    }

    // Position tabs left-to-right in scroll space.
    float x = 0.f;
    for (int i = 0; i < n; ++i) {
        items_[i].x     = x;
        items_[i].width = tab_widths[i];
        float name_w    = tab_widths[i] - kChrome;
        ensure_layout_(ctx, i, name_w);
        x += tab_widths[i];
    }
    total_width_ = x;
    clamp_scroll_();
}

// ── Paint ──────────────────────────────────────────────────────────────────

void TabBar::paint(PaintCtx& ctx) {
    const auto& pal = ctx.theme.palette;
    Canvas& c = ctx.canvas;

    // Tab bar background.
    c.fill_rect(bounds_, pal.chrome_bg);
    // 1px bottom border.
    c.fill_rect({ bounds_.x, bounds_.y + bounds_.h - 1.f, bounds_.w, 1.f },
                pal.separator);

    c.push_clip_rect(bounds_);

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto& t = items_[i];
        // Tab bounds in widget space.
        float tx = bounds_.x + t.x - scroll_x_;
        // Skip tabs entirely outside the clip.
        if (tx + t.width < bounds_.x || tx > bounds_.x + bounds_.w) continue;

        Rect tab_rect{ tx, bounds_.y, t.width, bounds_.h - 1.f };

        // Background.
        const bool is_active = (i == active_idx_);
        Color bg = is_active ? pal.sidebar_selected
                 : t.hovered  ? pal.sidebar_hover
                              : pal.chrome_bg;
        c.fill_rounded_rect(tab_rect, kRadius, bg);

        // Avatar.
        float av_x = tx + kPadOuter;
        float av_y = bounds_.y + (kHeight - kAvatarSz) * 0.5f;
        Rect  av_rect{ av_x, av_y, kAvatarSz, kAvatarSz };
        c.draw_circle_image(av_rect, &t.avatar,
                            t.display_name, pal.sidebar_selected);

        // Display name (ellipsis-truncated TextLayout).
        if (t.layout) {
            float name_x = av_x + kAvatarSz + kPadInner;
            float name_w = t.width - kChrome;
            float name_h = t.layout->measure().h;
            float name_y = bounds_.y + (kHeight - name_h) * 0.5f;
            c.draw_text(*t.layout, { name_x, name_y, name_w, name_h },
                        pal.text_primary);
        }

        // × close button (only when multiple tabs).
        if (items_.size() > 1) {
            Rect cr = close_rect_(i);
            cr.x = tx + t.width - kPadOuter - kCloseSz;
            Color close_col = t.close_hovered ? pal.text_primary : pal.text_secondary;
            TextStyle xs;
            xs.role   = FontRole::Body;
            xs.halign = TextHAlign::Center;
            xs.valign = TextVAlign::Center;
            c.draw_text("×", xs, cr, close_col);
        }
    }

    c.pop_clip();
}

// ── Pointer events ─────────────────────────────────────────────────────────

bool TabBar::on_pointer_down(Point local) {
    float scroll_local = local.x + scroll_x_;
    int idx = tab_at_(scroll_local);
    if (idx < 0) return false;
    pressed_idx_   = idx;
    Rect cr = close_rect_(idx);
    cr.x -= scroll_x_;  // widget-local
    pressed_close_ = (items_.size() > 1
        && local.x >= cr.x && local.x < cr.x + cr.w
        && local.y >= cr.y && local.y < cr.y + cr.h);
    return true;
}

void TabBar::on_pointer_up(Point local, bool inside_self) {
    if (pressed_idx_ < 0) return;
    float scroll_local = local.x + scroll_x_;
    int idx = inside_self ? tab_at_(scroll_local) : -1;
    if (idx == pressed_idx_) {
        if (pressed_close_) {
            // Verify still in close rect.
            Rect cr = close_rect_(idx);
            cr.x -= scroll_x_;
            if (local.x >= cr.x && local.x < cr.x + cr.w
                    && local.y >= cr.y && local.y < cr.y + cr.h) {
                if (on_tab_closed) on_tab_closed(items_[idx].room_id);
            }
        } else {
            if (on_tab_selected) on_tab_selected(items_[idx].room_id);
        }
    }
    pressed_idx_   = -1;
    pressed_close_ = false;
}

void TabBar::on_pointer_move(Point local) {
    float scroll_local = local.x + scroll_x_;
    int hover_idx = tab_at_(scroll_local);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        items_[i].hovered = (i == hover_idx);
        if (i == hover_idx && items_.size() > 1) {
            Rect cr = close_rect_(i);
            cr.x -= scroll_x_;
            items_[i].close_hovered = (local.x >= cr.x && local.x < cr.x + cr.w
                                    && local.y >= cr.y && local.y < cr.y + cr.h);
        } else {
            items_[i].close_hovered = false;
        }
    }
}

void TabBar::on_pointer_leave() {
    for (auto& t : items_) { t.hovered = false; t.close_hovered = false; }
}

bool TabBar::on_wheel(Point /*local*/, float dx, float dy) {
    // Accept horizontal scroll directly; remap vertical to horizontal on
    // mice that only produce vertical events.
    float delta = (std::abs(dx) > std::abs(dy)) ? -dx : -dy;
    scroll_x_ += delta * 40.f;
    clamp_scroll_();
    return true;
}

// Explicit instantiations so the template body doesn't need to live in the header.
template void TabBar::ensure_layout_<LayoutCtx>(LayoutCtx&, int, float);
template void TabBar::ensure_layout_<PaintCtx> (PaintCtx&,  int, float);

} // namespace tk
```

> **Note on `kChrome` constant:** `kChrome` is used in both `arrange()` and as a local constexpr in `natural_width_()`. In `arrange()`, define it locally as a `constexpr float` rather than a member, since the canvas is needed for accurate name measurement.
>
> **Note on `draw_text` overloads:** Check the existing `Canvas` API for the exact overload that takes a string literal vs `TextLayout`. If `draw_text(string, TextStyle, Rect, Color)` doesn't exist, use a temporary TextLayout from the factory — but `paint()` receives a `PaintCtx` which has a `factory` member, so: `auto lay = ctx.factory.make_text_layout("×", xs); c.draw_text(*lay, cr, close_col);`
>
> **Note on `draw_circle_image`:** The existing signature is `draw_circle_image(Rect, const Image*, std::string initials, Color fallback_color)`. The `Image` is passed as a pointer so it can be null for the initials fallback.

---

## Task 4: Add tab state to `ShellBase`

**Files:**
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`

- [ ] **Step 1: Add `TabState` and tab fields to `ShellBase.h`**

In `ShellBase.h`, directly after the `// ── Rooms` block (around line 79), add:

```cpp
    // ── Tab state ─────────────────────────────────────────────────────────────
    struct TabState {
        std::string room_id;
        float       scroll_offset = 0.f;  // fractional [0,1]: 0=top
        std::string compose_draft;
    };
    std::vector<TabState> tabs_;
    size_t                active_tab_idx_ = 0;
```

- [ ] **Step 2: Add tab management methods and virtual hooks to `ShellBase.h`**

In the `public:` section of `ShellBase`, add:

```cpp
    // ── Tab management (call from the UI thread only) ─────────────────────────

    // Ctrl+click: open room_id in a new tab, or switch to it if already open.
    void tab_open_room(const std::string& room_id);

    // Normal click: replace the current tab's room (if not already open),
    // or switch to the existing tab for room_id.
    void tab_select_room(const std::string& room_id);

    // Notification click: replace current tab if only one open; open new tab
    // if multiple tabs are open. Switches to existing tab if already open.
    void tab_navigate_room(const std::string& room_id);

    // Close the tab for room_id. No-op when tabs_.size() <= 1.
    void tab_close(const std::string& room_id);
```

In the `protected:` section, add the virtual hooks that shells override:

```cpp
    // Called after tabs_ and current_room_id_ are updated; the shell must:
    // 1. Sync the TabBar widget (add/remove/set_active).
    // 2. Show/hide the TabBar and set RoomHeader condensed mode.
    // 3. Call its existing room-switch method (onRoomSelected / on_room_selected)
    //    when current_room_id_ != view_displayed_room_id_.
    // 4. Restore compose_draft for the newly active tab.
    virtual void on_tab_state_changed_ui_() = 0;

    // Read the current fractional scroll position [0,1] from the message list.
    virtual float       get_message_scroll_fraction_()               { return 0.f; }
    // Seek the message list to fractional position t.
    virtual void        set_message_scroll_fraction_(float /*t*/)    {}
    // Read the current compose-bar draft text.
    virtual std::string get_compose_draft_()                         { return {}; }
    // Write text to the compose bar (called after a room switch restores draft).
    virtual void        set_compose_draft_(const std::string& /*s*/) {}
```

- [ ] **Step 3: Implement tab methods in `ShellBase.cpp`**

Add to the bottom of `ui/shared/app/ShellBase.cpp` (before the closing `}` of the namespace):

```cpp
// ── Tab management ─────────────────────────────────────────────────────────

static size_t find_tab_(const std::vector<tesseract::ShellBase::TabState>& tabs,
                        const std::string& room_id) {
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].room_id == room_id) return i;
    }
    return SIZE_MAX;
}

void ShellBase::tab_open_room(const std::string& room_id) {
    if (room_id.empty()) return;
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX) {
        // Already open — save current state and switch.
        if (!tabs_.empty()) {
            tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
            tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
        }
        active_tab_idx_ = existing;
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        on_tab_state_changed_ui_();
        return;
    }
    // Not open — save current state and append a new tab.
    if (!tabs_.empty()) {
        tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
        tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
    }
    // Bootstrap: if no tabs exist yet, convert current_room_id_ to first tab.
    if (tabs_.empty() && !current_room_id_.empty()) {
        tabs_.push_back({ current_room_id_, 0.f, {} });
        active_tab_idx_ = 0;
    }
    tabs_.push_back({ room_id, 0.f, {} });
    active_tab_idx_ = tabs_.size() - 1;
    current_room_id_ = room_id;
    on_tab_state_changed_ui_();
}

void ShellBase::tab_select_room(const std::string& room_id) {
    if (room_id.empty()) return;
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX) {
        // Already open — switch to it.
        if (!tabs_.empty()) {
            tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
            tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
        }
        active_tab_idx_ = existing;
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        on_tab_state_changed_ui_();
        return;
    }
    // Not open — replace the current tab (save nothing; old room discarded).
    if (tabs_.empty()) {
        tabs_.push_back({ room_id, 0.f, {} });
        active_tab_idx_ = 0;
    } else {
        tabs_[active_tab_idx_] = { room_id, 0.f, {} };
    }
    current_room_id_ = room_id;
    on_tab_state_changed_ui_();
}

void ShellBase::tab_navigate_room(const std::string& room_id) {
    if (room_id.empty()) return;
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX) {
        // Already open — switch to that tab.
        if (!tabs_.empty()) {
            tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
            tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
        }
        active_tab_idx_ = existing;
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        on_tab_state_changed_ui_();
        return;
    }
    if (tabs_.size() <= 1) {
        // Single-tab mode: replace current tab.
        tab_select_room(room_id);
    } else {
        // Multi-tab mode: open a new tab.
        tab_open_room(room_id);
    }
}

void ShellBase::tab_close(const std::string& room_id) {
    if (tabs_.size() <= 1) return;  // cannot close last tab
    size_t idx = find_tab_(tabs_, room_id);
    if (idx == SIZE_MAX) return;

    const bool closing_active = (idx == active_tab_idx_);
    if (!closing_active) {
        // Non-active tab closed: save active tab state (room doesn't change).
        tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
        tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
    }
    // Determine successor before erasing.
    size_t new_active = active_tab_idx_;
    if (closing_active) {
        new_active = (idx > 0) ? idx - 1 : 0;
    } else if (idx < active_tab_idx_) {
        --new_active;
    }
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(idx));
    active_tab_idx_ = new_active;
    current_room_id_ = tabs_[active_tab_idx_].room_id;
    on_tab_state_changed_ui_();
}
```

---

## Task 5: Add scroll API to `MessageListView`

**Files:**
- Modify: `ui/shared/views/MessageListView.h`
- Modify: `ui/shared/views/MessageListView.cpp`

- [ ] **Step 1: Declare methods in `MessageListView.h`**

In the public section of `MessageListView`, after the existing `set_historical_mode` declaration, add:

```cpp
    // Save/restore the scroll position as a fraction [0,1] (0 = top, 1 = bottom).
    float scroll_fraction() const;
    void  scroll_to_offset(float t);
```

- [ ] **Step 2: Implement in `MessageListView.cpp`**

Add at the bottom of `MessageListView.cpp` (before the closing namespace brace):

```cpp
float MessageListView::scroll_fraction() const {
    float max_s = std::max(0.f, content_height() - bounds_.h);
    return (max_s > 0.f) ? scroll_y_ / max_s : 0.f;
}

void MessageListView::scroll_to_offset(float t) {
    float max_s = std::max(0.f, content_height() - bounds_.h);
    scroll_y_ = t * max_s;
    clamp_scroll();
}
```

> `content_height()`, `scroll_y_`, `bounds_`, and `clamp_scroll()` are all accessible because `MessageListView : public tk::ListView` and these are protected/public members of `ListView`.

---

## Task 6: Add condensed mode to `RoomHeader`

**Files:**
- Modify: `ui/shared/views/RoomHeader.h`
- Modify: `ui/shared/views/RoomHeader.cpp`

- [ ] **Step 1: Update `RoomHeader.h`**

Add to the public section (after `set_avatar_provider`):

```cpp
    // Condensed mode: hides avatar/name/actions; shows only the room topic
    // with reduced height. Measured height changes — caller must trigger
    // a relayout after switching modes.
    void set_condensed(bool condensed);
```

Add to the private section:

```cpp
    bool condensed_ = false;
```

- [ ] **Step 2: Implement `set_condensed` and update `measure` / `arrange` in `RoomHeader.cpp`**

Add the method:

```cpp
void RoomHeader::set_condensed(bool condensed) {
    condensed_ = condensed;
    if (name_label_) name_label_->set_visible(!condensed);
    // topic_label_ visibility is governed by set_room(); we leave it as-is.
    // The caller (MainAppWidget) must trigger relayout after calling this.
}
```

Replace the existing `measure` implementation:

```cpp
tk::Size RoomHeader::measure(tk::LayoutCtx&, tk::Size constraints) {
    if (condensed_) {
        // One line of SidebarPreview text + 8px top + 8px bottom padding.
        constexpr float kCondensedPadY = 8.0f;
        constexpr float kLineH         = 16.0f;  // approx SidebarPreview line height
        return { constraints.w, kLineH + kCondensedPadY * 2.0f };
    }
    return { constraints.w, kHeight };
}
```

In `RoomHeader::arrange`, wrap the existing full-mode layout in `if (!condensed_)`, and add a condensed branch:

```cpp
void RoomHeader::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    if (condensed_) {
        // Only the topic label, centred vertically.
        constexpr float kCondensedPadY = 8.0f;
        constexpr float kPadX_         = 16.0f;
        float topic_h = bounds.h - kCondensedPadY * 2.0f;
        if (topic_label_) {
            topic_label_->arrange(ctx,
                { bounds.x + kPadX_, bounds.y + kCondensedPadY,
                  bounds.w - kPadX_ * 2.0f, topic_h });
        }
        if (name_label_) {
            // Keep arranged (so it doesn't hold a stale Rect) but invisible.
            name_label_->arrange(ctx, { bounds.x, bounds.y, 0.f, 0.f });
        }
        return;
    }

    // ── Full mode (unchanged from original) ──────────────────────────────
    const float text_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
    // … rest of the existing arrange() body unchanged …
```

> **Important:** the `// … rest of the existing arrange() body unchanged …` line is a placeholder — do NOT leave it as-is. Copy the actual existing body of `arrange()` from the current file and paste it after the `if (condensed_) { … return; }` block.

In `RoomHeader::paint`, guard the avatar/name/calendar-button painting with `if (!condensed_)`:

```cpp
void RoomHeader::paint(tk::PaintCtx& ctx) {
    const auto& pal = ctx.theme.palette;

    ctx.canvas.fill_rect(bounds_, pal.bg);

    if (!condensed_) {
        // ── Full-mode painting (avatar, name label, calendar button) ────
        // … existing full-mode paint code unchanged …
    }

    // Topic is painted in both modes (topic_label_ delegates to Label::paint).
    if (topic_label_ && topic_label_->visible()) topic_label_->paint(ctx);

    // Rich-text topic spans painted in both modes.
    if (!topic_spans_.empty() && !topic_display_spans_.empty()) {
        // … existing rich-topic paint code …
    }
}
```

> **Important:** again, these are guides — copy the existing bodies verbatim; do not truncate them. Only add the `if (!condensed_)` guard around the avatar/name/calendar block.

---

## Task 7: Add `TabBar` to `MainAppWidget`

**Files:**
- Modify: `ui/shared/views/MainAppWidget.h`
- Modify: `ui/shared/views/MainAppWidget.cpp`

- [ ] **Step 1: Update `MainAppWidget.h`**

Add the include:

```cpp
#include "tk/tab_bar.h"
```

Add to the public accessor section:

```cpp
    tk::TabBar*         tab_bar()         const { return tab_bar_; }
```

Add to the private member section (just before `RoomView* room_view_`):

```cpp
    tk::TabBar*         tab_bar_         = nullptr;
```

- [ ] **Step 2: Update `MainAppWidget.cpp` constructor**

After the `verif_banner_` add-child block (line ~42) and before the `room_view_` block, add:

```cpp
    // Chat panel: tab bar (hidden until 2+ rooms are open).
    auto tb = std::make_unique<tk::TabBar>();
    tab_bar_ = add_child(std::move(tb));
    // TabBar manages its own visibility via add_tab/remove_tab.
```

- [ ] **Step 3: Update `arrange()` in `MainAppWidget.cpp`**

In the chat panel section, just before `room_view_->arrange(...)`, add tab bar layout:

```cpp
    float tab_bar_h = 0.f;
    if (tab_bar_ && tab_bar_->visible()) {
        tab_bar_->arrange(ctx, { chat_x, chat_y, chat_w, tk::TabBar::kHeight });
        tab_bar_h = tk::TabBar::kHeight;
        chat_y += tab_bar_h;
    }
```

Change `kHeight` visibility: make `kHeight` public in `tab_bar.h` (it is already — `static constexpr float kHeight = 40.0f;` is in the class body, which is public).

Adjust the existing line:

```cpp
    const float room_h = std::max(0.0f, y + h - chat_y);
    room_view_->arrange(ctx, { chat_x, chat_y, chat_w, room_h });
```

(This is already correct — `chat_y` now accounts for the tab bar.)

- [ ] **Step 4: Update `paint()` in `MainAppWidget.cpp`**

In the chat panel paint section, add tab bar paint before `room_view_`:

```cpp
    if (tab_bar_ && tab_bar_->visible()) tab_bar_->paint(ctx);
    if (room_view_)                       room_view_->paint(ctx);
```

---

## Task 8: Qt6 shell — implement `on_tab_state_changed_ui_()` and wire Ctrl+click

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h`
- Modify: `ui/linux-qt/src/MainWindow.cpp`

- [ ] **Step 1: Declare overrides in `MainWindow.h`**

In the `private:` section (near the other ShellBase virtual hook declarations), add:

```cpp
    void        on_tab_state_changed_ui_()            override;
    float       get_message_scroll_fraction_()        override;
    void        set_message_scroll_fraction_(float t) override;
    std::string get_compose_draft_()                  override;
    void        set_compose_draft_(const std::string& s) override;
```

- [ ] **Step 2: Implement the virtual hooks in `MainWindow.cpp`**

Add these implementations (near the other handle_*_ui_ implementations):

```cpp
float MainWindow::get_message_scroll_fraction_() {
    if (!mainApp_) return 0.f;
    return mainApp_->room_view()->message_list()->scroll_fraction();
}

void MainWindow::set_message_scroll_fraction_(float t) {
    if (!mainApp_) return;
    mainApp_->room_view()->message_list()->scroll_to_offset(t);
}

std::string MainWindow::get_compose_draft_() {
    return roomTextArea_ ? roomTextArea_->text() : std::string{};
}

void MainWindow::set_compose_draft_(const std::string& s) {
    if (roomTextArea_) roomTextArea_->set_text(s);
}
```

> **Note:** Check that `MessageListView` is accessible via `room_view()->message_list()`. If the accessor is named differently, look at `RoomView.h` to find the correct method name for the `MessageListView*` accessor.

- [ ] **Step 3: Implement `on_tab_state_changed_ui_()` in `MainWindow.cpp`**

```cpp
void MainWindow::on_tab_state_changed_ui_() {
    if (!mainApp_) return;

    auto* tab_bar = mainApp_->tab_bar();
    const bool multi_tab = tabs_.size() > 1;

    // ── Sync TabBar widget contents ──────────────────────────────────────
    if (tab_bar) {
        // Rebuild TabBar from scratch to keep it in sync.
        // Collect which room_ids are currently in the bar.
        std::vector<std::string> bar_ids;
        for (int i = 0; i < static_cast<int>(tab_bar->item_count()); ++i)
            bar_ids.push_back(tab_bar->room_id_at(i));

        // Add missing tabs.
        for (const auto& ts : tabs_) {
            bool found = std::find(bar_ids.begin(), bar_ids.end(), ts.room_id)
                         != bar_ids.end();
            if (!found) {
                const tk::Image* av = nullptr;
                for (const auto& r : rooms_) {
                    if (r.id == ts.room_id) {
                        auto it = tk_avatars_.find(r.avatar_url);
                        if (it != tk_avatars_.end()) av = it->second.get();
                        std::string name = r.name;
                        tab_bar->add_tab(ts.room_id, name,
                                         av ? *av : tk::Image{});
                        break;
                    }
                }
            }
        }
        // Remove stale tabs.
        for (const auto& id : bar_ids) {
            bool found = std::any_of(tabs_.begin(), tabs_.end(),
                [&id](const TabState& ts) { return ts.room_id == id; });
            if (!found) tab_bar->remove_tab(id);
        }
        // Set active.
        if (active_tab_idx_ < tabs_.size())
            tab_bar->set_active(tabs_[active_tab_idx_].room_id);
    }

    // ── Condensed header ─────────────────────────────────────────────────
    if (auto* header = mainApp_->room_view()->header())
        header->set_condensed(multi_tab);

    // ── Trigger relayout (tab bar height or condensed header height changed) ─
    mainAppSurface_->relayout();
    mainAppSurface_->update();

    // ── Switch room if needed ────────────────────────────────────────────
    if (current_room_id_ != view_displayed_room_id_) {
        onRoomSelected(current_room_id_);
        // Restore compose draft for the new tab (onRoomSelected clears it).
        if (active_tab_idx_ < tabs_.size()) {
            const auto& draft = tabs_[active_tab_idx_].compose_draft;
            if (!draft.empty() && roomTextArea_) {
                roomTextArea_->set_text(draft);
            }
        }
    }
}
```

> **Note:** `tab_bar->item_count()` and `tab_bar->room_id_at(i)` are new accessors. Add them to `TabBar`:
>
> In `tab_bar.h`:
> ```cpp
>     int         item_count()             const { return static_cast<int>(items_.size()); }
>     const std::string& room_id_at(int i) const { return items_[i].room_id; }
> ```

> **Note on `RoomView::header()`:** check `RoomView.h` for the accessor name for `RoomHeader*`. It may be `header()`, `room_header()`, or similar. Use whichever exists.

- [ ] **Step 4: Update `navigate_to_room` to use tab routing**

Find the existing `navigate_to_room` implementation (~line 1505) and change it to:

```cpp
void MainWindow::navigate_to_room(const std::string& room_id) {
    if (room_id.empty()) return;
    if (mainApp_)
        mainApp_->room_list_view()->set_selected_room(room_id);
    tab_navigate_room(room_id);   // ShellBase tab logic → on_tab_state_changed_ui_
    show();
    raise();
    activateWindow();
}
```

- [ ] **Step 5: Wire Ctrl+click in the `on_room_selected` callback**

Find the existing `on_room_selected` wiring (~line 115):

```cpp
mainApp_->room_list_view()->on_room_selected =
    [this](const std::string& room_id) { onRoomSelected(room_id); };
```

Replace with:

```cpp
mainApp_->room_list_view()->on_room_selected =
    [this](const std::string& room_id) {
        // Spaces drill into themselves — bypass tab logic.
        for (const auto& r : rooms_) {
            if (r.id == room_id && r.is_space) {
                onRoomSelected(room_id);
                return;
            }
        }
        bool ctrl = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
        if (ctrl) {
            tab_open_room(room_id);
        } else {
            tab_select_room(room_id);
        }
    };
```

- [ ] **Step 6: Wire `TabBar` callbacks in the surface construction block**

In the same constructor/surface-construction block, after the `on_room_selected` wiring, add:

```cpp
    if (mainApp_->tab_bar()) {
        mainApp_->tab_bar()->on_tab_selected =
            [this](const std::string& room_id) {
                tab_select_room(room_id);
            };
        mainApp_->tab_bar()->on_tab_closed =
            [this](const std::string& room_id) {
                tab_close(room_id);
            };
    }
```

- [ ] **Step 7: Restore scroll offset in `handle_timeline_reset_ui_`**

In `MainWindow::handle_timeline_reset_ui_`, after the `set_messages(...)` call, add:

```cpp
    // Restore saved scroll offset for this tab.
    for (const auto& ts : tabs_) {
        if (ts.room_id == room_id && ts.scroll_offset > 0.f) {
            if (mainApp_)
                mainApp_->room_view()->message_list()
                    ->scroll_to_offset(ts.scroll_offset);
            break;
        }
    }
```

---

## Task 9: GTK4 shell — stub + Ctrl+click

**Files:**
- Modify: `ui/linux-gtk/src/MainWindow.h`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`

- [ ] **Step 1: Declare overrides in GTK4 `MainWindow.h`**

```cpp
    void        on_tab_state_changed_ui_()               override;
    float       get_message_scroll_fraction_()           override;
    void        set_message_scroll_fraction_(float t)    override;
    std::string get_compose_draft_()                     override;
    void        set_compose_draft_(const std::string& s) override;
```

- [ ] **Step 2: Implement in GTK4 `MainWindow.cpp`**

```cpp
float MainWindow::get_message_scroll_fraction_() {
    if (!mainApp_) return 0.f;
    return mainApp_->room_view()->message_list()->scroll_fraction();
}
void MainWindow::set_message_scroll_fraction_(float t) {
    if (mainApp_) mainApp_->room_view()->message_list()->scroll_to_offset(t);
}
std::string MainWindow::get_compose_draft_() {
    // GTK4 shell: read from the native text area equivalent.
    // Replace room_text_area_ with whatever the GTK4 shell uses for compose input.
    return room_text_area_ ? room_text_area_->text() : std::string{};
}
void MainWindow::set_compose_draft_(const std::string& s) {
    if (room_text_area_) room_text_area_->set_text(s);
}

void MainWindow::on_tab_state_changed_ui_() {
    if (!mainApp_) return;
    const bool multi_tab = tabs_.size() > 1;

    // Sync TabBar widget.
    auto* tab_bar = mainApp_->tab_bar();
    if (tab_bar) {
        // (Same sync logic as Qt6 — add missing tabs, remove stale tabs, set_active.)
        // For brevity: follow the exact same pattern as in the Qt6 implementation above.
        for (const auto& ts : tabs_) {
            // find RoomInfo for ts.room_id, call tab_bar->add_tab(...)
        }
        if (active_tab_idx_ < tabs_.size())
            tab_bar->set_active(tabs_[active_tab_idx_].room_id);
    }

    if (auto* header = mainApp_->room_view()->header())
        header->set_condensed(multi_tab);

    // GTK4 surface relayout equivalent — force measure+arrange+paint.
    // Use whatever the GTK4 host/surface exposes for this (likely gtk_widget_queue_resize
    // or the host's relayout method on the tk::gtk::Surface).
    if (surface_) { surface_->relayout(); surface_->queue_draw(); }

    if (current_room_id_ != view_displayed_room_id_) {
        on_room_selected(current_room_id_);
        if (active_tab_idx_ < tabs_.size()) {
            const auto& draft = tabs_[active_tab_idx_].compose_draft;
            if (!draft.empty() && room_text_area_)
                room_text_area_->set_text(draft);
        }
    }
}
```

- [ ] **Step 3: Wire Ctrl+click in GTK4 shell**

Find the `on_room_selected` callback wiring in GTK4 `MainWindow.cpp` (~line 386):

```cpp
room_list_view_->on_room_selected =
    [this](const std::string& room_id) { on_room_selected(room_id); };
```

Replace with:

```cpp
room_list_view_->on_room_selected =
    [this](const std::string& room_id) {
        for (const auto& r : rooms_) {
            if (r.id == room_id && r.is_space) {
                on_room_selected(room_id);
                return;
            }
        }
        // Check Ctrl modifier via GDK. At callback time the GDK event is still current.
        GdkModifierType state = GDK_NO_MODIFIER_MASK;
        if (GdkEvent* ev = gtk_get_current_event())
            gdk_event_get_modifier_state(ev, &state);
        bool ctrl = (state & GDK_CONTROL_MASK) != 0;
        if (ctrl) tab_open_room(room_id);
        else      tab_select_room(room_id);
    };
```

Also update `navigate_to_room` in GTK4 shell (~line 2566):

```cpp
void MainWindow::navigate_to_room(const std::string& room_id) {
    if (room_id.empty()) return;
    if (mainApp_) mainApp_->room_list_view()->set_selected_room(room_id);
    tab_navigate_room(room_id);
    // show/raise/focus the GTK4 window — use existing mechanism
    gtk_window_present(GTK_WINDOW(window_));
}
```

Wire `TabBar` callbacks after the room-list wiring:

```cpp
if (mainApp_->tab_bar()) {
    mainApp_->tab_bar()->on_tab_selected =
        [this](const std::string& id) { tab_select_room(id); };
    mainApp_->tab_bar()->on_tab_closed =
        [this](const std::string& id) { tab_close(id); };
}
```

---

## Task 10: Win32 shell — stub + Ctrl+click

**Files:**
- Modify: `ui/windows/src/MainWindow.h`
- Modify: `ui/windows/src/MainWindow.cpp`

- [ ] **Step 1: Declare overrides in Win32 `MainWindow.h`**

(Same declarations as Qt6 and GTK4 — `on_tab_state_changed_ui_()` and the four scroll/draft hooks.)

- [ ] **Step 2: Implement in Win32 `MainWindow.cpp`**

Follow the same pattern as Qt6. The Win32 shell uses `on_room_selected` (or `onRoomSelected`) for room switching and `surface_->relayout() + surface_->InvalidateRect(...)` or equivalent for repaint.

For Ctrl detection in the `on_room_selected` callback:

```cpp
bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
```

Update `navigate_to_room` to call `tab_navigate_room(room_id)` as in Qt6.

Wire `TabBar` callbacks analogously.

---

## Task 11: macOS shell — stub + Cmd+click

**File:**
- Modify: `ui/macos/src/MainWindowController.mm`

- [ ] **Step 1: Implement in `MacShell` (the C++ nested class)**

Add `on_tab_state_changed_ui_()` and the scroll/draft hooks to `MacShell`:

```cpp
void MacShell::on_tab_state_changed_ui_() {
    if (!ctrl_->mainApp()) return;
    const bool multi_tab = tabs_.size() > 1;

    auto* tab_bar = ctrl_->mainApp()->tab_bar();
    if (tab_bar) {
        // Sync logic — same pattern as Qt6.
        if (active_tab_idx_ < tabs_.size())
            tab_bar->set_active(tabs_[active_tab_idx_].room_id);
    }

    if (auto* hdr = ctrl_->mainApp()->room_view()->header())
        hdr->set_condensed(multi_tab);

    // Trigger relayout on the macOS tk Surface.
    if (ctrl_->surface()) { ctrl_->surface()->relayout(); ctrl_->surface()->setNeedsDisplay(); }

    if (current_room_id_ != view_displayed_room_id_) {
        [ctrl_->controller() onRoomSelected:current_room_id_];
        if (active_tab_idx_ < tabs_.size()) {
            const auto& draft = tabs_[active_tab_idx_].compose_draft;
            if (!draft.empty()) [ctrl_->controller() setComposeDraft:draft];
        }
    }
}

float MacShell::get_message_scroll_fraction_() {
    return ctrl_->mainApp()
        ? ctrl_->mainApp()->room_view()->message_list()->scroll_fraction()
        : 0.f;
}
void MacShell::set_message_scroll_fraction_(float t) {
    if (ctrl_->mainApp())
        ctrl_->mainApp()->room_view()->message_list()->scroll_to_offset(t);
}
std::string MacShell::get_compose_draft_() {
    return [ctrl_->controller() composeDraft];
}
void MacShell::set_compose_draft_(const std::string& s) {
    [ctrl_->controller() setComposeDraft:s];
}
```

> **Note on macOS accessor names:** `ctrl_->mainApp()`, `ctrl_->surface()`, `ctrl_->controller()` — use whatever member accessors `MainWindowController` exposes to `MacShell` via `public:` using-declarations in the class body. Check the actual `MacShell` definition in `MainWindowController.mm` for the exact names.

> **Note on `composeDraft`/`setComposeDraft:`:** These are ObjC helpers that read/write the compose NativeTextArea. If they don't exist, look at how the macOS shell clears compose text and add analogous methods for get/set.

- [ ] **Step 2: Wire Cmd+click in macOS shell**

Find the `on_room_selected` callback wiring in `MainWindowController.mm` (~line 861):

```objc
_mainApp->room_list_view()->on_room_selected =
    [s](const std::string& room_id) {
        if (s) [s onRoomSelected:room_id];
    };
```

Replace with (C++ closure in ObjC++ context):

```objc
_mainApp->room_list_view()->on_room_selected =
    [s](const std::string& room_id) {
        if (!s) return;
        // Check if it's a space.
        for (const auto& r : s->_shell->rooms_) {
            if (r.id == room_id && r.is_space) {
                [s onRoomSelected:room_id];
                return;
            }
        }
        // Cmd key = tab-open on macOS.
        bool cmd = ([NSEvent modifierFlags] & NSEventModifierFlagCommand) != 0;
        if (cmd) s->_shell->tab_open_room(room_id);
        else     s->_shell->tab_select_room(room_id);
    };
```

> **Note on macOS accessor to `_shell`:** `MacShell` is held as a `unique_ptr` in `MainWindowController`. The capture above uses `s` which is the `__weak MainWindowController*`. Access `_shell` appropriately — if `_shell` is a private ivar, either use a method or adjust the accessor pattern.

---

## Task 12: Build and smoke-test

- [ ] **Step 1: Build**

```bash
cmake --preset linux-qt6-debug
cmake --build build/linux-qt6-debug 2>&1 | head -80
```

Expected: build succeeds with 0 errors.

- [ ] **Step 2: Address compile errors**

Common issues and fixes:
- `tk::Image` passed by value in `TabBar::add_tab` but `Image` may be non-copyable — use the existing copy/move semantics or store by `std::shared_ptr<Image>`. Check how other widgets store images.
- `canvas.draw_circle_image` signature mismatch — check `canvas.h` for the exact overload and adjust the call in `tab_bar.cpp`.
- `canvas.draw_text` string overload — if only `TextLayout` overload exists, use `ctx.factory.make_text_layout("×", style)` then `c.draw_text(*lay, rect, color)`.
- `tab_bar->item_count()` not found — verify it was added to `tab_bar.h`.
- `room_view()->message_list()` returns wrong type or no such accessor — check `RoomView.h`.
- `room_view()->header()` — check `RoomView.h` for the actual accessor name.

- [ ] **Step 3: Smoke-test**

```bash
./build/linux-qt6-debug/ui/linux-qt/tesseract
```

Manual checks:
1. App starts and shows rooms as before (single-tab mode, no visible tab bar).
2. Ctrl+click a room: tab bar appears with two tabs (previous room + new room).
3. Room header switches to condensed (topic only, smaller height).
4. Ctrl+click another room: third tab appears.
5. Click a tab: switches to that room, scroll/draft are preserved on switch back.
6. Click × on a tab: tab closes; if 1 tab left, tab bar hides and header returns to full mode.
7. Click a room in the sidebar that is already open in a tab: switches focus to that tab.
8. Receive a notification, click it: navigates to that room per the spec rules.

---

## Task 13: Single commit

- [ ] **Step 1: Stage and commit all changes**

```bash
git add \
  ui/shared/tk/tab_bar.h \
  ui/shared/tk/tab_bar.cpp \
  ui/shared/app/ShellBase.h \
  ui/shared/app/ShellBase.cpp \
  ui/shared/views/MessageListView.h \
  ui/shared/views/MessageListView.cpp \
  ui/shared/views/RoomHeader.h \
  ui/shared/views/RoomHeader.cpp \
  ui/shared/views/MainAppWidget.h \
  ui/shared/views/MainAppWidget.cpp \
  ui/linux-qt/src/MainWindow.h \
  ui/linux-qt/src/MainWindow.cpp \
  ui/linux-gtk/src/MainWindow.h \
  ui/linux-gtk/src/MainWindow.cpp \
  ui/windows/src/MainWindow.h \
  ui/windows/src/MainWindow.cpp \
  ui/macos/src/MainWindowController.mm

git commit -m "feat(tabs): multi-room horizontal tab bar

- tk::TabBar: new shared widget; avatar + display name (ellipsis-truncated)
  + close button; horizontal scroll; self-hides at one tab
- ShellBase: TabState, tabs_, active_tab_idx_; tab_open_room / tab_select_room /
  tab_navigate_room / tab_close manage tab list; virtual scroll/draft hooks
- RoomHeader: set_condensed(bool) collapses to topic-only with reduced height
- MessageListView: scroll_fraction() + scroll_to_offset(float) for tab state save/restore
- MainAppWidget: mounts TabBar above RoomView in the chat panel
- All four shells: on_tab_state_changed_ui_() syncs TabBar + condensed header;
  Ctrl/Cmd+click in room list opens a new tab; notification click uses tab_navigate_room

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Self-review notes

- `tk::Image` move/copy semantics need to match usage in `TabBar::add_tab` and `TabBar::update_tab`. If `Image` is non-copyable, change the parameter to `Image&&` and use `std::move`.
- `TabBar::paint` references `kChrome` as a local computation — ensure it's a `constexpr float` inside the function, not a member variable.
- `tab_bar.h` has `template void ensure_layout_<LayoutCtx>` and `<PaintCtx>` explicit instantiations in `tab_bar.cpp`. If the project's build setup doesn't see both CTUs together, move the template body to the header.
- Win32 shell: the `surface_->relayout()` call pattern should match whatever the Win32 `tk::win32::Surface` exposes — may be `InvalidateRect` + `UpdateWindow`, or a `relayout()` method analogous to Qt6.
- macOS `_shell->rooms_` access in the ObjC++ closure: `MacShell` grants `MainWindowController` access via `public:` `using` declarations in the class body — verify `rooms_` is exposed. If not, expose it or use `_shell->find_room(room_id).is_space` via a public helper.
