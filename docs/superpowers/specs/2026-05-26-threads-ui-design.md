# Threads UI

**Date:** 2026-05-26

## Context

The Tesseract SDK, FFI, and C++ client API for Matrix threads (MSC3440) landed in commit `9d99d2e` — subscribe/paginate/list, send-into-thread, per-event thread metadata (`thread_root_id`, `is_thread_root`, `thread_reply_count`, `thread_latest_*`), and `IEventHandler` callbacks (`on_thread_reset/inserted/updated/removed`, `on_threads_updated`) are all in place and unit-tested. **No UI consumes any of it.** This spec adds the cross-platform UI on top.

The user-visible feature is: see threads exist in the main timeline, browse all threads in a room from a side panel, open a thread to read/reply without losing the main timeline context, and have visual cues (root highlight, main-list dim) that keep the user oriented.

## Design

### Layout

`RoomView` gains a right-side panel that occupies 40% of the room area when open (MessageListView shrinks to 60%). The room's own ComposeBar stays under the MessageListView at 60% width. The right panel has three states:

```
ThreadPanel ::= Closed | List | Open(thread_root_id)
```

| State | Right panel content | Main message list |
|-------|---------------------|-------------------|
| `Closed` | hidden (full-width main list) | normal |
| `List` | `ThreadListView` — rows of threads in the room | normal |
| `Open(id)` | `ThreadView` — header + thread timeline + own ComposeBar | dimmed + root row highlighted + scrolled into view |

### State machine

| Trigger | Transition | Side effects |
|---------|------------|--------------|
| RoomHeader "threads" button (toggle) | `Closed → List` / `List → Closed` | `subscribe_room_threads` / `unsubscribe_room_threads` |
| Thread row clicked in ThreadListView | `List → Open(id)` | `subscribe_thread(id)` (list stays subscribed) |
| Thread preview chip clicked in MessageListView | `* → Open(id)` | if was `Open(old)`: `unsubscribe_thread(old)` first; then `subscribe_thread(id)` |
| ThreadView back/close button | `Open → previous` | `unsubscribe_thread(current)`; if previous was `Closed`: also `unsubscribe_room_threads` |
| Room switch | `* → Closed` | release every current sub |
| Logout / SDK restart | `* → Closed` | (already covered by existing teardown) |

The "previous" field is captured at every transition into `Open`. There is no persistence across room switches — the panel always reopens `Closed` when the user returns to a room.

### New widgets

Three new files under `ui/shared/views/`:

**`ThreadView.{h,cpp}`** — vertical stack mirroring `RoomView`:
- `ThreadHeader` (close button + root-message preview: avatar / sender / body snippet / reply count)
- `MessageListView` (reused via composition; populated only with the thread reply timeline)
- `ComposeBar` (reused; sends route to `Client::send_thread_message(room_id, root_event_id, …)`)
- Public API mirrors RoomView's: `set_thread(root_event_id, root_preview)`, `set_messages(rows, switch)`, `insert/update/remove_message`, `clear()`; callbacks: `on_close`, `on_send(body, formatted)`, `on_message_clicked`, `on_reply_requested`, etc.

**`ThreadListView.{h,cpp}`** — lightweight panel:
- Small header: "Threads" title + close button
- `tk::ListView` of `ThreadRow` widgets — one per `ThreadInfo` from `Client::list_room_threads(room_id)`. Row shows root preview, latest reply preview (sender + snippet), reply count, latest timestamp.
- Callbacks: `on_close`, `on_thread_clicked(root_event_id)`, `on_near_bottom` (drives `paginate_room_threads`).

**MessageListView changes** (no new widget — additions to existing file):
- **Filter:** drop rows whose `thread_root_id` is non-empty from `set_messages` / `insert_message` inputs. (Thread roots themselves have `thread_root_id == ""` and stay.) The shell also short-circuits these calls (see "ShellBase plumbing" below) — the filter here is defence in depth.
- **Thread preview chip:** when a row has `is_thread_root && thread_reply_count > 0`, render a compact chip immediately below the row showing `thread_latest_sender_name` + `thread_latest_body` snippet + reply count. Clickable → fires new callback `on_thread_preview_clicked(root_event_id)`.
- **Dim/highlight state:** new methods `set_dimmed(bool)` (paints a semi-transparent dark overlay over the list bounds) and `set_highlighted_event(event_id)` (paints a coloured outline on that row; drawn *after* the dim layer so the highlight is visible). Both clear on `set_messages(.., room_switch=true)`. The highlighted row + its preview chip remain interactive while dimmed.

### ShellBase plumbing

**New state** in `ShellBase`:

```cpp
enum class ThreadPanel { Closed, List, Open };
ThreadPanel thread_panel_      = ThreadPanel::Closed;
ThreadPanel thread_panel_prev_ = ThreadPanel::Closed;
std::string current_thread_root_;  // non-empty only when thread_panel_ == Open
```

No per-room or per-thread caching for v1 (no persistence across room switches).

**Event routing** (in `ShellBase` UI-thread handlers added/extended):
- `handle_thread_reset_ui_(room_id, root, snapshot)` / `_inserted_ui_` / `_updated_ui_` / `_removed_ui_` — if `room_id == current_room_id_ && root == current_thread_root_`, dispatch to `room_view_->thread_view()->...`. Otherwise drop (no cache → re-subscription will resync).
- `handle_threads_updated_ui_(room_id)` — if `room_id == current_room_id_ && thread_panel_ != Closed`, re-fetch `Client::list_room_threads(room_id)` and push into the `ThreadListView`.
- `handle_message_inserted_ui_` / `_updated_ui_` — if `ev.thread_root_id` is non-empty, skip the `room_view_->insert_message(...)` call (in-thread reply doesn't belong in the main list). Existing cache-invalidation rules from commit `9354490` still apply unchanged.

**RoomView orchestration** (no business logic in RoomView — it just lays out and forwards):
- `set_thread_panel(ThreadPanel state, std::string root_id)` — toggles layout, creates/destroys `ThreadView` / `ThreadListView` children, drives `MessageListView::set_dimmed` + `set_highlighted_event` + `scroll_to_event_id` for the root row.
- New callbacks routed up to shell: `on_threads_button_clicked`, `on_thread_open_requested(root_id)`, `on_thread_close_requested`, `on_thread_send(body, formatted_body)`.

**RoomHeader changes:**
- New "threads" button rendered immediately left of the existing calendar button. Same hit-test / press-release pattern. Wires `on_threads_requested` callback → forwarded by RoomView → shell handles state transition.

### Send path

The ThreadView's ComposeBar's send callback routes through the shell to `Client::send_thread_message(current_room_id_, current_thread_root_, body, formatted_body)`. Replies-within-thread (a user replying to a specific in-thread message) use `Client::send_thread_reply(...)` — same path as today's reply UI but with the thread root id supplied. Media sends use the existing `send_image`/`send_video`/etc. APIs with their `thread_root` parameter set.

## Files Changed

| Action | Path |
|--------|------|
| Create | `ui/shared/views/ThreadView.{h,cpp}` |
| Create | `ui/shared/views/ThreadListView.{h,cpp}` |
| Modify | `ui/shared/views/MessageListView.{h,cpp}` (filter + preview chip + dim/highlight + new callback) |
| Modify | `ui/shared/views/RoomView.{h,cpp}` (split layout + thread-panel orchestration) |
| Modify | `ui/shared/views/RoomHeader.{h,cpp}` (threads button) |
| Modify | `ui/shared/app/ShellBase.{h,cpp}` (state, event routing, lifecycle wiring) |
| Modify | `ui/shared/app/EventHandlerBase.{h,cpp}` (override + dispatch new thread callbacks) |
| Create | `tests/cpp/test_tk_thread_view.cpp` (widget tests, Catch2) |
| Create | `tests/cpp/test_tk_thread_list_view.cpp` (widget tests) |
| Create | `tests/cpp/test_shell_thread_panel.cpp` (state-machine + event-routing tests) |
| Modify | `tests/cpp/test_tk_message_list.cpp` or new file (filter + preview chip + dim/highlight tests) |
| Modify | `tests/CMakeLists.txt` (register new test files) |

No per-shell changes — all four shells (Qt6, GTK4, Win32, macOS) inherit the behaviour through `ShellBase` and the shared widget tree.

## Verification

- `ctest --test-dir build/linux-qt6-debug --output-on-failure` — full suite passes, new `[thread*]`-tagged tests included.
- Manual on `build/linux-qt6-debug/ui/linux-qt/tesseract`:
  1. Open a room with no threads — threads button toggles an empty `ThreadListView`.
  2. Send a reply-in-thread from another client to a message — root row gets a preview chip; click it; ThreadView opens with the reply; main list dims; root row highlights and scrolls into view.
  3. Send a reply from the thread's ComposeBar — appears in ThreadView, does *not* appear in the main list. Thread preview chip updates with the new latest reply (driven by `on_threads_updated`).
  4. Open ThreadListView; pick a different thread; ThreadView swaps to that one, highlight + scroll target updates accordingly.
  5. Click thread back: returns to ThreadListView (since that's where we came from); click again: returns to `Closed`.
  6. Switch rooms: panel forced to `Closed`; come back: still `Closed`.
