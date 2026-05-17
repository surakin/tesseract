# Design: Optimistic Send + Picker Polish

**Date:** 2026-05-17  
**Status:** Approved

## Context

Two independent improvements:

1. **Optimistic messages** — Currently, sent messages only appear in the list after the homeserver echoes them back through the sync loop. This means a noticeable delay between pressing send and seeing your own message. We want the message to appear immediately with a clock indicator (sending), transition to a checkmark (delivered), and surface a retry/dismiss option on failure.

2. **Picker polish** — The emoji and sticker pickers have a dark-mode palette defined but hover highlighting is silently broken (GridView never updates `hovered_index_`). Additionally, neither picker shows a shortcode tooltip on hover. Both gaps are fixed together because they share the same root cause (missing `on_pointer_move` routing).

---

## Feature 1: Optimistic Messages via SDK Local Echo

### Approach

`matrix-sdk-ui 0.17` (already a dependency) supports local echo via `Timeline::send()`. Calling it instead of `room.send()` immediately pushes a `VectorDiff::PushBack` into the existing timeline stream — no separate threading or body-matching required. The local echo item carries `EventSendState` (`NotSentYet`, `SendingFailed`, `Sent`) and flows through the same `handle_timeline_diff()` → `on_message_inserted` / `on_message_updated` path that all messages use today.

### Rust changes (`sdk/src/`)

**`bridge.rs` — `TimelineEvent` struct**  
Add four new fields:

```rust
pending_state:       String,   // "sending" | "failed" | ""
pending_error:       String,   // error message when failed
pending_recoverable: bool,     // true → queue disabled, retry re-enables it
pending_txn_id:      String,   // transaction ID for abort/retry lookup
```

**`bridge.rs` — new FFI functions**

```rust
fn retry_send(self: &mut ClientFfi, room_id: &str) -> OpResult;
fn abort_send(self: &mut ClientFfi, room_id: &str, txn_id: &str) -> OpResult;
```

**`client.rs` — `send_message()`**  
Replace `room.send(content).await` with `timeline.send(content).await` when a timeline handle exists for the room. Fall back to `room.send()` if no timeline is subscribed (e.g., room not open). `timeline.send()` returns quickly (message is queued); the call no longer stalls the UI thread.

**`client.rs` — item-to-event conversion**  
In the function that converts a `TimelineItem` to a `TimelineEvent` bridge struct: when `event_item.is_local_echo()` is true, read `send_state()` and populate the four new `pending_*` fields. Map:

- `EventSendState::NotSentYet { .. }` → `pending_state = "sending"`
- `EventSendState::SendingFailed { error, is_recoverable }` → `pending_state = "failed"`, populate error and recoverable fields
- `EventSendState::Sent { .. }` → not reached as local echo (SDK replaces item with server event)

**`client.rs` — `retry_send()`**  
`room.send_queue().set_enabled(true)` — re-enables the queue after a recoverable failure; the SDK automatically retries all pending sends.

**`client.rs` — `abort_send()`**  
`timeline.redact(&TimelineEventItemId::TransactionId(txn_id.into()), None).await` — the public `Timeline::redact()` API calls `SendHandle::abort()` internally for local echoes.

### C++ client layer (`client/`)

`client.h` / `client.cpp`: add `retry_send(room_id)` and `abort_send(room_id, txn_id)` mirroring the new FFI functions.

### C++ shared views (`ui/shared/`)

**`views/MessageListView.h` — `MessageRowData`**  
Add:

```cpp
enum class PendingState { None, Sending, Failed };

PendingState pending_state     = PendingState::None;
std::string  pending_txn_id;
std::string  pending_error;
bool         pending_recoverable = false;
```

**Event → `MessageRowData` conversion** (`app/ShellBase.cpp` or `app/EventHandlerBase.cpp`)  
Map the four `pending_*` bridge fields to `MessageRowData` fields when building a row from an incoming event.

**`views/MessageListView.cpp` — `paint_row()`**  
For own messages only, draw a small indicator at the bottom-right of the message body (same zone as read receipts, non-overlapping):

| State | Glyph | Color | Notes |
|-------|-------|-------|-------|
| `Sending` | ◷ | `text_muted` | `FontRole::Small` |
| `Failed` recoverable | ⚠ + "Retry" | red + `accent` | "Retry" is tappable |
| `Failed` unrecoverable | ⚠ + ✕ | red | ✕ calls `abort_send` |
| Just-sent (2 s) | ✓ | `accent` | tracked in `RoomWindowBase` |

Hit-testing and click handling for Retry / ✕ follow the same pattern as the existing hover-button strip (reply / edit / delete) in `paint_row()`.

**`views/MessageListView.h/.cpp` — just-sent detection**  
`update_message(idx, new_row)` checks whether the existing row at `idx` had `pending_state == Sending` and the new row has `pending_state == None`. If so, it sets `new_row.just_sent = true` (a new bool field, not persisted in `MessageRowData` permanently — it is only kept in the in-memory list) and fires a new `on_just_sent` callback with the event ID. `paint_row()` draws ✓ in `accent` for rows where `just_sent` is true.

**`app/RoomWindowBase.h/.cpp`**  
- Wire `on_retry_send` / `on_abort_send` callbacks from `MessageListView` through to `Client`.
- When `on_just_sent(event_id)` fires, schedule a 2 s delayed call via `shell_->post_to_ui_after_(2000ms, [this, event_id]{ clear_just_sent(event_id); })`. `clear_just_sent` calls `room_view_->clear_just_sent(event_id)` which sets `just_sent = false` on the matching row and triggers a repaint.

**`app/ShellBase.h/.cpp` and platform shells**  
Add `virtual void post_to_ui_after_(std::chrono::milliseconds delay, std::function<void()> fn)` alongside the existing `post_to_ui_()`. Each platform shell implements it with: `QTimer::singleShot` (Qt6), `g_timeout_add` (GTK4), `SetTimer` / `WM_TIMER` (Win32), `dispatch_after(dispatch_get_main_queue())` (macOS).

### Fallback behavior

If `send_message()` is called when no timeline is subscribed for the room, it falls back to `room.send()` (old behavior, no local echo). This handles edge cases like sending from a non-open room.

---

## Feature 2: Dark Theme + Shortcode Tooltips

### Dark theme audit

Audit `EmojiPicker::paint()` and `StickerPicker::paint()` for hardcoded `Color::rgb(...)` literals. Cell rendering already uses palette colors (`subtle_hover`, `chrome_bg`, `text_primary`). Any hardcoded values in the picker background, border, tab strip, or search field styling are replaced with equivalents from `ctx.theme.palette` (e.g., `bg`, `border`, `popup_border`, `text_secondary`).

### Hover tracking fix (`ui/shared/tk/`)

**`widget.h`** — Add a new virtual:

```cpp
virtual void on_pointer_move(tk::Point local) {}
```

**`list_view.h` / `list_view.cpp` — `GridView`**  
Override `on_pointer_move`: compute the cell index under the pointer, update `hovered_index_`, and call `invalidate()` if it changed. Clear `hovered_index_` to `-1` when the pointer leaves the grid bounds.

**Platform surfaces** (4 files: Qt6, GTK4, Win32, macOS)  
Route native mouse-move events to `widget->on_pointer_move(local_point)` on the currently hit-tested widget, following the same dispatch pattern used for `on_pointer_down`. No platform-specific tooltip API is used.

### Shortcode tooltips (inline painted)

No new widget class or platform tooltip API. When `hovered_index_ >= 0`, the picker's `paint()` method draws a floating label directly on top of the grid — clipped to the picker's own bounds so it never overflows the popup edge.

**Label style:**
- Background: `chrome_bg` fill, `popup_border` 1px stroke, 4px corner radius
- Text: `FontRole::Small`, `text_primary` color
- Position: centered horizontally over the hovered cell, 4px above its top edge (flipped to below if too close to the picker's top)
- Content: `:shortcode:` — first space-delimited token from `emoji::Entry::shortcodes` for emoji; `ImagePackImage::body` for stickers

**EmojiPicker** — add `on_pointer_move(Point local)` override. The picker owns the full panel (tab strip at top, search field, grid body). When the pointer is inside the grid body rect, compute the hovered cell index from the local coordinates, update `hovered_grid_cell_` (a new `int`, -1 when no cell hovered), and repaint. When the pointer is outside the grid body, set `hovered_grid_cell_ = -1` and repaint. The tooltip label is only drawn when `hovered_grid_cell_ >= 0`.

**StickerPicker** — same pattern with `hovered_grid_cell_`.

The tooltip appears immediately on hover (no delay) — the user has already intentionally opened the picker, so instant feedback is appropriate.

---

## File Inventory

| File | Change |
|------|--------|
| `sdk/src/bridge.rs` | Add 4 pending fields to `TimelineEvent`; add `retry_send`, `abort_send` FFI fns |
| `sdk/src/client.rs` | `send_message()` → `timeline.send()`; item conversion for local echo; `retry_send`, `abort_send` impl |
| `client/include/tesseract/client.h` | Add `retry_send`, `abort_send` |
| `client/src/client.cpp` | Implement new methods |
| `ui/shared/views/MessageListView.h` | `PendingState` enum + pending fields on `MessageRowData` |
| `ui/shared/views/MessageListView.cpp` | `paint_row()` indicators; hit-test for retry/dismiss; `measure_row_height()` adjustment |
| `ui/shared/app/ShellBase.cpp` (or EventHandlerBase) | Map pending fields in event→row conversion |
| `ui/shared/app/ShellBase.h/.cpp` + 4 platform shells | Add `post_to_ui_after_(ms, fn)` |
| `ui/shared/app/RoomWindowBase.h/.cpp` | `retry_send`, `abort_send` wiring; `on_just_sent` → delayed clear for ✓ |
| `ui/shared/views/EmojiPicker.cpp` | Dark theme audit; `on_pointer_move`; inline tooltip paint |
| `ui/shared/views/StickerPicker.cpp` | Dark theme audit; `on_pointer_move`; inline tooltip paint |
| `ui/shared/tk/widget.h` | Add `virtual on_pointer_move(Point)` |
| `ui/shared/tk/list_view.h` / `list_view.cpp` | `GridView::on_pointer_move()` impl |
| `ui/linux-qt/src/` (Surface) | Route Qt mouse-move → `on_pointer_move` |
| `ui/linux-gtk/src/` (Surface) | Route GTK motion-notify → `on_pointer_move` |
| `ui/windows/src/` (Surface) | Route WM_MOUSEMOVE → `on_pointer_move` |
| `ui/macos/src/` (Surface) | Route `mouseMoved:` → `on_pointer_move` |

---

## Verification

1. **Optimistic send (happy path)**: Send a message → message appears immediately with ◷ → ◷ disappears and ✓ appears briefly → ✓ fades after ~2 s.
2. **Optimistic send (failure)**: Disconnect network, send → ◷ → ⚠ + "Retry" → reconnect, click Retry → message sends normally.
3. **Optimistic send (unrecoverable)**: Simulate an unrecoverable error → ⚠ + ✕ → click ✕ → message removed.
4. **Dark theme**: Switch OS to dark mode → open emoji/sticker picker → all surfaces use palette colors (no hardcoded light-mode values visible).
5. **Hover highlighting**: Hover over emoji cells → cell highlights correctly (was broken before).
6. **Shortcode tooltips**: Hover over any emoji or sticker → `:shortcode:` label appears centered above the cell; moving away clears it.
7. **Run Rust tests**: `cargo test -p tesseract-sdk-ffi` passes.
8. **Run C++ tests**: `ctest --test-dir build/linux-qt6-debug` passes.
