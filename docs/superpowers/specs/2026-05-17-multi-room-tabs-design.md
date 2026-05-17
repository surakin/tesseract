# Multi-Room Tab View Design

**Date:** 2026-05-17
**Status:** Approved

## Overview

Tesseract currently shows one room at a time. This design adds a tab bar that lets users open multiple rooms simultaneously. Tabs appear only when two or more rooms are open; with a single room the UI is identical to today's. The implementation uses a single `RoomView` instance whose content is swapped on tab switch (Approach B), with per-tab scroll offset and compose draft cached in `ShellBase`.

---

## Widget Tree

```
Chat panel (flex column)
  [RecoveryBanner]          ← hidden by default, unchanged
  [VerificationBanner]      ← hidden by default, unchanged
  TabBar                    ← NEW — hidden when tab_count == 1
  RoomView
    RoomHeader              ← full mode when tab_count == 1
                            ← condensed mode (topic only) when tab_count > 1
    MessageListView
    ComposeBar
```

`TabBar` is a `tk::Widget` subclass living in `ui/shared/tk/tab_bar.h`. It is owned by `MainAppWidget` and rendered via `tk::Canvas`, following the same measure/arrange/paint contract as all other toolkit widgets.

---

## Section 1 — TabBar Widget

### Public interface

```cpp
// ui/shared/tk/tab_bar.h
class TabBar : public Widget {
public:
    void add_tab(std::string room_id, std::string display_name, Image avatar);
    void remove_tab(const std::string& room_id);
    void set_active(const std::string& room_id);
    void update_tab(const std::string& room_id, std::string display_name, Image avatar);

    std::function<void(const std::string& room_id)> on_tab_selected;
    std::function<void(const std::string& room_id)> on_tab_closed;
};
```

`TabBar` hides itself (`set_visible(false)`) when it contains exactly one tab.

### Internal state

```cpp
struct TabItem {
    std::string room_id;
    std::string display_name;
    Image       avatar;     // decoded, sized for tab height minus padding
    float       x;          // left edge in scroll space, set by arrange()
    float       width;      // computed width for this tab
};
std::vector<TabItem> items_;
size_t               active_idx_ = 0;
float                scroll_x_   = 0.f;  // horizontal scroll offset in pixels
```

### Measure

Returns (∞ preferred width, fixed height ~40px). `MainAppWidget` provides the full chat-panel width.

### Arrange

Each tab is sized to its natural content width (avatar + display name + padding + × button), subject to a minimum (~120px) and maximum (~240px). If the total natural width fits within the panel, tabs expand evenly to fill it. If it overflows, tabs stay at natural width and the widget becomes horizontally scrollable.

### Paint (per visible tab)

1. Background fill — active tab gets highlight colour; inactive tabs get hover-sensitive fill.
2. Circle-clipped avatar at a fixed size (e.g. 20×20px).
3. Display name — measured via `Canvas::measure_text`; clipped with `…` appended when it overflows the remaining tab width.
4. × close button — drawn only when `items_.size() > 1` (the last tab cannot be closed).

The paint pass clips to the widget's bounds so partially-visible tabs trim naturally.

### Scrolling

`on_scroll` advances `scroll_x_`, clamped to `[0, total_content_width − panel_width]`. If the host delivers only vertical wheel deltas (common on scroll-wheel mice), `TabBar` remaps vertical to horizontal scroll when it is the current scroll target.

### Hit testing

`on_pointer_down` maps the pointer x + `scroll_x_` to the items list. A hit on the × area fires `on_tab_closed`; a hit on the tab body fires `on_tab_selected`.

---

## Section 2 — ShellBase State and Tab Lifecycle

### New state

```cpp
struct TabState {
    std::string room_id;
    float       scroll_offset = 0.f;  // fractional position [0,1]
    std::string compose_draft;
};

std::vector<TabState> tabs_;
size_t                active_tab_idx_ = 0;
```

`current_room_id_` is replaced by a computed accessor:

```cpp
const std::string& active_room_id() const {
    return tabs_.empty() ? empty_string : tabs_[active_tab_idx_].room_id;
}
```

All existing callers of `current_room_id_` are updated to `active_room_id()`. `view_displayed_room_id_` is retained to guard redundant redraws.

### Lifecycle table

| User action | Effect |
|---|---|
| Ctrl+click room (not open) | Append `TabState{room_id}` to `tabs_`; set `active_tab_idx_` to new entry; trigger room switch |
| Ctrl+click room (already open) | Set `active_tab_idx_` to existing entry; trigger room switch |
| Normal click room (not open) | Save current scroll/draft; replace `tabs_[active_tab_idx_].room_id`; clear cached state; trigger room switch |
| Normal click room (already open) | Set `active_tab_idx_` to existing entry; trigger room switch |
| Click tab in TabBar | Save current scroll/draft; set `active_tab_idx_`; trigger room switch with cached offset |
| Close tab (× button) | Save draft; remove entry from `tabs_`; activate the tab to the left, or the new leftmost if the closed tab was leftmost; if `tabs_.size() == 1` after removal, `TabBar` hides itself |
| Click desktop notification | If the room is already open in a tab, switch to that tab. Otherwise: if only one tab is open, replace it; if multiple tabs are open, open a new tab and switch to it |

"Trigger room switch" means the existing `on_room_selected` path: subscribe room, paginate if needed, `set_room`, `set_messages`, then `MessageListView::scroll_to_offset(tab.scroll_offset)`.

---

## Section 3 — RoomHeader Condensed Mode

```cpp
void RoomHeader::set_condensed(bool condensed);
```

**Full mode** (single-room, `tab_count == 1`): unchanged — avatar, display name, topic, member count, action buttons, fixed ~60px height.

**Condensed mode** (`tab_count > 1`): only the room topic as a single ellipsis-truncated line of muted text with symmetric vertical padding (~8px top + 8px bottom). All other elements are hidden. `measure()` returns the natural height of one text line plus padding (~32–36px).

`MainAppWidget` calls `tab_bar_->set_visible` and `room_header_->set_condensed` together when the tab count crosses 1, and triggers a chat-panel re-layout because the header height changes.

---

## Section 4 — Cross-Platform and Input

### Ctrl+click on RoomListView

`RoomListView` gains a second callback:

```cpp
std::function<void(const std::string& room_id)> on_room_open_in_tab;
```

This fires when the primary button is released with the Ctrl modifier held. On macOS, Cmd replaces Ctrl (standard platform convention) — `RoomListView` maps both to `on_room_open_in_tab` based on the platform modifier flag already present on `PointerEvent::modifiers`.

### Native overlay positions

The `NativeTextArea` for the compose bar is repositioned in each shell's `set_on_layout` callback. When `TabBar` is visible and `RoomHeader` is condensed, the compose area shifts up. The existing layout callback handles this automatically because it measures widget positions at layout time rather than using cached fixed offsets.

### Desktop notification click

Each shell already handles notification activation (the OS callback that fires when the user clicks a notification). That handler must call a new `ShellBase` method:

```cpp
void ShellBase::navigate_to_room(const std::string& room_id);
```

`navigate_to_room` implements the lifecycle rule above: switch to the existing tab if the room is already open, otherwise replace the active tab. Shells pass the `room_id` carried by the notification payload to this method — no per-shell tab logic is needed.

### All four shells

`TabBar`, `MainAppWidget`, and the condensed `RoomHeader` are all `tk` widgets in `ui/shared/`. No per-shell code changes are required beyond wiring `on_room_open_in_tab` in each shell's `RoomListView` setup — all four shells (Qt6, GTK4, Win32, macOS) inherit this behaviour via `ShellBase` and `MainAppWidget`.

---

## What Is Not Changing

- `MessageListView` — unchanged except for a new `scroll_to_offset(float)` method.
- `ComposeBar` — unchanged; draft text is saved/restored by `ShellBase` on tab switch.
- `NativeTextArea` / `NativeTextField` overlays — one per shell window as today; repositioned on layout as today.
- Room subscription and pagination — existing per-room logic in `ShellBase` is reused.
- Secondary pop-out windows — `secondary_windows_` registry is unaffected.
