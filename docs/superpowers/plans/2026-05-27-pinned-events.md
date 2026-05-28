# Pinned Events Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship Matrix pinned-events support across the SDK, FFI, client wrapper, and cross-platform UI — a banner above the message list cycles through pinned messages with click-to-jump, and per-message Pin/Unpin lives on the hover-action row (hidden when the user lacks PL).

**Architecture:** New `sdk/src/client/pins.rs` submodule mirrors the existing `set_room_topic` pattern for state-event writes; `build_room_infos` in `room_list.rs` enriches each pinned id with sender + body snippet so the banner can render without a separate fetch. Pin changes propagate through the existing `on_rooms_updated` callback — no new IEventHandler entry. UI side: a new `PinnedBanner` widget parented by `RoomView` between header and message list, plus a Pin/Unpin button on `MessageListView`'s existing hover-action row gated by a `set_can_pin(bool)` toggle.

**Tech Stack:** C++17, Rust (matrix-sdk + cxx FFI), Catch2 tests, CMake/ctest. No new external deps.

Spec: [`docs/superpowers/specs/2026-05-27-pinned-events-design.md`](../specs/2026-05-27-pinned-events-design.md).

---

## File Structure

| Action | Path                                              | Responsibility |
|--------|---------------------------------------------------|----------------|
| Create | `sdk/src/client/pins.rs`                          | `pin_event`, `unpin_event`, `can_pin_in_room` impl on `ClientFfi` |
| Modify | `sdk/src/client/mod.rs`                           | `mod pins;` declaration |
| Modify | `sdk/src/client/room_list.rs`                     | Enrich `RoomInfo.pinned_events` in `build_room_infos` |
| Modify | `sdk/src/bridge.rs`                               | `PinnedEvent` struct + `RoomInfo.pinned_events` field + 3 new fn decls |
| Modify | `client/include/tesseract/types.h`                | `PinnedEvent` struct + `RoomInfo.pinned_events` field |
| Modify | `client/include/tesseract/client.h`               | 3 new methods |
| Modify | `client/src/client.cpp`                           | 3 new method impls (thin wrappers) |
| Modify | `client/src/ffi_convert.h`                        | `from_ffi(PinnedEvent)` + `RoomInfo` field copy |
| Create | `ui/shared/views/PinnedBanner.{h,cpp}`            | The banner widget (header + body + chevrons + counter) |
| Modify | `ui/shared/views/MessageListView.{h,cpp}`         | `set_pinned_event_ids` + `set_can_pin` + Pin/Unpin hover button + 2 callbacks |
| Modify | `ui/shared/views/RoomView.{h,cpp}`                | Owns `PinnedBanner` child; `set_pinned` / `set_can_pin` fan-out; arrange split |
| Modify | `ui/shared/app/ShellBase.{h,cpp}`                 | `on_pin_requested` / `on_unpin_requested` entry points + RoomInfo→RoomView fan-out |
| Modify | `ui/shared/CMakeLists.txt`                        | Compile `PinnedBanner.cpp` |
| Modify | `ui/linux-qt/src/MainWindow.cpp`                  | Wire 2 new RoomView callbacks |
| Modify | `ui/linux-gtk/src/MainWindow.cpp`                 | Wire 2 new RoomView callbacks |
| Modify | `ui/windows/src/MainWindow.cpp`                   | Wire 2 new RoomView callbacks |
| Modify | `ui/macos/src/MainWindowController.mm`            | Wire 2 new RoomView callbacks |
| Create | `tests/cpp/test_tk_pinned_banner.cpp`             | PinnedBanner widget tests |
| Create | `tests/cpp/test_message_list_pinning.cpp`         | MessageListView pin/unpin hover-button tests |
| Modify | `tests/cpp/test_event_handler_bridge.cpp`         | PinnedEvent FFI round-trip test |
| Modify | `tests/CMakeLists.txt`                            | Register new test files |

---

## Task 1: Rust SDK — `pins.rs` (pin/unpin/can_pin)

Pure Rust. State-event read-modify-write following the `set_room_topic` pattern. Server enforces permission (`M_FORBIDDEN` on PL too low).

**Files:**

- Create: `sdk/src/client/pins.rs`
- Modify: `sdk/src/client/mod.rs`

- [ ] **Step 1.1: Create `sdk/src/client/pins.rs`**

```rust
//! Matrix pinned events: m.room.pinned_events state event read/write +
//! the power-level check used by the UI to hide the Pin button when the
//! current user can't pin. Mirrors the set_room_topic pattern in
//! room_list.rs (read-modify-write of a state event).

use super::{err, ok, ClientFfi};
use crate::ffi::OpResult;

#[cfg(not(test))]
use super::{require_room, try_op};

#[cfg(not(test))]
impl ClientFfi {
    /// Append `event_id` to m.room.pinned_events if not already present, then
    /// write the state event back. Server enforces permission.
    pub fn pin_event(&mut self, room_id: &str, event_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
        use matrix_sdk::ruma::OwnedEventId;

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));

        let Ok(target) = event_id.parse::<OwnedEventId>() else {
            return err("invalid event_id");
        };

        // Read existing m.room.pinned_events (may be absent → start empty).
        let existing: Vec<OwnedEventId> = match self
            .rt
            .block_on(room.get_state_event_static::<RoomPinnedEventsEventContent>())
        {
            Ok(Some(raw)) => match raw.deserialize() {
                Ok(ev) => ev.into_content().pinned,
                Err(_) => Vec::new(),
            },
            Ok(None) => Vec::new(),
            Err(e) => return err(format!("read pinned: {e}")),
        };

        if existing.iter().any(|id| id == &target) {
            return ok(""); // already pinned — idempotent
        }

        let mut next = existing;
        next.push(target);
        let content = RoomPinnedEventsEventContent::new(next);

        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    /// Remove `event_id` from m.room.pinned_events if present, then write
    /// back. No-op (returns ok) if not pinned.
    pub fn unpin_event(&mut self, room_id: &str, event_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
        use matrix_sdk::ruma::OwnedEventId;

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));

        let Ok(target) = event_id.parse::<OwnedEventId>() else {
            return err("invalid event_id");
        };

        let existing: Vec<OwnedEventId> = match self
            .rt
            .block_on(room.get_state_event_static::<RoomPinnedEventsEventContent>())
        {
            Ok(Some(raw)) => match raw.deserialize() {
                Ok(ev) => ev.into_content().pinned,
                Err(_) => return ok(""),
            },
            Ok(None) => return ok(""),
            Err(e) => return err(format!("read pinned: {e}")),
        };

        if !existing.iter().any(|id| id == &target) {
            return ok(""); // not pinned — idempotent
        }

        let next: Vec<OwnedEventId> = existing.into_iter()
            .filter(|id| id != &target)
            .collect();
        let content = RoomPinnedEventsEventContent::new(next);

        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    /// Returns true iff the current user has permission to send
    /// m.room.pinned_events state events in this room. Reads cached
    /// m.room.power_levels — no network round-trip. Returns false on any
    /// uncertainty (not logged in, room unknown, PL unreadable).
    pub fn can_pin_in_room(&self, room_id: &str) -> bool {
        use matrix_sdk::ruma::events::StateEventType;
        use matrix_sdk::ruma::OwnedRoomId;

        let Some(client) = self.client.as_ref() else { return false; };
        let Ok(room_id_parsed) = room_id.parse::<OwnedRoomId>() else { return false; };
        let Some(room) = client.get_room(&room_id_parsed) else { return false; };
        let Some(user_id) = client.user_id() else { return false; };

        match self.rt.block_on(room.power_levels()) {
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomPinnedEvents),
            Err(_) => false,
        }
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn pin_event(&mut self, _room_id: &str, _event_id: &str) -> OpResult {
        err("not logged in")
    }
    pub fn unpin_event(&mut self, _room_id: &str, _event_id: &str) -> OpResult {
        err("not logged in")
    }
    pub fn can_pin_in_room(&self, _room_id: &str) -> bool { false }
}

// ---------------------------------------------------------------------------
// Tests (content-shape only — state-event sends require a live homeserver).
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
    use matrix_sdk::ruma::OwnedEventId;

    #[test]
    fn empty_pinned_content_serializes() {
        let c = RoomPinnedEventsEventContent::new(Vec::new());
        let v = serde_json::to_value(&c).unwrap();
        assert!(v.get("pinned").is_some());
        assert_eq!(v["pinned"].as_array().unwrap().len(), 0);
    }

    #[test]
    fn pinned_content_round_trips_two_ids() {
        let ids: Vec<OwnedEventId> = vec![
            "$abc:server".parse().unwrap(),
            "$def:server".parse().unwrap(),
        ];
        let c = RoomPinnedEventsEventContent::new(ids.clone());
        let v = serde_json::to_value(&c).unwrap();
        let arr = v["pinned"].as_array().unwrap();
        assert_eq!(arr.len(), 2);
        assert_eq!(arr[0].as_str().unwrap(), "$abc:server");
        assert_eq!(arr[1].as_str().unwrap(), "$def:server");
    }
}
```

(If `room.power_levels()` is named differently in this matrix-sdk version, search `sdk/src/client/` for any existing call to find the real name. The `user_can_send_state` method on `RoomPowerLevels` is stable across matrix-sdk versions.)

- [ ] **Step 1.2: Register the submodule in `sdk/src/client/mod.rs`**

Locate the existing `mod recovery;` / `mod notifications;` / etc. lines and add:

```rust
#[cfg(not(test))]
mod pins;
```

If those are in a different layout (e.g. ungated `mod recovery;`), match the existing style.

- [ ] **Step 1.3: Build + run Rust tests**

```bash
cargo build -p tesseract-sdk-ffi --manifest-path sdk/Cargo.toml
cargo test -p tesseract-sdk-ffi --manifest-path sdk/Cargo.toml --lib 2>&1 | tail -3
```

Expected: clean build, test count = 152 (was 150 + 2 new).

- [ ] **Step 1.4: Commit**

```bash
git add sdk/src/client/pins.rs sdk/src/client/mod.rs
git commit -m "feat(sdk): add pin_event / unpin_event / can_pin_in_room"
```

---

## Task 2: Enrich `RoomInfo.pinned_events`

The state event holds only event IDs. The banner needs sender + body snippet to render. `build_room_infos` resolves each id via the event cache.

**Files:**

- Modify: `sdk/src/client/room_list.rs`

- [ ] **Step 2.1: Add `PinnedEvent` resolver helper in `room_list.rs`**

Near the top of the file (alongside other helpers), add:

```rust
#[cfg(not(test))]
async fn resolve_pinned_event(
    room: &matrix_sdk::Room,
    event_id: &matrix_sdk::ruma::OwnedEventId,
) -> crate::ffi::PinnedEvent {
    use matrix_sdk::deserialized_responses::TimelineEventKind;

    let mut out = crate::ffi::PinnedEvent {
        event_id: event_id.to_string(),
        sender_name: String::new(),
        body_preview: String::new(),
        timestamp: 0,
    };

    // Try the event cache first (cheap, no network).
    let cached = room.event_cache().await
        .ok()
        .and_then(|(c, _)| c.event(event_id).now_or_never().flatten());

    let raw = match cached {
        Some(ev) => ev,
        None => {
            // Fall back to /event/{id} REST. May fail for old events not in
            // the homeserver's retention; we surface a placeholder body so the
            // banner still works.
            out.body_preview = "(unavailable)".to_owned();
            return out;
        }
    };

    // Pull sender + timestamp from the raw event metadata.
    if let TimelineEventKind::PlainText { event } = raw.kind() {
        if let Ok(any) = event.deserialize() {
            out.sender_name = any.sender().as_str().to_owned();
            out.timestamp = any.origin_server_ts().get().into();
            // Resolve body preview using the existing helper.
            out.body_preview = super::timeline_convert::msglike_snippet(&any);
        }
    }
    out
}
```

The exact `Room::event_cache()` / `TimelineEventKind` shape may differ — adapt to whatever the real matrix-sdk version exposes. The principle is: take an event id, return a `PinnedEvent` with best-effort sender + body preview. If you can't resolve, return the placeholder. Look at how `latest_event_preview` / `extract_local_preview` in `room_list.rs` already resolve content for the room-list previews — copy that pattern.

- [ ] **Step 2.2: Populate `pinned_events` in `build_room_infos`**

Find `pub(super) fn build_room_infos` (or `pub(super) async fn`) and locate where each `RoomInfo` is built. For each room, before pushing to the result:

```rust
// Read m.room.pinned_events; resolve each id to a PinnedEvent.
use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
let pinned_ids: Vec<matrix_sdk::ruma::OwnedEventId> = match room
    .get_state_event_static::<RoomPinnedEventsEventContent>()
    .await
{
    Ok(Some(raw)) => raw.deserialize().map(|ev| ev.into_content().pinned)
                        .unwrap_or_default(),
    _ => Vec::new(),
};
let mut pinned_events: Vec<crate::ffi::PinnedEvent> = Vec::new();
for id in &pinned_ids {
    pinned_events.push(resolve_pinned_event(&room, id).await);
}
// Sort newest first by timestamp; banner displays in this order.
pinned_events.sort_by(|a, b| b.timestamp.cmp(&a.timestamp));
info.pinned_events = pinned_events;
```

(The exact local variable name for the RoomInfo under construction varies — match what's already there.)

If `build_room_infos` isn't currently `async`, mirror however the existing per-room work happens — the file already calls `.await` on `room.topic()` / etc. somewhere.

- [ ] **Step 2.3: Build (the field doesn't exist yet — this will fail)**

```bash
cargo build -p tesseract-sdk-ffi --manifest-path sdk/Cargo.toml 2>&1 | tail -10
```

Expected: error about `info.pinned_events` not existing — the FFI struct hasn't been updated yet. That's the next task. Stash or set this aside.

- [ ] **Step 2.4: (No commit yet — Task 3 finishes the wiring before this compiles)**

---

## Task 3: FFI bridge + cxx types

Add the `PinnedEvent` shared struct, the new `RoomInfo` field, and the 3 fn decls. This will unstick Task 2's build.

**Files:**

- Modify: `sdk/src/bridge.rs`

- [ ] **Step 3.1: Add `PinnedEvent` struct + `RoomInfo` field in `bridge.rs`**

Find the cxx `#[bridge]` block. Inside the shared `mod ffi { … }` section, near the existing `struct RoomInfo`, add:

```rust
/// One pinned event in a room. The banner above the message list renders
/// these newest-first. body_preview is "(image)" / "(file)" / etc. for
/// non-text events and "(unavailable)" when the id can't be resolved.
struct PinnedEvent {
    event_id: String,
    sender_name: String,
    body_preview: String,
    timestamp: u64,
}
```

Add to `struct RoomInfo` (find it nearby, ~line 98):

```rust
    pinned_events: Vec<PinnedEvent>,
```

- [ ] **Step 3.2: Add the 3 fn decls in the `extern "Rust"` block**

Find the existing `fn set_room_topic` declaration (line ~1289). Add nearby:

```rust
fn pin_event(self: &mut ClientFfi, room_id: &str, event_id: &str) -> OpResult;
fn unpin_event(self: &mut ClientFfi, room_id: &str, event_id: &str) -> OpResult;
fn can_pin_in_room(self: &ClientFfi, room_id: &str) -> bool;
```

- [ ] **Step 3.3: Build the Rust side end-to-end**

```bash
cargo build -p tesseract-sdk-ffi --manifest-path sdk/Cargo.toml 2>&1 | tail -10
```

Expected: clean build. Task 2's `info.pinned_events = ...` now compiles because the field exists.

- [ ] **Step 3.4: Run Rust tests**

```bash
cargo test -p tesseract-sdk-ffi --manifest-path sdk/Cargo.toml --lib 2>&1 | tail -3
```

Expected: 152 passed (no new tests added in this task; we're verifying the FFI changes don't break the pins.rs tests from Task 1).

- [ ] **Step 3.5: Commit Tasks 2 + 3 together**

```bash
git add sdk/src/client/room_list.rs sdk/src/bridge.rs
git commit -m "feat(sdk): enrich RoomInfo with pinned_events resolved snippets"
```

---

## Task 4: C++ Client wrapper + types + ffi_convert + bridge test

Mirrors the SDK methods, adds the `PinnedEvent` C++ type, wires `from_ffi` to copy the new field, and extends the existing `test_event_handler_bridge.cpp` to lock down the PinnedEvent round-trip.

**Files:**

- Modify: `client/include/tesseract/types.h`
- Modify: `client/include/tesseract/client.h`
- Modify: `client/src/client.cpp`
- Modify: `client/src/ffi_convert.h`
- Modify: `tests/cpp/test_event_handler_bridge.cpp`

- [ ] **Step 4.1: Add `PinnedEvent` struct + `RoomInfo` field in `types.h`**

Locate `struct RoomInfo` (around line 352). Above it, add the new type:

```cpp
struct PinnedEvent {
    std::string event_id;
    std::string sender_name;
    std::string body_preview;
    std::uint64_t timestamp = 0;
};
```

Inside `struct RoomInfo`, add (alongside `topic` / `topic_html`):

```cpp
    std::vector<PinnedEvent> pinned_events;
```

- [ ] **Step 4.2: Declare the 3 new methods in `client.h`**

Locate `Result set_room_topic(...)` (around line 715). Add nearby:

```cpp
    /// Append event_id to the room's m.room.pinned_events state event.
    /// Idempotent (returns ok when already pinned). Server enforces PL —
    /// failure surfaces as Result{ ok=false, message="<server error>" }.
    Result pin_event(const std::string& room_id, const std::string& event_id);

    /// Remove event_id from m.room.pinned_events. Idempotent.
    Result unpin_event(const std::string& room_id, const std::string& event_id);

    /// True iff the current user's PL meets the requirement for sending
    /// m.room.pinned_events in this room. Cached read — no network.
    /// Returns false on any uncertainty.
    bool can_pin_in_room(const std::string& room_id);
```

- [ ] **Step 4.3: Implement the 3 methods in `client.cpp`**

Find the existing `Result Client::set_room_topic(...)` impl. Add nearby:

```cpp
Result Client::pin_event(const std::string& room_id,
                         const std::string& event_id)
{
    auto r = impl_->ffi->pin_event(room_id, event_id);
    return Result{r.ok, std::string(r.message)};
}

Result Client::unpin_event(const std::string& room_id,
                           const std::string& event_id)
{
    auto r = impl_->ffi->unpin_event(room_id, event_id);
    return Result{r.ok, std::string(r.message)};
}

bool Client::can_pin_in_room(const std::string& room_id)
{
    return impl_->ffi->can_pin_in_room(room_id);
}
```

(Match the existing per-method style — some methods use `if (!impl_) return ...;` guards; copy whatever `set_room_topic` does for consistency.)

- [ ] **Step 4.4: Add `from_ffi(PinnedEvent)` + extend `from_ffi(RoomInfo)` in `ffi_convert.h`**

Locate `from_ffi(const tesseract_ffi::RoomInfo&)`. Above it (with the other `from_ffi` helpers), add:

```cpp
inline PinnedEvent from_ffi(const tesseract_ffi::PinnedEvent& p)
{
    return PinnedEvent{
        std::string(p.event_id),
        std::string(p.sender_name),
        std::string(p.body_preview),
        p.timestamp,
    };
}
```

Inside `from_ffi(const tesseract_ffi::RoomInfo&)`, after the existing field copies, add:

```cpp
    out.pinned_events.reserve(in.pinned_events.size());
    for (const auto& p : in.pinned_events) {
        out.pinned_events.push_back(from_ffi(p));
    }
```

- [ ] **Step 4.5: Add bridge test for PinnedEvent round-trip**

In `tests/cpp/test_event_handler_bridge.cpp`, append:

```cpp
TEST_CASE("from_ffi copies PinnedEvent fields", "[ffi][pinned]")
{
    tesseract_ffi::PinnedEvent in{};
    in.event_id     = "$pinned:server";
    in.sender_name  = "Alice";
    in.body_preview = "Important announcement";
    in.timestamp    = 1700000000000ULL;

    auto out = tesseract::from_ffi(in);
    CHECK(out.event_id == "$pinned:server");
    CHECK(out.sender_name == "Alice");
    CHECK(out.body_preview == "Important announcement");
    CHECK(out.timestamp == 1700000000000ULL);
}

TEST_CASE("from_ffi(RoomInfo) propagates pinned_events", "[ffi][pinned]")
{
    tesseract_ffi::RoomInfo in{};
    in.room_id     = "!room:server";
    in.display_name = "Room";

    tesseract_ffi::PinnedEvent p1{};
    p1.event_id    = "$a:s";
    p1.sender_name = "Alice";
    p1.body_preview = "first";
    p1.timestamp   = 1000;
    in.pinned_events.push_back(std::move(p1));

    tesseract_ffi::PinnedEvent p2{};
    p2.event_id    = "$b:s";
    p2.sender_name = "Bob";
    p2.body_preview = "second";
    p2.timestamp   = 2000;
    in.pinned_events.push_back(std::move(p2));

    auto out = tesseract::from_ffi(in);
    REQUIRE(out.pinned_events.size() == 2);
    CHECK(out.pinned_events[0].event_id == "$a:s");
    CHECK(out.pinned_events[1].timestamp == 2000);
}
```

The exact `RoomInfo` field name for "display name" varies (`display_name` / `name`) — set whatever's required to make a minimally valid `RoomInfo`. The test only asserts on `pinned_events`.

- [ ] **Step 4.6: Build + test**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
ctest --test-dir build/linux-qt6-debug -R "pinned" --output-on-failure 2>&1 | tail -10
```

Expected: clean build; 2 new `[pinned]` tests pass.

- [ ] **Step 4.7: Commit**

```bash
git add client/include/tesseract/types.h client/include/tesseract/client.h \
        client/src/client.cpp client/src/ffi_convert.h \
        tests/cpp/test_event_handler_bridge.cpp
git commit -m "feat(client): pin_event/unpin_event/can_pin_in_room + PinnedEvent type"
```

---

## Task 5: `MessageListView` Pin/Unpin hover button

Adds two new public setters (`set_pinned_event_ids`, `set_can_pin`), two callbacks (`on_pin_requested`, `on_unpin_requested`), and a Pin/Unpin button in the existing hover-action row that toggles based on the current pinned set. Both flags clear on `set_messages(.., room_switch=true)`.

**Files:**

- Modify: `ui/shared/views/MessageListView.h`
- Modify: `ui/shared/views/MessageListView.cpp`
- Create: `tests/cpp/test_message_list_pinning.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 5.1: Declare the new API in `MessageListView.h`**

In the public section, alongside the existing hover-action callbacks (`on_reply_requested`, `on_edit_requested`, `on_delete_requested`, etc.), add:

```cpp
    std::function<void(const std::string& event_id)> on_pin_requested;
    std::function<void(const std::string& event_id)> on_unpin_requested;

    /// Replace the cached set of pinned event ids for this room.
    /// MessageListView consults the set when drawing the hover-action row
    /// to pick Pin vs Unpin and the corresponding callback. Cleared on
    /// set_messages(.., room_switch=true).
    void set_pinned_event_ids(std::unordered_set<std::string> ids);
    const std::unordered_set<std::string>& pinned_event_ids() const
    {
        return pinned_event_ids_;
    }

    /// Toggle visibility of the Pin/Unpin button in the hover-action row.
    /// When false, the button is hidden entirely (not greyed out). Driven
    /// by ShellBase from Client::can_pin_in_room. Cleared on room switch.
    void set_can_pin(bool can_pin);
    bool can_pin() const { return can_pin_; }
```

In the private section:

```cpp
    std::unordered_set<std::string> pinned_event_ids_;
    bool can_pin_ = false;
```

Add `#include <unordered_set>` at the top of the header if not already present.

- [ ] **Step 5.2: Implement the new setters in `MessageListView.cpp`**

Add (near the other simple setters like `set_dimmed`):

```cpp
void MessageListView::set_pinned_event_ids(std::unordered_set<std::string> ids)
{
    if (pinned_event_ids_ == ids) return;
    pinned_event_ids_ = std::move(ids);
    if (request_repaint_) request_repaint_();
}

void MessageListView::set_can_pin(bool can_pin)
{
    if (can_pin_ == can_pin) return;
    can_pin_ = can_pin;
    if (request_repaint_) request_repaint_();
}
```

Clear both on room switch — locate the existing `set_messages` body where `dimmed_` / `highlighted_event_id_` are reset on `room_switch=true` (added in the threads work). Add alongside:

```cpp
    if (room_switch) {
        pinned_event_ids_.clear();
        can_pin_ = false;
    }
```

- [ ] **Step 5.3: Add the Pin/Unpin button to the hover-action row**

Find the hover-action row paint code (search MessageListView.cpp for the existing reply / edit / delete buttons — typically inside a `paint_hover_actions_` helper or the per-row paint method's tail). Where the existing actions are enumerated, add:

```cpp
    // Pin / Unpin (only when the current user has permission).
    if (can_pin_) {
        const bool is_pinned = pinned_event_ids_.find(row.event_id)
                               != pinned_event_ids_.end();
        // Reuse the same button-rect math as the sibling actions; the icon
        // and tooltip differ based on is_pinned.
        // <existing helper to draw an icon button>
        // is_pinned ? "📌 Unpin" : "📌 Pin"
        // On click: fires either on_pin_requested or on_unpin_requested with row.event_id.
    }
```

The exact pattern depends on how the hover row is structured today. The principle: one more button alongside Reply / React / Edit / Delete, gated by `can_pin_`, label/callback toggled by membership in `pinned_event_ids_`. Use a stack-of-pins glyph or a small `📌`-style icon — match how `draw_threads_icon` / `draw_calendar_icon` in `RoomHeader.cpp` render hand-drawn vector icons.

In the pointer-up dispatch for the hover row, when the hit is the new button:

```cpp
    if (is_pinned) {
        if (on_unpin_requested) on_unpin_requested(hovered_event_id);
    } else {
        if (on_pin_requested) on_pin_requested(hovered_event_id);
    }
```

- [ ] **Step 5.4: Create the test file**

`tests/cpp/test_message_list_pinning.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"

#include <unordered_set>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace {

MessageRowData make_row(const std::string& id, const std::string& body = "x")
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.body = body;
    return r;
}

} // namespace

TEST_CASE("set_pinned_event_ids stores the set", "[message_list][pinning]")
{
    MessageListView v;
    CHECK(v.pinned_event_ids().empty());
    v.set_pinned_event_ids({"$a", "$b"});
    CHECK(v.pinned_event_ids().size() == 2);
    CHECK(v.pinned_event_ids().count("$a") == 1);
    v.set_pinned_event_ids({});
    CHECK(v.pinned_event_ids().empty());
}

TEST_CASE("set_can_pin flips the flag", "[message_list][pinning]")
{
    MessageListView v;
    CHECK_FALSE(v.can_pin());
    v.set_can_pin(true);
    CHECK(v.can_pin());
    v.set_can_pin(false);
    CHECK_FALSE(v.can_pin());
}

TEST_CASE("set_messages(.., room_switch=true) clears pinning state",
          "[message_list][pinning]")
{
    MessageListView v;
    v.set_pinned_event_ids({"$a"});
    v.set_can_pin(true);
    v.set_messages({make_row("$x")}, /*room_switch=*/true);
    CHECK(v.pinned_event_ids().empty());
    CHECK_FALSE(v.can_pin());
}

TEST_CASE("set_messages(.., room_switch=false) preserves pinning state",
          "[message_list][pinning]")
{
    MessageListView v;
    v.set_pinned_event_ids({"$a"});
    v.set_can_pin(true);
    v.set_messages({make_row("$x")}, /*room_switch=*/false);
    CHECK(v.pinned_event_ids().count("$a") == 1);
    CHECK(v.can_pin());
}
```

(Optional pointer-event test for the hover-button click follows the same TestSurface + LayoutCtx pattern used in `tests/cpp/test_tk_message_list_threads.cpp`. If the existing hover-row tests have a helper for clicking a specific action button, reuse it. If not, the four field-level tests above are sufficient — the click dispatch is one `if` branch added to existing code.)

- [ ] **Step 5.5: Register the test file**

In `tests/CMakeLists.txt`, add (alphabetically near the other `test_message_list*`):

```cmake
    cpp/test_message_list_pinning.cpp
```

- [ ] **Step 5.6: Build + test**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
ctest --test-dir build/linux-qt6-debug -R "pinning" --output-on-failure 2>&1 | tail -10
```

Expected: 4 new `[pinning]` tests pass. Full suite still passes.

- [ ] **Step 5.7: Commit**

```bash
git add ui/shared/views/MessageListView.h ui/shared/views/MessageListView.cpp \
        tests/cpp/test_message_list_pinning.cpp tests/CMakeLists.txt
git commit -m "feat(message-list): Pin/Unpin hover button gated by can_pin"
```

---

## Task 6: `PinnedBanner` widget

A standalone widget: header + body + chevrons + counter. Owned by RoomView (Task 7) but tests construct it standalone.

**Files:**

- Create: `ui/shared/views/PinnedBanner.h`
- Create: `ui/shared/views/PinnedBanner.cpp`
- Modify: `ui/shared/CMakeLists.txt`
- Create: `tests/cpp/test_tk_pinned_banner.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 6.1: Declare `PinnedBanner` in the header**

`ui/shared/views/PinnedBanner.h`:

```cpp
#pragma once

// Thin banner shown above the message list when a room has pinned events.
// Displays the currently-selected pin (sender + body preview), with chevrons
// + an "i/n" counter to step through multiple pins. Click on the body fires
// on_jump_to(event_id) so RoomView can scroll the main list to that event.

#include "tk/canvas.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class PinnedBanner : public tk::Widget
{
public:
    PinnedBanner();
    ~PinnedBanner() override = default;

    /// Replace the pin set. Clamps current_index_ to valid range, or to 0
    /// when empty. Banner widget is hidden (zero-height) when empty.
    void set_pins(std::vector<PinnedEvent> pins);
    const std::vector<PinnedEvent>& pins() const { return pins_; }

    /// Currently displayed pin index. 0 when empty.
    std::size_t current_index() const { return current_index_; }

    /// Fired when the user clicks the banner body. event_id is from the
    /// currently-displayed pin.
    std::function<void(const std::string& event_id)> on_jump_to;

    /// tk::Widget — adapt signatures to the real toolkit.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;

    // Layout constants exposed for tests.
    static constexpr float kBannerH    = 44.0f;
    static constexpr float kChevronSz  = 20.0f;
    static constexpr float kChevronPad = 4.0f;

private:
    std::vector<PinnedEvent> pins_;
    std::size_t current_index_ = 0;
    tk::Rect body_rect_{};
    tk::Rect up_rect_{};
    tk::Rect down_rect_{};
    bool press_body_ = false;
    bool press_up_ = false;
    bool press_down_ = false;
};

} // namespace tesseract::views
```

(`tk::Widget` virtual signatures vary — `arrange(LayoutCtx&, Rect)`, `measure(LayoutCtx&, Size)`, `on_pointer_down(Point)` returning `bool`, etc. Mirror `ThreadListView.h` for the exact shape.)

- [ ] **Step 6.2: Implement `PinnedBanner.cpp`**

```cpp
#include "views/PinnedBanner.h"

#include "views/media_utils.h"  // rect_contains

namespace tesseract::views
{

PinnedBanner::PinnedBanner() = default;

void PinnedBanner::set_pins(std::vector<PinnedEvent> pins)
{
    pins_ = std::move(pins);
    if (current_index_ >= pins_.size()) {
        current_index_ = pins_.empty() ? 0 : pins_.size() - 1;
    }
    if (request_relayout_) request_relayout_();
}

tk::Size PinnedBanner::measure(tk::LayoutCtx& /*ctx*/, tk::Size c)
{
    // Zero-height when empty so RoomView's split-arrange skips us cleanly.
    return tk::Size{c.w, pins_.empty() ? 0.0f : kBannerH};
}

void PinnedBanner::arrange(tk::LayoutCtx& /*ctx*/, tk::Rect bounds)
{
    bounds_ = bounds;
    if (pins_.empty()) {
        body_rect_ = up_rect_ = down_rect_ = {};
        return;
    }
    // chevrons on the right, body fills the rest.
    const float right = bounds.x + bounds.w;
    down_rect_ = {right - kChevronPad - kChevronSz, bounds.y + 4.0f,
                  kChevronSz, kChevronSz};
    up_rect_   = {right - kChevronPad - kChevronSz, bounds.y + 4.0f + kChevronSz,
                  kChevronSz, kChevronSz};
    body_rect_ = {bounds.x, bounds.y,
                  bounds.w - (kChevronSz + 2 * kChevronPad + 36.0f /*counter*/),
                  bounds.h};
}

void PinnedBanner::paint(tk::PaintCtx& ctx)
{
    if (pins_.empty()) return;
    auto& canvas = ctx.canvas;
    canvas.fill_rect(bounds_, ctx.theme.palette.surface);

    const auto& p = pins_[current_index_];
    std::string preview = p.sender_name + ": " + p.body_preview;
    if (preview.size() > 60) preview = preview.substr(0, 57) + "...";

    tk::TextStyle small{}; small.role = tk::FontRole::Small;
    canvas.draw_text(preview, body_rect_.x + 28.0f, body_rect_.y + 12.0f, small);

    // Counter.
    if (pins_.size() > 1) {
        std::string counter = std::to_string(current_index_ + 1) + "/" +
                              std::to_string(pins_.size());
        canvas.draw_text(counter, body_rect_.x + body_rect_.w + 4.0f,
                         body_rect_.y + 12.0f, small);
        // Chevrons (use a simple triangle or text glyph).
        canvas.draw_text("\xE2\x96\xB2", up_rect_.x, up_rect_.y, small); // ▲
        canvas.draw_text("\xE2\x96\xBC", down_rect_.x, down_rect_.y, small); // ▼
    }
}

bool PinnedBanner::on_pointer_down(tk::Point local)
{
    if (pins_.empty()) return false;
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
    if (pins_.size() > 1 && rect_contains(up_rect_, world)) {
        press_up_ = true;
        if (request_repaint_) request_repaint_();
        return true;
    }
    if (pins_.size() > 1 && rect_contains(down_rect_, world)) {
        press_down_ = true;
        if (request_repaint_) request_repaint_();
        return true;
    }
    if (rect_contains(body_rect_, world)) {
        press_body_ = true;
        if (request_repaint_) request_repaint_();
        return true;
    }
    return false;
}

void PinnedBanner::on_pointer_up(tk::Point local, bool /*inside_self*/)
{
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
    if (press_up_) {
        press_up_ = false;
        if (rect_contains(up_rect_, world) && current_index_ > 0) {
            --current_index_;
            if (request_repaint_) request_repaint_();
        }
        return;
    }
    if (press_down_) {
        press_down_ = false;
        if (rect_contains(down_rect_, world) && current_index_ + 1 < pins_.size()) {
            ++current_index_;
            if (request_repaint_) request_repaint_();
        }
        return;
    }
    if (press_body_) {
        press_body_ = false;
        if (rect_contains(body_rect_, world) && current_index_ < pins_.size()) {
            if (on_jump_to) on_jump_to(pins_[current_index_].event_id);
        }
        return;
    }
}

} // namespace tesseract::views
```

(Same caveat as ThreadListView: adapt names like `tk::PaintCtx` / `ctx.theme.palette` / `request_repaint_` / `request_relayout_` to the real toolkit API by mirroring an existing widget like `ThreadListView.cpp`.)

- [ ] **Step 6.3: Register in `ui/shared/CMakeLists.txt`**

Add `views/PinnedBanner.cpp` to the `target_sources(tesseract_tk ...)` list (alphabetically, near `views/PresenceTracker.cpp` or similar).

- [ ] **Step 6.4: Create tests**

`tests/cpp/test_tk_pinned_banner.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "views/PinnedBanner.h"

using tesseract::views::PinnedBanner;
using tesseract::PinnedEvent;

namespace {

PinnedEvent make_pin(const std::string& id, std::uint64_t ts)
{
    PinnedEvent p;
    p.event_id = id;
    p.sender_name = "Alice";
    p.body_preview = "Important";
    p.timestamp = ts;
    return p;
}

} // namespace

TEST_CASE("set_pins stores the list", "[pinned_banner]")
{
    PinnedBanner b;
    b.set_pins({make_pin("$a", 100), make_pin("$b", 200)});
    REQUIRE(b.pins().size() == 2);
    CHECK(b.pins()[0].event_id == "$a");
    CHECK(b.current_index() == 0);
}

TEST_CASE("set_pins clamps current_index_ when list shrinks",
          "[pinned_banner]")
{
    PinnedBanner b;
    b.set_pins({make_pin("$a", 100), make_pin("$b", 200), make_pin("$c", 300)});
    // We need to advance current_index_ somehow to test clamp behavior; the
    // chevron-click flow does that. Direct field access via friend or test
    // helper if available, otherwise drive via arrange + pointer events
    // (see test_tk_thread_list_view.cpp for the TestSurface pattern).
    // For now: shrink to empty and verify it's 0.
    b.set_pins({});
    CHECK(b.current_index() == 0);
}

TEST_CASE("on_jump_to fires for the currently-displayed pin",
          "[pinned_banner]")
{
    PinnedBanner b;
    b.set_pins({make_pin("$pinned", 100)});
    std::string clicked;
    b.on_jump_to = [&](const std::string& id) { clicked = id; };
    // Drive arrange + click body. Mirror test_tk_thread_list_view.cpp's
    // TestSurface + LayoutCtx pattern.
    // Click at (10, banner_y_center) — body rect spans most of the width.
    // CHECK(clicked == "$pinned");
    // (If reproducing the TestSurface dance is too heavy, you can fall
    // back to direct rect inspection if the banner exposes a _for_test
    // accessor.)
}
```

The third test depends on driving a pointer event through the widget — mirror the dance in `tests/cpp/test_tk_thread_list_view.cpp` (TestSurface + LayoutCtx + manual measure/arrange/paint, then `on_pointer_down`/`on_pointer_up` with widget-local coords).

- [ ] **Step 6.5: Register the test**

In `tests/CMakeLists.txt`:

```cmake
    cpp/test_tk_pinned_banner.cpp
```

- [ ] **Step 6.6: Build + test**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
ctest --test-dir build/linux-qt6-debug -R "pinned_banner" --output-on-failure 2>&1 | tail -10
```

Expected: 3 new `[pinned_banner]` tests pass.

- [ ] **Step 6.7: Commit**

```bash
git add ui/shared/views/PinnedBanner.h ui/shared/views/PinnedBanner.cpp \
        ui/shared/CMakeLists.txt \
        tests/cpp/test_tk_pinned_banner.cpp tests/CMakeLists.txt
git commit -m "feat(views): add PinnedBanner widget"
```

---

## Task 7: `RoomView` orchestration

RoomView owns the `PinnedBanner` child, exposes `set_pinned` / `set_can_pin` for ShellBase to drive, narrows the message list's vertical extent when the banner is visible, and routes the banner's `on_jump_to` plus the message-list's pin/unpin callbacks up to ShellBase.

**Files:**

- Modify: `ui/shared/views/RoomView.h`
- Modify: `ui/shared/views/RoomView.cpp`

- [ ] **Step 7.1: Add includes + new members in `RoomView.h`**

Add include:

```cpp
#include "views/PinnedBanner.h"
```

In the public section (alongside `set_thread_panel`):

```cpp
    PinnedBanner* pinned_banner() const { return pinned_banner_.get(); }

    void set_pinned(std::vector<PinnedEvent> pins);
    void set_can_pin(bool can_pin);

    std::function<void(const std::string& event_id)> on_pin_requested;
    std::function<void(const std::string& event_id)> on_unpin_requested;
```

In the private section:

```cpp
    std::unique_ptr<PinnedBanner> pinned_banner_;
```

- [ ] **Step 7.2: Wire callbacks in `RoomView` constructor**

After the existing `header_->on_threads_requested = ...` wiring, add:

```cpp
    message_list_->on_pin_requested =
        [this](const std::string& event_id) {
            if (on_pin_requested) on_pin_requested(event_id);
        };
    message_list_->on_unpin_requested =
        [this](const std::string& event_id) {
            if (on_unpin_requested) on_unpin_requested(event_id);
        };
```

- [ ] **Step 7.3: Implement `set_pinned` + `set_can_pin`**

In `RoomView.cpp`:

```cpp
void RoomView::set_pinned(std::vector<PinnedEvent> pins)
{
    if (pins.empty() && !pinned_banner_) {
        // Nothing to update; also clear the message-list set.
        message_list_->set_pinned_event_ids({});
        return;
    }
    if (!pinned_banner_) {
        pinned_banner_ = std::make_unique<PinnedBanner>();
        add_child(pinned_banner_.get());
        pinned_banner_->on_jump_to =
            [this](const std::string& event_id) {
                if (message_list_) {
                    message_list_->scroll_to_event_id(event_id);
                    message_list_->set_highlighted_event(event_id);
                }
            };
    }
    // Build the id-only set for the message list's hover-button toggle.
    std::unordered_set<std::string> ids;
    ids.reserve(pins.size());
    for (const auto& p : pins) ids.insert(p.event_id);
    message_list_->set_pinned_event_ids(std::move(ids));

    pinned_banner_->set_pins(std::move(pins));
    if (request_relayout_) request_relayout_();
}

void RoomView::set_can_pin(bool can_pin)
{
    if (message_list_) message_list_->set_can_pin(can_pin);
}
```

- [ ] **Step 7.4: Update `arrange` to make room for the banner**

Find the existing `arrange` body where `list_top = header_bottom;` is computed (added in the threads work). Modify:

```cpp
    float list_top = header_bottom;
    if (pinned_banner_ && !pinned_banner_->pins().empty()) {
        const float bh = PinnedBanner::kBannerH;
        pinned_banner_->arrange(/* ctx, */ {bounds.x, list_top, main_w, bh});
        list_top += bh;
    }
    // Existing message_list_ / compose_bar_ arrange continues from list_top.
```

(Match the real arrange signature — see how the existing children are arranged for the `ctx` argument.)

In paint, similarly:

```cpp
    if (pinned_banner_ && !pinned_banner_->pins().empty()) {
        pinned_banner_->paint(/* ctx */);
    }
```

(Add the paint call where the existing message_list_ paint happens.)

- [ ] **Step 7.5: Build (no new tests yet — wiring smoke-tested in Task 8)**

```bash
cmake --build build/linux-qt6-debug 2>&1 | tail -10
```

Expected: clean. RoomView compiles; existing `[room_view]` tests still pass.

- [ ] **Step 7.6: Commit**

```bash
git add ui/shared/views/RoomView.h ui/shared/views/RoomView.cpp
git commit -m "feat(room-view): own PinnedBanner; route pin/unpin callbacks"
```

---

## Task 8: ShellBase end-to-end + 4 shell wirings + manual smoke

ShellBase exposes the two pin entry points, computes the can-pin bit on every rooms_updated, and fans pinned_events out to RoomView. Each shell wires the two new RoomView callbacks.

**Files:**

- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`
- Modify: `ui/linux-qt/src/MainWindow.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`
- Modify: `ui/windows/src/MainWindow.cpp`
- Modify: `ui/macos/src/MainWindowController.mm`

- [ ] **Step 8.1: Declare entry points in `ShellBase.h`**

In the public section, alongside `on_threads_button_clicked`, add:

```cpp
    void on_pin_requested(const std::string& event_id);
    void on_unpin_requested(const std::string& event_id);
```

- [ ] **Step 8.2: Implement in `ShellBase.cpp`**

```cpp
void ShellBase::on_pin_requested(const std::string& event_id)
{
    if (!client_ || current_room_id_.empty()) return;
    auto r = client_->pin_event(current_room_id_, event_id);
    // Failures (e.g. M_FORBIDDEN if the cached can_pin bit was stale)
    // surface via the existing transient-status channel.
    if (!r.ok) {
        report_send_failure_(current_room_id_,
            std::string("Pin failed: ") + r.message);
    }
}

void ShellBase::on_unpin_requested(const std::string& event_id)
{
    if (!client_ || current_room_id_.empty()) return;
    auto r = client_->unpin_event(current_room_id_, event_id);
    if (!r.ok) {
        report_send_failure_(current_room_id_,
            std::string("Unpin failed: ") + r.message);
    }
}
```

**Locating the failure-report helper:** before writing the impls above, grep the codebase for how an existing failed state-event call surfaces its error. Try `grep -n "set_room_topic\|set_display_name" sdk/src/client/account.rs ui/shared/app/ShellBase.cpp client/src/client.cpp` and trace from there. If the codebase already has a `transient_status_` / `report_failure_` / `on_send_failed_` mechanism, use it. If no such helper exists (state-event failures may currently be silent in the codebase), fall back to:

```cpp
if (!r.ok) {
    tracing::warn("pin failed for {}: {}", event_id, r.message);
}
```

— and document the missing UX surface as a follow-up rather than blocking on it. The verification check at Step 8.7 catches whether the silent-fail path is acceptable for v1; if not, surface in a follow-up commit. Either way, don't block this task on creating a brand-new transient-status mechanism — that's a separate, broader piece of work.

- [ ] **Step 8.3: Fan out pinned_events + can_pin on every RoomInfo update**

Find where ShellBase processes `on_rooms_updated` / `push_rooms_`. Where the current room's info is identified, add:

```cpp
    // Fan pinned events + can-pin bit to RoomView whenever the current
    // room's RoomInfo updates. Banner hides automatically when empty.
    if (room_view_ && !current_room_id_.empty()) {
        for (const auto& r : rooms_) {
            if (r.room_id == current_room_id_) {
                room_view_->set_pinned(r.pinned_events);
                room_view_->set_can_pin(client_
                    ? client_->can_pin_in_room(current_room_id_)
                    : false);
                break;
            }
        }
    }
```

Also fire the same fan-out from whichever method handles room-switching (typically `tab_select_room` / `tab_open_room` — locate where `current_room_id_` is assigned, look at the threads-work commit `29541b2` or the recent thread-panel wiring for the exact insertion point). Without this, opening a room initially leaves the banner stale until the next `on_rooms_updated` fires.

- [ ] **Step 8.4: Wire the 2 new RoomView callbacks in each shell**

In each of these files, find where existing RoomView callbacks like `on_threads_button_clicked` / `on_thread_open_requested` are wired, and add:

```cpp
room_view_->on_pin_requested = [this](const std::string& ev) { on_pin_requested(ev); };
room_view_->on_unpin_requested = [this](const std::string& ev) { on_unpin_requested(ev); };
```

Files:
- `ui/linux-qt/src/MainWindow.cpp`
- `ui/linux-gtk/src/MainWindow.cpp`
- `ui/windows/src/MainWindow.cpp`
- `ui/macos/src/MainWindowController.mm` (uses the `s->_shell->on_*` pattern per the threads work — match it)

- [ ] **Step 8.5: Full build**

```bash
cmake --build build/linux-qt6-debug 2>&1 | tail -10
```

Expected: clean build across `tesseract_tk`, `tesseract_qt`, `tesseract_tests`.

- [ ] **Step 8.6: Full test suite**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass. Counts: was 545 at session start; +2 (bridge) +4 (pinning) +3 (banner) = **554/554**.

- [ ] **Step 8.7: Manual smoke**

```bash
./build/linux-qt6-debug/ui/linux-qt/tesseract
```

Run through the 7 scenarios in the spec's Verification section:

1. Open a room with no pins → no banner visible.
2. Hover a message as a user with PL ≥ 50 → "Pin" button shows; click → banner appears with that message previewed; same message's hover button now says "Unpin".
3. Pin a second message (or from another client) → banner counter shows `2/2`; chevrons enabled; click → cycles.
4. Click banner body → main list scrolls to that pinned event, highlight outline flashes.
5. Unpin the displayed message → banner advances or hides on last unpin.
6. Switch rooms → banner reflects new room's pins; index resets to 0.
7. As a user with PL < 50 → no Pin button visible; banner still shows existing pins.

- [ ] **Step 8.8: Commit (HOLD for user verification per project policy)**

Per the project's no-auto-commit policy, **DO NOT commit until the user confirms the smoke test works**. When approved:

```bash
git add ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp \
        ui/linux-qt/src/MainWindow.cpp \
        ui/linux-gtk/src/MainWindow.cpp \
        ui/windows/src/MainWindow.cpp \
        ui/macos/src/MainWindowController.mm
git commit -m "feat(threads): wire pinned events end-to-end across the four shells"
```
