# Pinned Events

**Date:** 2026-05-27

## Context

Matrix supports room-level pinned messages via the `m.room.pinned_events` state event (a list of event IDs that the room moderators have chosen to surface to all members). Tesseract has no UI or SDK plumbing for this today: the feature is greenfield. This spec adds it.

User-visible behaviour: a thin banner above the message list shows the most recently pinned message; counter + arrows step through additional pins; clicking the banner scrolls the main list to that event and flashes a highlight outline. Users with sufficient power level (PL ≥ room's `state_default`, typically 50) can pin or unpin any message via the existing hover-action row on the row itself.

## Design

### UI

**`PinnedBanner` (new widget, `ui/shared/views/PinnedBanner.{h,cpp}`)**
- ~44 px tall, full main-list width, parented by `RoomView` between `RoomHeader` and `MessageListView`. Visible only when the current room has ≥ 1 pinned event.
- Layout left → right: small pin icon, `<sender_name>: <body_preview>` (truncated to ~60 chars with ellipsis), spacer, `<i>/<n>` counter, up chevron, down chevron.
- Click anywhere on the body fires `on_jump_to(event_id)`. RoomView wires that to `message_list_->scroll_to_event_id(event_id)` + `message_list_->set_highlighted_event(event_id)` (both already in place). The highlight clears automatically on next `set_messages(.., room_switch=true)`.
- Click on chevrons steps `current_index_` (clamped to `0..pins.size()-1`); banner re-renders with that pin's preview.
- Internal state: `pins: std::vector<PinnedEvent>`, `current_index_`. `set_pins(vec)` replaces both and clamps the index (so a removed pin doesn't leave the index pointing past the end). `current_index_` resets to `0` on room switch (RoomView calls `set_pins({})` then the new vector).
- No "dismiss" button in v1.

**`MessageListView` additions** (no new widget)
- `void set_pinned_event_ids(std::unordered_set<std::string> ids)` — replaces the cached set; triggers a repaint.
- `void set_can_pin(bool can_pin)` — toggles whether the pin/unpin button shows in the hover-action row.
- New callbacks: `std::function<void(const std::string&)> on_pin_requested;` and `…on_unpin_requested;`. Wired by the hover-action click handler: if the hovered row's `event_id` is in the pinned set → fire `on_unpin_requested`, otherwise → fire `on_pin_requested`.
- Both new flags clear (as existing) on `set_messages(.., room_switch=true)` so room switches start fresh.

**`RoomView` orchestration**
- Owns a `std::unique_ptr<PinnedBanner>` child (lazy — only created on first `set_pinned(...)` with a non-empty list).
- New public methods: `set_pinned(std::vector<PinnedEvent>)`, `set_can_pin(bool)`. Both fan out: the banner gets the full enriched list; MessageListView gets the id-only set (built once at the call site).
- `arrange` block: when `pinned_banner_` is present and visible, it consumes `kPinnedBannerH = 44 px` immediately below `header_bottom`; the message list's `list_top` becomes `header_bottom + kPinnedBannerH` instead of `header_bottom`. The right-side thread panel (60/40 split) stays as today — the banner only narrows the left column's vertical extent.
- New callbacks forwarded to the shell: `on_pin_requested(event_id)`, `on_unpin_requested(event_id)`. RoomView installs lambdas on the embedded `MessageListView` that forward to these.

### Rust SDK

**New submodule `sdk/src/client/pins.rs`** (parallel to the existing per-feature splits):

```rust
impl ClientFfi {
    // Reads m.room.pinned_events; appends event_id if not already present;
    // writes back. Server enforces M_FORBIDDEN when the user lacks PL.
    pub fn pin_event(&mut self, room_id: &str, event_id: &str) -> OpResult;

    // Reads m.room.pinned_events; removes event_id if present; writes back.
    pub fn unpin_event(&mut self, room_id: &str, event_id: &str) -> OpResult;

    // True iff the current user's PL meets the room's m.room.pinned_events
    // requirement (typically state_default = 50). Read from cached
    // m.room.power_levels — no network round-trip.
    pub fn can_pin_in_room(&self, room_id: &str) -> bool;
}
```

Pattern: `pin_event` / `unpin_event` mirror `set_room_topic` (read existing state via `room.get_state_event_static::<RoomPinnedEventsEventContent>()`, mutate the `pinned` list, `room.send_state_event(content)`). The append/remove preserves order (newest at the end is the Matrix convention; banner reverses to show newest first).

**`RoomInfo` enrichment** in `sdk/src/client/room_list.rs`:

```rust
pub struct PinnedEvent {
    pub event_id: String,
    pub sender_name: String,
    pub body_preview: String,   // first ~80 chars; "(image)" / "(file)" /
                                // "(sticker)" / "(deleted)" for non-text
    pub timestamp: u64,         // unix-ms; used to order newest-first in the
                                // banner regardless of the on-wire order
}
```

`build_room_infos` resolves each pinned event id via the event cache (`room.event_cache().event(&event_id)`); unresolvable ids keep their `event_id`, `sender_name = ""`, `body_preview = "(unavailable)"`, `timestamp = 0`. The banner still works for unresolvable pins (shows placeholder text + jump still attempts the scroll, which becomes a no-op if the event hasn't been loaded).

### FFI bridge

In `sdk/src/bridge.rs`:
- Add a new shared `struct PinnedEvent` mirroring the Rust struct (same four fields).
- Add `pinned_events: Vec<PinnedEvent>` to the cxx-bridged `RoomInfo`.
- Add three new `extern "Rust"` method declarations on `ClientFfi`: `pin_event`, `unpin_event`, `can_pin_in_room`.

### C++ client wrapper

In `client/include/tesseract/types.h`:

```cpp
struct PinnedEvent {
    std::string event_id;
    std::string sender_name;
    std::string body_preview;
    std::uint64_t timestamp = 0;
};
// Added to RoomInfo:
std::vector<PinnedEvent> pinned_events;
```

In `client/include/tesseract/client.h`:

```cpp
Result pin_event(const std::string& room_id, const std::string& event_id);
Result unpin_event(const std::string& room_id, const std::string& event_id);
bool   can_pin_in_room(const std::string& room_id);
```

In `client/src/ffi_convert.h`:
- A `from_ffi(const tesseract_ffi::PinnedEvent&)` helper.
- Extend the existing `from_ffi(const tesseract_ffi::RoomInfo&)` to copy the new vector field through.

**No new `IEventHandler` callback.** Pin changes are state events, so they flow through the existing `on_rooms_updated` → `push_rooms_` → `ShellBase::handle_rooms_updated_ui_` → eventually `RoomView::set_pinned(...)` path. The threads UI established this exact pattern (`on_threads_updated` is room-scoped and similarly piped through).

### ShellBase wiring

- Public methods: `on_pin_requested(event_id)` / `on_unpin_requested(event_id)` — call `client_->pin_event` / `unpin_event` with `current_room_id_`. Failures (returned `Result::ok == false`) are surfaced via the existing transient-status callback (the same channel the send-failure path uses today).
- On every `RoomInfo` update for `current_room_id_`: build a `std::unordered_set<std::string>` of pinned ids, call `room_view_->set_pinned(events_vec)` (banner-side), and recompute `room_view_->set_can_pin(client_->can_pin_in_room(current_room_id_))`. Membership / PL events also fire `on_rooms_updated`, so the can-pin bit stays current.
- Each platform shell wires two new RoomView callbacks (`on_pin_requested`, `on_unpin_requested`) to the corresponding ShellBase entry points — same one-liner-per-shell pattern the threads UI established.

## Files Changed

| Action | Path |
|--------|------|
| Create | `sdk/src/client/pins.rs` |
| Modify | `sdk/src/client/mod.rs` (add `mod pins;`) |
| Modify | `sdk/src/client/room_list.rs` (PinnedEvent build during `build_room_infos`) |
| Modify | `sdk/src/bridge.rs` (PinnedEvent struct + 3 new fn decls + RoomInfo field) |
| Modify | `client/include/tesseract/types.h` (PinnedEvent struct + RoomInfo field) |
| Modify | `client/include/tesseract/client.h` (3 new methods) |
| Modify | `client/src/client.cpp` (3 new method impls) |
| Modify | `client/src/ffi_convert.h` (from_ffi for PinnedEvent + RoomInfo update) |
| Create | `ui/shared/views/PinnedBanner.{h,cpp}` |
| Modify | `ui/shared/views/MessageListView.{h,cpp}` (set_pinned_event_ids + set_can_pin + pin/unpin hover button + 2 callbacks) |
| Modify | `ui/shared/views/RoomView.{h,cpp}` (PinnedBanner child + set_pinned/set_can_pin + arrange split + callback wiring) |
| Modify | `ui/shared/app/ShellBase.{h,cpp}` (on_pin_requested / on_unpin_requested entry points + RoomInfo→RoomView fan-out) |
| Modify | `ui/shared/CMakeLists.txt` (compile PinnedBanner.cpp) |
| Modify | `ui/linux-qt/src/MainWindow.cpp` (wire 2 RoomView callbacks) |
| Modify | `ui/linux-gtk/src/MainWindow.cpp` (wire 2 RoomView callbacks) |
| Modify | `ui/windows/src/MainWindow.cpp` (wire 2 RoomView callbacks) |
| Modify | `ui/macos/src/MainWindowController.mm` (wire 2 RoomView callbacks) |
| Create | `tests/cpp/test_tk_pinned_banner.cpp` |
| Create | `tests/cpp/test_message_list_pinning.cpp` (extends pin-hover tests) |
| Modify | `tests/cpp/test_event_handler_bridge.cpp` (PinnedEvent round-trip) |
| Modify | `tests/CMakeLists.txt` (register new test files) |
| Modify | Rust tests in `pins.rs` (state-event read/write content shape) |

## Verification

- `cargo test -p tesseract-sdk-ffi --manifest-path sdk/Cargo.toml --lib` — Rust tests pass.
- `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure` — full suite passes, new pin tests included.
- Manual on `./build/linux-qt6-debug/ui/linux-qt/tesseract`:
  1. Open a room with no pins → no banner visible.
  2. Hover a message; if you have PL ≥ 50 → "Pin" button shows; click → banner appears with that message previewed; the in-row hover button on the same message now says "Unpin".
  3. Pin a second message from another client (or the same client) → banner's counter changes to `2/2`; chevrons enabled; click chevron → cycles preview.
  4. Click banner body → main list scrolls to that pinned event and the row gets a highlight outline.
  5. Unpin the displayed message → banner advances to the next pin; if it was the last one → banner disappears.
  6. Switch to a different room → banner reflects the new room's pins (or hidden if none); banner index resets to `0`.
  7. As a user with PL < 50 → no Pin button visible on hover; banner still shows existing pins normally.
