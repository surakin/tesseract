# Expose Matrix Threads to the C++ Level — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire matrix-sdk-ui 0.17 thread support through the `cxx` FFI bridge and the C++ `client/` layer — thread metadata on events, a thread-focused timeline subscription, a per-room thread list, and threaded sending — with no `ui/` changes.

**Architecture:** Reuse the existing per-room timeline diff engine (`spawn_timeline_tasks` / `handle_timeline_diff`) by parameterizing it with a `TimelineChannel { Room, Thread(root) }` enum, and add 4 thread-scoped callbacks plus `on_threads_updated`, all as default-no-op C++ `IEventHandler` virtuals so the shells compile untouched. Thread metadata is added to the existing `TimelineEvent`/`Event` structs; threaded sending uses ruma `Relation::Thread` (text) and `AttachmentConfig::reply` (media).

**Tech Stack:** Rust (matrix-sdk 0.17, matrix-sdk-ui 0.17, ruma-events 0.33, `cxx`), C++17 (`client/` library), Catch2/ctest, `cargo test`.

**Design spec:** `docs/superpowers/specs/2026-05-22-expose-matrix-threads-to-cpp-design.md`

---

## Background the engineer needs

- The Rust↔C++ boundary is a `#[cxx::bridge]` in `sdk/src/bridge.rs`. It defines plain-data structs (`TimelineEvent`, `RoomInfo`, …), the `ClientFfi` methods C++ calls, and an `extern "C++"` block of `EventHandlerBridge` callbacks Rust calls.
- `sdk/src/lib.rs` has a **second** copy of these shapes inside `#[cfg(test)] pub mod ffi` — pure-Rust stubs so `cargo test` runs without a C++ toolchain. **Every struct field or `EventHandlerBridge` method you add to the bridge must be mirrored in this stub**, or `cargo test` won't compile.
- `sdk/src/client.rs` implements `ClientFfi`. Most network methods have a real `#[cfg(not(test))]` body and a `#[cfg(test)]` stub returning `err("not logged in")`. Follow that pattern.
- `client/src/event_handler_bridge.cpp` (+ `.h`) implements the C++ side of the callbacks, converts FFI structs via `tesseract::make_event` (in `client/src/ffi_convert.h`), and forwards to an `IEventHandler*`.
- `client/include/tesseract/event_handler.h` declares `IEventHandler`. New callbacks added there as virtuals **with default empty bodies** require no shell changes.
- `client/include/tesseract/client.h` (+ `client/src/client.cpp`) is the thin `Client` wrapper; methods are one-liners: `return from_ffi(impl_->ffi->method(...));`.
- `client/include/tesseract/types.h` holds the C++ `Event` struct (+ subtypes); `client/src/ffi_convert.h::assign_base` copies every base field from `TimelineEvent` onto an `Event`.

### Build & test commands

```bash
# Rust unit tests (no C++ needed):
cargo test -p tesseract-sdk-ffi

# Full C++ build + ctest:
cmake --preset linux-qt6-debug
cmake --build build/linux-qt6-debug
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

> Run `cargo test -p tesseract-sdk-ffi` after every Rust task. Run the cmake build after C++ tasks (it compiles the bridge + client + ctest target). The C++ build also recompiles the Rust crate via Corrosion, so it surfaces any bridge/stub mismatch.

### Confirmed SDK API (pinned versions)

- `content().thread_root() -> Option<OwnedEventId>`; `content().thread_summary() -> Option<ThreadSummary>`.
- `ThreadSummary { latest_event: TimelineDetails<Box<EmbeddedEvent>>, num_replies: u32, .. }`.
- `TimelineFocus::Thread { root_event_id: OwnedEventId }`.
- `ThreadListService::{new(room), items() -> Vec<ThreadListItem>, paginate(), subscribe_to_items_updates(), pagination_state(), reset()}`.
  `ThreadListItem { root_event: ThreadListItemEvent, latest_event: Option<ThreadListItemEvent>, num_replies: u32 }`;
  `ThreadListItemEvent { event_id, timestamp: MilliSecondsSinceUnixEpoch, sender: OwnedUserId, is_own, sender_profile: TimelineDetails<Profile>, content: Option<TimelineItemContent> }`.
- ruma `ruma::events::room::message::relation::Thread::{without_fallback(root), reply(root, reply_to)}`; `Relation::Thread(Thread)`.
- `matrix_sdk::room::reply::{Reply { event_id, enforce_thread, add_mentions }, EnforceThread::{Threaded(ReplyWithinThread), Unthreaded}}`; `ReplyWithinThread::{Yes, No}`; `AttachmentConfig::reply(Option<Reply>)`.

---

## Phase 1 — Thread metadata on timeline events (capability A)

### Task 1: Add thread fields to the FFI `TimelineEvent` and populate the converter

**Files:**
- Modify: `sdk/src/bridge.rs` (the `struct TimelineEvent` in the `#[cxx::bridge]`)
- Modify: `sdk/src/lib.rs` (the `#[cfg(test)]` stub `struct TimelineEvent`)
- Modify: `sdk/src/client.rs` (converter `timeline_item_to_ffi` ~line 6484; 5 `TimelineEvent { .. }` literals at ~6500, ~6604, ~6682, ~6778, ~7402)
- Test: `sdk/src/client.rs` (existing `#[cfg(test)] mod tests`)

- [ ] **Step 1: Add the six fields to the bridge struct.** In `sdk/src/bridge.rs`, inside `struct TimelineEvent { … }`, after the `location_description: String,` field add:

```rust
        // ----- MSC3440 threads -----
        /// Event ID of the thread root when this event is an in-thread reply
        /// (`content().thread_root()`); empty otherwise.
        thread_root_id: String,
        /// True when this event roots a thread (`content().thread_summary().is_some()`).
        is_thread_root: bool,
        /// Replies in the thread excluding the root (`ThreadSummary.num_replies`).
        /// 0 when this event is not a thread root.
        thread_reply_count: u64,
        /// Display name of the latest thread reply's sender (resolved profile,
        /// bare Matrix ID fallback). Empty when not a root or summary not ready.
        thread_latest_sender_name: String,
        /// Snippet of the latest thread reply ("(image)" etc. for non-text).
        /// Empty when not a root or summary not ready.
        thread_latest_body: String,
        /// Unix-ms timestamp of the latest thread reply. 0 when unavailable.
        thread_latest_ts: u64,
```

- [ ] **Step 2: Mirror the six fields in the test stub.** In `sdk/src/lib.rs`, inside the `#[cfg(test)] pub mod ffi`'s `struct TimelineEvent`, after `location_description: String,` add the identical six fields (same names/types, no doc comments needed).

- [ ] **Step 3: Populate the four non-message literals with empty defaults.** In `sdk/src/client.rs`, each of the four `return Some(TimelineEvent { … })` literals at ~6500 (virtual), ~6604 (UTD), ~6682, and ~6778 — add these six lines before the closing `})` (alongside `location_description`):

```rust
            thread_root_id: String::new(),
            is_thread_root: false,
            thread_reply_count: 0,
            thread_latest_sender_name: String::new(),
            thread_latest_body: String::new(),
            thread_latest_ts: 0,
```

(Indentation: match the surrounding fields in each literal.)

- [ ] **Step 4: Compute thread metadata in the main converter.** In `sdk/src/client.rs`, in `timeline_item_to_ffi`, immediately **before** the final `Some(TimelineEvent {` at ~7402 (after the `let reactions = …; let read_receipts = …;` lines), insert:

```rust
    // MSC3440 thread metadata.
    let thread_root_id = event_item
        .content()
        .thread_root()
        .map(|id| id.to_string())
        .unwrap_or_default();
    let (
        is_thread_root,
        thread_reply_count,
        thread_latest_sender_name,
        thread_latest_body,
        thread_latest_ts,
    ) = match event_item.content().thread_summary() {
        None => (false, 0u64, String::new(), String::new(), 0u64),
        Some(summary) => {
            let count = summary.num_replies as u64;
            let (name, body, ts) = match &summary.latest_event {
                TimelineDetails::Ready(embedded) => embedded_event_preview(embedded),
                _ => (String::new(), String::new(), 0u64),
            };
            (true, count, name, body, ts)
        }
    };
```

- [ ] **Step 5: Add the fields to the final literal.** In the same `Some(TimelineEvent { … })` at ~7402, before the closing `})`, after `location_description,` add:

```rust
        thread_root_id,
        is_thread_root,
        thread_reply_count,
        thread_latest_sender_name,
        thread_latest_body,
        thread_latest_ts,
```

- [ ] **Step 6: Add the `embedded_event_preview` helper.** The `ThreadSummary.latest_event` is `TimelineDetails<Box<EmbeddedEvent>>`. Add a free function near `timeline_item_to_ffi` (file-scope, `#[cfg(not(test))]`). The exact field/accessor names on `EmbeddedEvent` must be confirmed against `matrix-sdk-ui-0.17.0` (`src/timeline/event_item/mod.rs` / `content/mod.rs`) while implementing; the structure is:

```rust
#[cfg(not(test))]
fn embedded_event_preview(
    embedded: &matrix_sdk_ui::timeline::EmbeddedEvent,
) -> (String, String, u64) {
    // Sender display name: resolved profile, bare id fallback.
    let name = match &embedded.sender_profile {
        TimelineDetails::Ready(p) => p
            .display_name
            .clone()
            .unwrap_or_else(|| embedded.sender.to_string()),
        _ => embedded.sender.to_string(),
    };
    let body = msglike_snippet(&embedded.content);
    let ts: u64 = embedded.timestamp.get().into();
    (name, body, ts)
}
```

> If `EmbeddedEvent` exposes its content as `TimelineItemContent`, reuse the snippet logic. Extract the existing reply-snippet match (currently inline at ~7350) into a shared `fn msglike_snippet(content: &TimelineItemContent) -> String` returning the text body or `(image)`/`(file)`/`(voice)`/`(audio)`/`(video)`/`(sticker)`/`(deleted)`/`(message)`, and call it from both the `in_reply_to` block and `embedded_event_preview`. If `EmbeddedEvent`'s content shape differs, adapt the match arms but keep the same output strings.

- [ ] **Step 7: Add a converter unit test for the new fields' defaults.** The converter needs a live timeline, so it isn't directly unit-testable; instead assert the stub struct carries the fields (compile-level coverage) by adding to `sdk/src/client.rs`'s `mod tests`:

```rust
    #[test]
    fn timeline_event_has_thread_fields() {
        let ev = crate::ffi::TimelineEvent {
            thread_root_id: "$root:server".to_owned(),
            is_thread_root: true,
            thread_reply_count: 3,
            thread_latest_sender_name: "Alice".to_owned(),
            thread_latest_body: "hi".to_owned(),
            thread_latest_ts: 42,
            ..Default::default()
        };
        assert!(ev.is_thread_root);
        assert_eq!(ev.thread_reply_count, 3);
        assert_eq!(ev.thread_root_id, "$root:server");
        assert_eq!(ev.thread_latest_ts, 42);
    }
```

(The `#[cfg(test)]` stub derives `Default`, so `..Default::default()` works.)

- [ ] **Step 8: Run the Rust tests.**

Run: `cargo test -p tesseract-sdk-ffi`
Expected: PASS, including `timeline_event_has_thread_fields`.

- [ ] **Step 9: Commit.**

```bash
git add sdk/src/bridge.rs sdk/src/lib.rs sdk/src/client.rs
git commit -m "feat(sdk): expose MSC3440 thread metadata on TimelineEvent"
```

---

### Task 2: Mirror thread metadata onto the C++ `Event`

**Files:**
- Modify: `client/include/tesseract/types.h` (`struct Event`)
- Modify: `client/src/ffi_convert.h` (`assign_base`)
- Test: `tests/cpp/test_types.cpp`

- [ ] **Step 1: Add a failing C++ test.** In `tests/cpp/test_types.cpp`, add:

```cpp
TEST_CASE("Event thread fields default-initialise", "[types][thread]")
{
    tesseract::TextEvent ev;
    CHECK(ev.thread_root_id.empty());
    CHECK_FALSE(ev.is_thread_root);
    CHECK(ev.thread_reply_count == 0);
    CHECK(ev.thread_latest_sender_name.empty());
    CHECK(ev.thread_latest_body.empty());
    CHECK(ev.thread_latest_ts == 0);
}

TEST_CASE("Event thread fields are settable", "[types][thread]")
{
    tesseract::TextEvent ev;
    ev.thread_root_id = "$root:server";
    ev.is_thread_root = true;
    ev.thread_reply_count = 5;
    ev.thread_latest_sender_name = "Bob";
    ev.thread_latest_body = "latest";
    ev.thread_latest_ts = 99;
    CHECK(ev.is_thread_root);
    CHECK(ev.thread_reply_count == 5);
    CHECK(ev.thread_latest_body == "latest");
}
```

- [ ] **Step 2: Build to verify it fails.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | head -30`
Expected: compile error — `Event` has no member `thread_root_id`.

- [ ] **Step 3: Add the fields to `Event`.** In `client/include/tesseract/types.h`, in `struct Event`, after `bool is_edited = false;` (line ~104) add:

```cpp
    /// MSC3440 threads. Non-empty when this event is an in-thread reply.
    std::string thread_root_id;
    /// True when this event roots a thread.
    bool is_thread_root = false;
    /// Replies in the thread excluding the root; 0 when not a root.
    uint64_t thread_reply_count = 0;
    /// Latest thread reply preview (for a "N replies" affordance on the root).
    std::string thread_latest_sender_name;
    std::string thread_latest_body;
    uint64_t thread_latest_ts = 0;
```

- [ ] **Step 4: Copy them in `assign_base`.** In `client/src/ffi_convert.h`, in `assign_base`, after `ev.is_edited = e.is_edited;` (line ~169) add:

```cpp
    ev.thread_root_id = std::string(e.thread_root_id);
    ev.is_thread_root = e.is_thread_root;
    ev.thread_reply_count = e.thread_reply_count;
    ev.thread_latest_sender_name = std::string(e.thread_latest_sender_name);
    ev.thread_latest_body = std::string(e.thread_latest_body);
    ev.thread_latest_ts = e.thread_latest_ts;
```

- [ ] **Step 5: Build and run ctest.**

Run: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure -R "thread|types"`
Expected: PASS.

- [ ] **Step 6: Commit.**

```bash
git add client/include/tesseract/types.h client/src/ffi_convert.h tests/cpp/test_types.cpp
git commit -m "feat(client): mirror thread metadata onto C++ Event"
```

---

## Phase 2 — Threaded sending (capability D)

### Task 3: Threaded text send helpers + `send_thread_message` / `send_thread_reply` (Rust)

**Files:**
- Modify: `sdk/src/bridge.rs` (`extern "Rust"` method list)
- Modify: `sdk/src/client.rs` (impl + helper + tests)

- [ ] **Step 1: Add a failing helper test.** In `sdk/src/client.rs`'s `mod tests`, add:

```rust
    #[test]
    fn thread_relation_message_shape() {
        let val = build_thread_message_content("body", "", "$root:server", "");
        assert_eq!(val["msgtype"], "m.text");
        assert_eq!(val["body"], "body");
        assert_eq!(val["m.relates_to"]["rel_type"], "m.thread");
        assert_eq!(val["m.relates_to"]["event_id"], "$root:server");
    }

    #[test]
    fn thread_relation_reply_shape() {
        let val = build_thread_message_content("body", "", "$root:server", "$reply:server");
        assert_eq!(val["m.relates_to"]["rel_type"], "m.thread");
        assert_eq!(val["m.relates_to"]["event_id"], "$root:server");
        assert_eq!(
            val["m.relates_to"]["m.in_reply_to"]["event_id"],
            "$reply:server"
        );
    }
```

- [ ] **Step 2: Run to verify it fails.**

Run: `cargo test -p tesseract-sdk-ffi thread_relation`
Expected: FAIL — `build_thread_message_content` not found.

- [ ] **Step 3: Add the content-builder helper.** This is a pure function returning a `serde_json::Value` so it is unit-testable without a client. Add near the other `build_*_content` helpers in `sdk/src/client.rs`:

```rust
/// Build an `m.room.message` content object carrying an `m.thread` relation.
/// `thread_root` is the thread root event id. When `in_reply_to` is non-empty
/// the relation also carries an `m.in_reply_to` (a reply to a specific message
/// within the thread); otherwise it is a plain thread message.
fn build_thread_message_content(
    body: &str,
    formatted_body: &str,
    thread_root: &str,
    in_reply_to: &str,
) -> serde_json::Value {
    let mut content = serde_json::json!({
        "msgtype": "m.text",
        "body": body,
    });
    if !formatted_body.is_empty() {
        content["format"] = serde_json::json!("org.matrix.custom.html");
        content["formatted_body"] = serde_json::json!(formatted_body);
    }
    let mut relates = serde_json::json!({
        "rel_type": "m.thread",
        "event_id": thread_root,
    });
    if in_reply_to.is_empty() {
        // Thread message with no in-thread reply target: fall back to the root
        // for non-threaded clients, and mark we are not a "real" reply.
        relates["m.in_reply_to"] = serde_json::json!({ "event_id": thread_root });
        relates["is_falling_back"] = serde_json::json!(true);
    } else {
        relates["m.in_reply_to"] = serde_json::json!({ "event_id": in_reply_to });
    }
    content["m.relates_to"] = relates;
    content
}
```

> Rationale for raw JSON: it mirrors the existing `build_animated_image_content` pattern, keeps the relation unit-testable, and sends via `room.send_raw("m.room.message", content)` exactly like the animated-image path. (Equivalent typed alternative: `RoomMessageEventContent` + `Relation::Thread(Thread::without_fallback(root))` / `Thread::reply(root, reply_to)` — use whichever the implementer prefers, but keep the test asserting the wire shape above.)

- [ ] **Step 4: Add the two FFI methods to the bridge.** In `sdk/src/bridge.rs`, in the `extern "Rust"` block near `send_reply`, add:

```rust
        /// Send `body` as a message into the thread rooted at `thread_root`
        /// (MSC3440 `m.thread` relation). Does not require `subscribe_room`.
        fn send_thread_message(
            self: &mut ClientFfi,
            room_id: &str,
            thread_root: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Send `body` as a reply to `in_reply_to_event_id` *within* the thread
        /// rooted at `thread_root`. Does not require `subscribe_room`.
        fn send_thread_reply(
            self: &mut ClientFfi,
            room_id: &str,
            thread_root: &str,
            in_reply_to_event_id: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;
```

- [ ] **Step 5: Add the real + stub impls.** In `sdk/src/client.rs`, add to the `impl ClientFfi`:

```rust
    #[cfg(not(test))]
    pub fn send_thread_message(
        &mut self,
        room_id: &str,
        thread_root: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        self.send_thread_inner(room_id, thread_root, "", body, formatted_body)
    }

    #[cfg(not(test))]
    pub fn send_thread_reply(
        &mut self,
        room_id: &str,
        thread_root: &str,
        in_reply_to_event_id: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        if in_reply_to_event_id.is_empty() {
            return err("in_reply_to_event_id required");
        }
        self.send_thread_inner(
            room_id,
            thread_root,
            in_reply_to_event_id,
            body,
            formatted_body,
        )
    }

    #[cfg(not(test))]
    fn send_thread_inner(
        &mut self,
        room_id: &str,
        thread_root: &str,
        in_reply_to: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        if thread_root.parse::<matrix_sdk::ruma::OwnedEventId>().is_err() {
            return err("invalid thread root id");
        }
        if !in_reply_to.is_empty()
            && in_reply_to.parse::<matrix_sdk::ruma::OwnedEventId>().is_err()
        {
            return err("invalid in_reply_to id");
        }
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        let content =
            build_thread_message_content(body, formatted_body, thread_root, in_reply_to);
        match self
            .rt
            .block_on(async move { room.send_raw("m.room.message", content).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_thread_message(
        &mut self,
        _room_id: &str,
        _thread_root: &str,
        _body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(test)]
    pub fn send_thread_reply(
        &mut self,
        _room_id: &str,
        _thread_root: &str,
        _in_reply_to_event_id: &str,
        _body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }
```

- [ ] **Step 6: Add not-logged-in / invalid-id tests.** In `mod tests`:

```rust
    #[test]
    fn send_thread_message_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.send_thread_message("!room:server", "$root:server", "hi", "");
        assert!(!r.ok);
    }

    #[test]
    fn send_thread_reply_not_logged_in() {
        let mut c = ClientFfi::new();
        let r =
            c.send_thread_reply("!room:server", "$root:server", "$reply:server", "hi", "");
        assert!(!r.ok);
    }
```

- [ ] **Step 7: Run the Rust tests.**

Run: `cargo test -p tesseract-sdk-ffi thread`
Expected: PASS (helper + not-logged-in tests).

- [ ] **Step 8: Commit.**

```bash
git add sdk/src/bridge.rs sdk/src/client.rs
git commit -m "feat(sdk): send_thread_message / send_thread_reply via m.thread"
```

---

### Task 4: C++ `Client` wrappers for threaded text sends

**Files:**
- Modify: `client/include/tesseract/client.h`
- Modify: `client/src/client.cpp`

- [ ] **Step 1: Declare the methods.** In `client/include/tesseract/client.h`, near `send_reply` (line ~380) add:

```cpp
    /// Send `body` into the thread rooted at `thread_root` (MSC3440). Does not
    /// require subscribe_room.
    Result send_thread_message(const std::string& room_id,
                               const std::string& thread_root,
                               const std::string& body,
                               const std::string& formatted_body);

    /// Reply to `in_reply_to_event_id` within the thread rooted at
    /// `thread_root`. Does not require subscribe_room.
    Result send_thread_reply(const std::string& room_id,
                             const std::string& thread_root,
                             const std::string& in_reply_to_event_id,
                             const std::string& body,
                             const std::string& formatted_body);
```

- [ ] **Step 2: Implement them.** In `client/src/client.cpp`, near `Client::send_reply`:

```cpp
Result Client::send_thread_message(const std::string& room_id,
                                   const std::string& thread_root,
                                   const std::string& body,
                                   const std::string& formatted_body)
{
    return from_ffi(impl_->ffi->send_thread_message(room_id, thread_root, body,
                                                    formatted_body));
}

Result Client::send_thread_reply(const std::string& room_id,
                                 const std::string& thread_root,
                                 const std::string& in_reply_to_event_id,
                                 const std::string& body,
                                 const std::string& formatted_body)
{
    return from_ffi(impl_->ffi->send_thread_reply(
        room_id, thread_root, in_reply_to_event_id, body, formatted_body));
}
```

- [ ] **Step 3: Build.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | tail -20`
Expected: links cleanly (the cmake build recompiles the Rust bridge too).

- [ ] **Step 4: Commit.**

```bash
git add client/include/tesseract/client.h client/src/client.cpp
git commit -m "feat(client): Client wrappers for threaded text sends"
```

---

### Task 5: Add `thread_root` to the five media senders (Rust)

**Files:**
- Modify: `sdk/src/bridge.rs` (signatures of `send_image`/`send_file`/`send_audio`/`send_video`/`send_voice`)
- Modify: `sdk/src/client.rs` (impls + the `build_animated_image_content` raw path + existing tests)

> Pattern: each media sender currently builds `Reply { event_id, enforce_thread: EnforceThread::Unthreaded, add_mentions }` only when `reply_event_id` is non-empty (see `send_image` ~2841). Add a trailing `thread_root: &str` arg and compute the reply as follows:
> - `thread_root` empty → current behavior (unthreaded; reply only if `reply_event_id` set).
> - `thread_root` non-empty → always set a `Reply`: `event_id = if reply_event_id non-empty { reply_event_id } else { thread_root }`, `enforce_thread = EnforceThread::Threaded(if reply_event_id non-empty { ReplyWithinThread::Yes } else { ReplyWithinThread::No })`.

- [ ] **Step 1: Update the bridge signatures.** In `sdk/src/bridge.rs`, append `thread_root: &str,` as the final parameter of `send_image`, `send_file`, `send_audio`, `send_video`, and `send_voice` (after `reply_event_id`). Add a doc line: `/// When non-empty, sends the media into this thread root (MSC3440).`

- [ ] **Step 2: Add a shared reply-builder helper.** In `sdk/src/client.rs`:

```rust
#[cfg(not(test))]
fn build_media_reply(
    reply_event_id: &str,
    thread_root: &str,
) -> Result<Option<matrix_sdk::room::reply::Reply>, String> {
    use matrix_sdk::room::reply::{EnforceThread, Reply};
    use matrix_sdk::ruma::events::room::message::{AddMentions, ReplyWithinThread};
    if thread_root.is_empty() {
        if reply_event_id.is_empty() {
            return Ok(None);
        }
        let id = reply_event_id
            .parse()
            .map_err(|e| format!("invalid reply event id: {e}"))?;
        return Ok(Some(Reply {
            event_id: id,
            enforce_thread: EnforceThread::Unthreaded,
            add_mentions: AddMentions::No,
        }));
    }
    let (target, within) = if reply_event_id.is_empty() {
        (thread_root, ReplyWithinThread::No)
    } else {
        (reply_event_id, ReplyWithinThread::Yes)
    };
    let id = target
        .parse()
        .map_err(|e| format!("invalid reply event id: {e}"))?;
    Ok(Some(Reply {
        event_id: id,
        enforce_thread: EnforceThread::Threaded(within),
        add_mentions: AddMentions::No,
    }))
}
```

- [ ] **Step 3: Rewire each `send_attachment`-path sender to use the helper.** In `send_image` (non-animated path ~2841), `send_file`, `send_audio`, `send_video`, `send_voice`, replace the existing `if !reply_event_id.is_empty() { … config = config.reply(Some(Reply { … Unthreaded … })) }` block with:

```rust
        match build_media_reply(reply_event_id, thread_root) {
            Ok(Some(reply)) => config = config.reply(Some(reply)),
            Ok(None) => {}
            Err(e) => return err(e),
        }
```

(Add the new `thread_root: &str` param to each `#[cfg(not(test))]` fn signature and each `#[cfg(test)]` stub signature.)

- [ ] **Step 4: Thread the raw animated-image path.** `build_animated_image_content` (~line near the `send_image` animated branch) currently takes `reply_event_id` and emits `m.relates_to.m.in_reply_to`. Add a `thread_root: &str` param: when non-empty, emit `m.relates_to` as `{ "rel_type": "m.thread", "event_id": thread_root, "m.in_reply_to": { "event_id": <reply_event_id-or-root> }, "is_falling_back": <reply_event_id is empty> }` instead of a bare `m.in_reply_to`. Update the call site in `send_image` to pass `thread_root`.

- [ ] **Step 5: Update existing media tests for the new arg.** In `mod tests`:
  - `send_audio_not_logged_in`: change call to `c.send_audio("!room:server", &[], "audio/ogg", "voice.ogg", "", 0, "", "")` (trailing `""` thread_root).
  - `send_video_not_logged_in`: append a trailing `""` argument.
  - `animated_image_content_sets_flags` / `animated_image_content_with_caption_and_reply`: append a trailing `""` thread_root argument to both `build_animated_image_content(...)` calls.

- [ ] **Step 6: Add a threaded-media unit test.** In `mod tests`:

```rust
    #[test]
    fn animated_image_content_threaded() {
        let val = build_animated_image_content(
            "mxc://server/abc", "a.gif", "", "image/gif", 1, 1, 10, "", "$root:server",
        );
        assert_eq!(val["m.relates_to"]["rel_type"], "m.thread");
        assert_eq!(val["m.relates_to"]["event_id"], "$root:server");
    }
```

- [ ] **Step 7: Run the Rust tests.**

Run: `cargo test -p tesseract-sdk-ffi`
Expected: PASS (all media tests, including updated ones).

- [ ] **Step 8: Commit.**

```bash
git add sdk/src/bridge.rs sdk/src/client.rs
git commit -m "feat(sdk): thread_root param on media senders (MSC3440)"
```

---

### Task 6: Add `thread_root` to the C++ media `Client` wrappers (default arg)

**Files:**
- Modify: `client/include/tesseract/client.h`
- Modify: `client/src/client.cpp`

> Use a C++ default argument (`= std::string{}`) so existing `ui/` callers compile unchanged.

- [ ] **Step 1: Add the parameter to each declaration.** In `client/include/tesseract/client.h`, append to `send_image`, `send_file`, `send_audio`, `send_video`, `send_voice`:

```cpp
                       const std::string& thread_root = std::string{}
```

as the final parameter (after `reply_event_id`). Add a doc line: `/// When non-empty, sends into this thread root (MSC3440).`

- [ ] **Step 2: Forward it in each impl.** In `client/src/client.cpp`, add `, thread_root` as the final argument of each `impl_->ffi->send_image(...)` / `send_file` / `send_audio` / `send_video` / `send_voice` call, and add the matching `const std::string& thread_root` parameter to each method definition's signature (definitions don't repeat the default).

- [ ] **Step 3: Build + ctest.**

Run: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure -R "media|compose|types"`
Expected: PASS; existing `ui/` callers still compile via the default arg.

- [ ] **Step 4: Commit.**

```bash
git add client/include/tesseract/client.h client/src/client.cpp
git commit -m "feat(client): thread_root default arg on media senders"
```

---

## Phase 3 — Thread-focused timeline subscription (capability B)

### Task 7: `TimelineChannel` + parameterized diff engine + `on_thread_*` callbacks

**Files:**
- Modify: `sdk/src/bridge.rs` (4 new callbacks in the `extern "C++"` block)
- Modify: `sdk/src/lib.rs` (4 new methods on the stub `EventHandlerBridge`)
- Modify: `sdk/src/client.rs` (`TimelineChannel`, emit helpers, `spawn_timeline_tasks`, `handle_timeline_diff`, the two `subscribe_room*` call sites)

- [ ] **Step 1: Declare the four thread callbacks in the bridge.** In `sdk/src/bridge.rs`, in the `unsafe extern "C++"` block after `on_message_removed`, add:

```rust
        /// Thread-timeline twins of the four room-timeline callbacks. `room_id`
        /// is the host room; `thread_root` is the thread root event id. Indices
        /// follow the same visible-index VectorDiff semantics.
        fn on_thread_reset(
            self: &EventHandlerBridge,
            room_id: &str,
            thread_root: &str,
            snapshot: &Vec<TimelineEvent>,
        );
        fn on_thread_inserted(
            self: &EventHandlerBridge,
            room_id: &str,
            thread_root: &str,
            index: u64,
            event: &TimelineEvent,
        );
        fn on_thread_updated(
            self: &EventHandlerBridge,
            room_id: &str,
            thread_root: &str,
            index: u64,
            event: &TimelineEvent,
        );
        fn on_thread_removed(
            self: &EventHandlerBridge,
            room_id: &str,
            thread_root: &str,
            index: u64,
        );
```

- [ ] **Step 2: Mirror them on the test stub.** In `sdk/src/lib.rs`, in `impl EventHandlerBridge`, add four no-op methods matching the signatures (e.g. `pub fn on_thread_reset(&self, _room_id: &str, _thread_root: &str, _snapshot: &Vec<TimelineEvent>) {}`, etc.).

- [ ] **Step 3: Add the `TimelineChannel` enum + emit helpers.** In `sdk/src/client.rs` (file scope, near `spawn_timeline_tasks`):

```rust
#[cfg(not(test))]
#[derive(Clone)]
enum TimelineChannel {
    Room,
    Thread(String), // thread root event id
}

#[cfg(not(test))]
fn emit_reset(g: &SendHandler, ch: &TimelineChannel, id: &str, snap: &Vec<TimelineEvent>) {
    match ch {
        TimelineChannel::Room => g.on_timeline_reset(id, snap),
        TimelineChannel::Thread(root) => g.on_thread_reset(id, root, snap),
    }
}
#[cfg(not(test))]
fn emit_inserted(g: &SendHandler, ch: &TimelineChannel, id: &str, idx: u64, ev: &TimelineEvent) {
    match ch {
        TimelineChannel::Room => g.on_message_inserted(id, idx, ev),
        TimelineChannel::Thread(root) => g.on_thread_inserted(id, root, idx, ev),
    }
}
#[cfg(not(test))]
fn emit_updated(g: &SendHandler, ch: &TimelineChannel, id: &str, idx: u64, ev: &TimelineEvent) {
    match ch {
        TimelineChannel::Room => g.on_message_updated(id, idx, ev),
        TimelineChannel::Thread(root) => g.on_thread_updated(id, root, idx, ev),
    }
}
#[cfg(not(test))]
fn emit_removed(g: &SendHandler, ch: &TimelineChannel, id: &str, idx: u64) {
    match ch {
        TimelineChannel::Room => g.on_message_removed(id, idx),
        TimelineChannel::Thread(root) => g.on_thread_removed(id, root, idx),
    }
}
```

- [ ] **Step 4: Parameterize `spawn_timeline_tasks`.** Add `channel: TimelineChannel` as the **final** parameter (after `rt`). Move it into the spawned task (`let ch = channel;`). Replace the reset emit at ~1991:

```rust
                if let Ok(guard) = h.lock() {
                    emit_reset(&guard, &ch, &rid, &snapshot);
                }
```

and pass `&ch` into `handle_timeline_diff(diff, &mut visible, &h, &rid, &room_clone, me.as_deref(), &client_ref, &ch).await;`.

- [ ] **Step 5: Parameterize `handle_timeline_diff`.** Add `channel: &TimelineChannel` as the final parameter. Replace **every** emit call in its body:
  - `g.on_message_inserted(room_id, IDX, &ev)` → `emit_inserted(&g, channel, room_id, IDX, &ev)`
  - `g.on_message_updated(room_id, IDX, &ev)` → `emit_updated(&g, channel, room_id, IDX, &ev)`
  - `g.on_message_removed(room_id, IDX)` → `emit_removed(&g, channel, room_id, IDX)`
  - the two `g.on_timeline_reset(room_id, &snapshot|&empty)` (~7735, ~7753) → `emit_reset(&g, channel, room_id, &snapshot|&empty)`

  (Preserve each existing index expression exactly — `idx`, `v_idx`, `0`.)

- [ ] **Step 6: Update the two existing call sites.** In `subscribe_room` (~2086) and `subscribe_room_at` (~2353), change the `Self::spawn_timeline_tasks(&timeline, &room, room_id_str, &handler, &client, &self.rt)` call to append `, TimelineChannel::Room` as the final argument. The two synchronous `guard.on_timeline_reset(&room_id_str, &empty)` calls (~2082, ~2349) stay as-is (they are the room path).

- [ ] **Step 7: Run the Rust tests (compile check of the engine refactor).**

Run: `cargo test -p tesseract-sdk-ffi`
Expected: PASS (no behavior change to the room path; the stub `EventHandlerBridge` now has the thread methods).

- [ ] **Step 8: Commit.**

```bash
git add sdk/src/bridge.rs sdk/src/lib.rs sdk/src/client.rs
git commit -m "refactor(sdk): parameterize timeline engine with TimelineChannel; add on_thread_* callbacks"
```

---

### Task 8: `subscribe_thread` / `unsubscribe_thread` / `paginate_thread_back` (Rust)

**Files:**
- Modify: `sdk/src/bridge.rs` (3 methods)
- Modify: `sdk/src/client.rs` (state field, impls, `sync_room_subscriptions`, `Drop`, tests)

- [ ] **Step 1: Add the `thread_timelines` map.** In `sdk/src/client.rs`, in the `ClientFfi` struct (near `timelines:` at ~field list), add:

```rust
    #[cfg(not(test))]
    thread_timelines: std::collections::HashMap<(OwnedRoomId, matrix_sdk::ruma::OwnedEventId), TimelineHandle>,
```

Initialize it to `HashMap::new()` wherever `timelines` is initialized in `ClientFfi::new()`.

- [ ] **Step 2: Declare the three FFI methods.** In `sdk/src/bridge.rs`, in `extern "Rust"` near the timeline methods:

```rust
        /// Subscribe to the thread rooted at `root_event_id` in `room_id`.
        /// Fires on_thread_reset then live on_thread_* callbacks. Call
        /// paginate_thread_back for older replies.
        fn subscribe_thread(
            self: &mut ClientFfi,
            room_id: &str,
            root_event_id: &str,
        ) -> OpResult;

        /// Unsubscribe from a thread timeline and cancel its tasks.
        fn unsubscribe_thread(self: &mut ClientFfi, room_id: &str, root_event_id: &str);

        /// Paginate backwards in a subscribed thread timeline.
        fn paginate_thread_back(
            self: &mut ClientFfi,
            room_id: &str,
            root_event_id: &str,
            count: u16,
        ) -> PaginateResult;
```

- [ ] **Step 3: Implement `subscribe_thread`.** Model on `subscribe_room_at` (~2292), but build a thread focus and key by `(room_id, root)`:

```rust
    #[cfg(not(test))]
    pub fn subscribe_thread(&mut self, room_id: &str, root_event_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(handler) = self.handler.clone() else {
            return err("sync not started");
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let root: matrix_sdk::ruma::OwnedEventId = match root_event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid thread root id: {e}")),
        };
        let key = (room_id.clone(), root.clone());
        if let Some(prev) = self.thread_timelines.remove(&key) {
            for h in prev.abort_tasks {
                h.abort();
            }
        }
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        let focus = TimelineFocus::Thread {
            root_event_id: root.clone(),
        };
        let room_for_build = room.clone();
        let timeline = match self.rt.block_on(self.rt.spawn(async move {
            room_for_build
                .timeline_builder()
                .with_focus(focus)
                .build()
                .await
        })) {
            Ok(Ok(t)) => Arc::new(t),
            Ok(Err(e)) => return err(format!("build thread timeline: {e}")),
            Err(e) => return err(format!("build thread timeline task: {e}")),
        };
        let room_id_str = room_id.to_string();
        let root_str = root.to_string();
        if let Ok(guard) = handler.lock() {
            let empty: Vec<TimelineEvent> = Vec::new();
            guard.on_thread_reset(&room_id_str, &root_str, &empty);
        }
        let (abort, fetch_abort) = Self::spawn_timeline_tasks(
            &timeline,
            &room,
            room_id_str,
            &handler,
            &client,
            &self.rt,
            TimelineChannel::Thread(root_str),
        );
        self.thread_timelines.insert(
            key,
            TimelineHandle {
                timeline,
                abort_tasks: vec![abort, fetch_abort],
                is_focused: true,
            },
        );
        self.sync_room_subscriptions();
        ok("")
    }
```

- [ ] **Step 4: Implement `unsubscribe_thread` and `paginate_thread_back`.**

```rust
    #[cfg(not(test))]
    pub fn unsubscribe_thread(&mut self, room_id: &str, root_event_id: &str) {
        if let (Ok(rid), Ok(root)) = (
            room_id.parse::<OwnedRoomId>(),
            root_event_id.parse::<matrix_sdk::ruma::OwnedEventId>(),
        ) {
            if let Some(h) = self.thread_timelines.remove(&(rid, root)) {
                for abort in h.abort_tasks {
                    abort.abort();
                }
            }
        }
        self.sync_room_subscriptions();
    }

    #[cfg(not(test))]
    pub fn paginate_thread_back(
        &mut self,
        room_id: &str,
        root_event_id: &str,
        count: u16,
    ) -> PaginateResult {
        let rid: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => {
                return PaginateResult {
                    ok: false,
                    message: format!("invalid room id: {e}"),
                    reached_start: false,
                    reached_end: false,
                }
            }
        };
        let root: matrix_sdk::ruma::OwnedEventId = match root_event_id.parse() {
            Ok(id) => id,
            Err(e) => {
                return PaginateResult {
                    ok: false,
                    message: format!("invalid thread root id: {e}"),
                    reached_start: false,
                    reached_end: false,
                }
            }
        };
        let Some(handle) = self.thread_timelines.get(&(rid, root)) else {
            return PaginateResult {
                ok: false,
                message: "thread not subscribed".to_owned(),
                reached_start: false,
                reached_end: false,
            };
        };
        let tl = Arc::clone(&handle.timeline);
        match self
            .rt
            .block_on(async move { tl.paginate_backwards(count).await })
        {
            Ok(reached_start) => PaginateResult {
                ok: true,
                message: String::new(),
                reached_start,
                reached_end: false,
            },
            Err(e) => PaginateResult {
                ok: false,
                message: e.to_string(),
                reached_start: false,
                reached_end: false,
            },
        }
    }
```

> Confirm the exact back-pagination call used by the room path (`paginate_back_with_status` ~2167) and reuse the identical method name (`paginate_backwards(count)` returns `bool` reached-start in 0.17). Match its error handling.

- [ ] **Step 5: Add `#[cfg(test)]` stubs** for all three methods returning `err("not logged in")` / a not-ok `PaginateResult` (mirror the `paginate_back_with_status` test stub if one exists; otherwise return the not-ok struct).

- [ ] **Step 6: Union thread rooms into `sync_room_subscriptions`.** In `sync_room_subscriptions` (~2119), build `ids` from both maps:

```rust
        let mut ids: Vec<OwnedRoomId> = self.timelines.keys().cloned().collect();
        for (rid, _root) in self.thread_timelines.keys() {
            if !ids.contains(rid) {
                ids.push(rid.clone());
            }
        }
```

- [ ] **Step 7: Abort thread tasks in `Drop`/`stop_sync`.** In the `Drop for ClientFfi` impl (~383, where it iterates `timelines`), add an equivalent loop aborting `self.thread_timelines` handles' `abort_tasks`. Do the same anywhere `stop_sync` tears down `timelines`.

- [ ] **Step 8: Add tests.** In `mod tests`:

```rust
    #[test]
    fn subscribe_thread_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.subscribe_thread("!room:server", "$root:server");
        assert!(!r.ok);
    }

    #[test]
    fn paginate_thread_back_not_subscribed() {
        let mut c = ClientFfi::new();
        let r = c.paginate_thread_back("!room:server", "$root:server", 20);
        assert!(!r.ok);
    }
```

- [ ] **Step 9: Run the Rust tests.**

Run: `cargo test -p tesseract-sdk-ffi`
Expected: PASS.

- [ ] **Step 10: Commit.**

```bash
git add sdk/src/bridge.rs sdk/src/client.rs
git commit -m "feat(sdk): subscribe_thread / paginate_thread_back / unsubscribe_thread"
```

---

### Task 9: `on_thread_*` `IEventHandler` virtuals + `EventHandlerBridge` forwarders (C++)

**Files:**
- Modify: `client/include/tesseract/event_handler.h` (4 new virtuals)
- Modify: `client/include/tesseract/event_handler_bridge.h` (4 declarations)
- Modify: `client/src/event_handler_bridge.cpp` (4 impls)

- [ ] **Step 1: Add default-no-op virtuals to `IEventHandler`.** In `client/include/tesseract/event_handler.h`, after `on_message_removed` (line ~51) add:

```cpp
    /// Thread-timeline twins of the four room-timeline callbacks. `thread_root`
    /// is the thread root event id. Default no-ops so shells opt in later.
    virtual void on_thread_reset(const std::string& /*room_id*/,
                                 const std::string& /*thread_root*/,
                                 EventList /*snapshot*/)
    {
    }
    virtual void on_thread_inserted(const std::string& /*room_id*/,
                                    const std::string& /*thread_root*/,
                                    std::size_t /*index*/,
                                    std::unique_ptr<Event> /*event*/)
    {
    }
    virtual void on_thread_updated(const std::string& /*room_id*/,
                                   const std::string& /*thread_root*/,
                                   std::size_t /*index*/,
                                   std::unique_ptr<Event> /*event*/)
    {
    }
    virtual void on_thread_removed(const std::string& /*room_id*/,
                                   const std::string& /*thread_root*/,
                                   std::size_t /*index*/)
    {
    }
```

- [ ] **Step 2: Declare the bridge forwarders.** In `client/include/tesseract/event_handler_bridge.h`, mirror the existing `on_message_*` declarations adding a `rust::Str thread_root` second parameter for each of the four. (Match the `const` + `rust::Str` / `std::uint64_t` style used by the room callbacks.)

- [ ] **Step 3: Implement the forwarders.** In `client/src/event_handler_bridge.cpp`, after `on_message_removed` (line ~125), add (mirroring the existing four, with `guard(...)`, `index_fits`, and `make_event`):

```cpp
void EventHandlerBridge::on_thread_reset(
    rust::Str room_id, rust::Str thread_root,
    const rust::Vec<TimelineEvent>& snapshot) const
{
    guard("on_thread_reset",
          [&]
          {
              if (!handler_)
                  return;
              std::vector<std::unique_ptr<tesseract::Event>> events;
              events.reserve(snapshot.size());
              for (const auto& ev : snapshot)
                  events.push_back(tesseract::make_event(ev));
              handler_->on_thread_reset(std::string(room_id),
                                        std::string(thread_root),
                                        std::move(events));
          });
}

void EventHandlerBridge::on_thread_inserted(rust::Str room_id,
                                            rust::Str thread_root,
                                            std::uint64_t index,
                                            const TimelineEvent& ev) const
{
    guard("on_thread_inserted",
          [&]
          {
              if (!handler_ || !index_fits(index))
                  return;
              handler_->on_thread_inserted(
                  std::string(room_id), std::string(thread_root),
                  static_cast<std::size_t>(index), tesseract::make_event(ev));
          });
}

void EventHandlerBridge::on_thread_updated(rust::Str room_id,
                                           rust::Str thread_root,
                                           std::uint64_t index,
                                           const TimelineEvent& ev) const
{
    guard("on_thread_updated",
          [&]
          {
              if (!handler_ || !index_fits(index))
                  return;
              handler_->on_thread_updated(
                  std::string(room_id), std::string(thread_root),
                  static_cast<std::size_t>(index), tesseract::make_event(ev));
          });
}

void EventHandlerBridge::on_thread_removed(rust::Str room_id,
                                           rust::Str thread_root,
                                           std::uint64_t index) const
{
    guard("on_thread_removed",
          [&]
          {
              if (!handler_ || !index_fits(index))
                  return;
              handler_->on_thread_removed(std::string(room_id),
                                          std::string(thread_root),
                                          static_cast<std::size_t>(index));
          });
}
```

- [ ] **Step 4: Build.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | tail -20`
Expected: clean build (the cxx codegen now matches the bridge declarations from Task 7).

- [ ] **Step 5: Commit.**

```bash
git add client/include/tesseract/event_handler.h client/include/tesseract/event_handler_bridge.h client/src/event_handler_bridge.cpp
git commit -m "feat(client): on_thread_* IEventHandler virtuals + bridge forwarders"
```

---

### Task 10: C++ `Client` wrappers for thread subscription

**Files:**
- Modify: `client/include/tesseract/client.h`
- Modify: `client/src/client.cpp`

- [ ] **Step 1: Declare the methods.** In `client/include/tesseract/client.h`, near `subscribe_room_at`:

```cpp
    /// Subscribe to the thread rooted at `root_event_id` in `room_id`. Fires
    /// IEventHandler::on_thread_* callbacks.
    Result subscribe_thread(const std::string& room_id,
                            const std::string& root_event_id);

    /// Unsubscribe from a thread timeline.
    void unsubscribe_thread(const std::string& room_id,
                            const std::string& root_event_id);

    /// Paginate backwards in a subscribed thread timeline.
    PaginateResult paginate_thread_back(const std::string& room_id,
                                        const std::string& root_event_id,
                                        std::uint16_t count);
```

- [ ] **Step 2: Implement them.** In `client/src/client.cpp`:

```cpp
Result Client::subscribe_thread(const std::string& room_id,
                                const std::string& root_event_id)
{
    return from_ffi(impl_->ffi->subscribe_thread(room_id, root_event_id));
}

void Client::unsubscribe_thread(const std::string& room_id,
                                const std::string& root_event_id)
{
    impl_->ffi->unsubscribe_thread(room_id, root_event_id);
}

PaginateResult Client::paginate_thread_back(const std::string& room_id,
                                            const std::string& root_event_id,
                                            std::uint16_t count)
{
    return from_ffi(
        impl_->ffi->paginate_thread_back(room_id, root_event_id, count));
}
```

- [ ] **Step 3: Build + ctest.**

Run: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure`
Expected: PASS (full suite still green).

- [ ] **Step 4: Commit.**

```bash
git add client/include/tesseract/client.h client/src/client.cpp
git commit -m "feat(client): Client wrappers for thread subscription"
```

---

## Phase 4 — Per-room thread list (capability C)

### Task 11: `ThreadInfo` FFI struct + `ThreadListService` wrapper (Rust)

**Files:**
- Modify: `sdk/src/bridge.rs` (`ThreadInfo` struct, `on_threads_updated` callback, 4 methods)
- Modify: `sdk/src/lib.rs` (`ThreadInfo` stub struct, `on_threads_updated` stub method)
- Modify: `sdk/src/client.rs` (state field, impls, tests)

- [ ] **Step 1: Add `ThreadInfo` to the bridge.** In `sdk/src/bridge.rs`, in the shared-types section:

```rust
    /// One thread in a room, flattened from matrix-sdk-ui's ThreadListItem.
    /// `latest_*` fields are empty/0 when no reply summary is available.
    struct ThreadInfo {
        root_event_id: String,
        root_sender_name: String,
        root_body: String,
        root_timestamp: u64,
        latest_event_id: String,
        latest_sender_name: String,
        latest_body: String,
        latest_timestamp: u64,
        num_replies: u64,
    }
```

- [ ] **Step 2: Add the `on_threads_updated` callback** to the `extern "C++"` block:

```rust
        /// Fired when the cached thread list for `room_id` changes. The UI
        /// re-queries via list_room_threads (re-query ping, like
        /// on_image_packs_updated).
        fn on_threads_updated(self: &EventHandlerBridge, room_id: &str);
```

- [ ] **Step 3: Declare the four FFI methods** in `extern "Rust"`:

```rust
        /// Start watching the thread list for `room_id`: builds a
        /// ThreadListService, kicks an initial pagination, and fires
        /// on_threads_updated when the list changes.
        fn subscribe_room_threads(self: &mut ClientFfi, room_id: &str) -> OpResult;

        /// Stop watching the thread list for `room_id`.
        fn unsubscribe_room_threads(self: &mut ClientFfi, room_id: &str);

        /// Snapshot of the current thread list for `room_id` (most-recent
        /// first). Empty when not subscribed or none known yet.
        fn list_room_threads(self: &ClientFfi, room_id: &str) -> Vec<ThreadInfo>;

        /// Paginate older threads. reached_start == true ⇒ list exhausted.
        fn paginate_room_threads(self: &mut ClientFfi, room_id: &str) -> PaginateResult;
```

- [ ] **Step 4: Mirror in the test stub.** In `sdk/src/lib.rs`: add the `ThreadInfo` struct (with `#[derive(Debug, PartialEq, Default)]`, same fields) to the `#[cfg(test)] pub mod ffi`, and add `pub fn on_threads_updated(&self, _room_id: &str) {}` to the stub `impl EventHandlerBridge`.

- [ ] **Step 5: Add the `thread_lists` state + handle type.** In `sdk/src/client.rs`:

```rust
#[cfg(not(test))]
struct ThreadListHandle {
    service: std::sync::Arc<matrix_sdk_ui::timeline::ThreadListService>,
    abort: tokio::task::AbortHandle,
}
```

and a field on `ClientFfi`:

```rust
    #[cfg(not(test))]
    thread_lists: std::collections::HashMap<OwnedRoomId, ThreadListHandle>,
```

initialized to `HashMap::new()` in `ClientFfi::new()`.

> Confirm `ThreadListService` is constructible as `ThreadListService::new(room)` and whether it needs `Arc`; adjust the handle type to what the 0.17 API returns. If `subscribe_to_items_updates()` yields an `eyeball_im` stream of `VectorDiff`, the watch task just needs to observe *any* change and ping — it does not need to mirror the vector.

- [ ] **Step 6: Implement `subscribe_room_threads`.**

```rust
    #[cfg(not(test))]
    pub fn subscribe_room_threads(&mut self, room_id: &str) -> OpResult {
        use matrix_sdk_ui::timeline::ThreadListService;
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(handler) = self.handler.clone() else {
            return err("sync not started");
        };
        let rid: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&rid) else {
            return err("room not found");
        };
        if let Some(prev) = self.thread_lists.remove(&rid) {
            prev.abort.abort();
        }
        let service = std::sync::Arc::new(ThreadListService::new(room));
        // Initial pagination so the first list is populated.
        {
            let svc = std::sync::Arc::clone(&service);
            self.rt.spawn(async move {
                let _ = svc.paginate().await;
            });
        }
        // Watch task: ping on any items change.
        let rid_str = rid.to_string();
        let svc_for_watch = std::sync::Arc::clone(&service);
        let h = handler.clone();
        let abort = self
            .rt
            .spawn(async move {
                let (_initial, mut stream) = svc_for_watch.subscribe_to_items_updates();
                if let Ok(g) = h.lock() {
                    g.on_threads_updated(&rid_str);
                }
                while stream.next().await.is_some() {
                    if let Ok(g) = h.lock() {
                        g.on_threads_updated(&rid_str);
                    }
                }
            })
            .abort_handle();
        self.thread_lists
            .insert(rid, ThreadListHandle { service, abort });
        ok("")
    }
```

> The exact return shape of `subscribe_to_items_updates()` (tuple vs stream-only) must be confirmed against 0.17 and the destructuring adjusted; the contract is "fire `on_threads_updated` once initially and on every change."

- [ ] **Step 7: Implement `list_room_threads`, `paginate_room_threads`, `unsubscribe_room_threads`, and a `thread_info_from_item` converter.**

```rust
    #[cfg(not(test))]
    pub fn list_room_threads(&self, room_id: &str) -> Vec<ThreadInfo> {
        let Ok(rid) = room_id.parse::<OwnedRoomId>() else {
            return Vec::new();
        };
        let Some(handle) = self.thread_lists.get(&rid) else {
            return Vec::new();
        };
        handle
            .service
            .items()
            .iter()
            .map(thread_info_from_item)
            .collect()
    }

    #[cfg(not(test))]
    pub fn paginate_room_threads(&mut self, room_id: &str) -> PaginateResult {
        let rid: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => {
                return PaginateResult {
                    ok: false,
                    message: format!("invalid room id: {e}"),
                    reached_start: false,
                    reached_end: false,
                }
            }
        };
        let Some(handle) = self.thread_lists.get(&rid) else {
            return PaginateResult {
                ok: false,
                message: "room threads not subscribed".to_owned(),
                reached_start: false,
                reached_end: false,
            };
        };
        let svc = std::sync::Arc::clone(&handle.service);
        match self.rt.block_on(async move { svc.paginate().await }) {
            Ok(()) => {
                use matrix_sdk_ui::timeline::ThreadListPaginationState;
                // `reached_start` here means "no more older threads to load".
                let reached_start = matches!(
                    handle.service.pagination_state(),
                    ThreadListPaginationState::Idle { end_reached: true }
                );
                PaginateResult {
                    ok: true,
                    message: String::new(),
                    reached_start,
                    reached_end: false,
                }
            }
            Err(e) => PaginateResult {
                ok: false,
                message: e.to_string(),
                reached_start: false,
                reached_end: false,
            },
        }
    }

    #[cfg(not(test))]
    pub fn unsubscribe_room_threads(&mut self, room_id: &str) {
        if let Ok(rid) = room_id.parse::<OwnedRoomId>() {
            if let Some(h) = self.thread_lists.remove(&rid) {
                h.abort.abort();
            }
        }
    }
```

Converter (file scope):

```rust
#[cfg(not(test))]
fn thread_list_event_preview(
    ev: &matrix_sdk_ui::timeline::ThreadListItemEvent,
) -> (String, String, String, u64) {
    let name = match &ev.sender_profile {
        TimelineDetails::Ready(p) => p
            .display_name
            .clone()
            .unwrap_or_else(|| ev.sender.to_string()),
        _ => ev.sender.to_string(),
    };
    let body = match &ev.content {
        Some(content) => msglike_snippet(content),
        None => String::new(),
    };
    let ts: u64 = ev.timestamp.get().into();
    (ev.event_id.to_string(), name, body, ts)
}

#[cfg(not(test))]
fn thread_info_from_item(item: &matrix_sdk_ui::timeline::ThreadListItem) -> ThreadInfo {
    let (root_event_id, root_sender_name, root_body, root_timestamp) =
        thread_list_event_preview(&item.root_event);
    let (latest_event_id, latest_sender_name, latest_body, latest_timestamp) =
        match &item.latest_event {
            Some(ev) => thread_list_event_preview(ev),
            None => (String::new(), String::new(), String::new(), 0),
        };
    ThreadInfo {
        root_event_id,
        root_sender_name,
        root_body,
        root_timestamp,
        latest_event_id,
        latest_sender_name,
        latest_body,
        latest_timestamp,
        num_replies: item.num_replies as u64,
    }
}
```

> `msglike_snippet` is the shared helper extracted in Task 1 Step 6. If `ThreadListItemEvent.content` is `Option<TimelineItemContent>` (confirmed in the API notes), this compiles directly.

- [ ] **Step 8: Add `#[cfg(test)]` stubs** for all four methods: `subscribe_room_threads`/`paginate_room_threads` return the not-ok forms, `unsubscribe_room_threads` is a no-op, `list_room_threads` returns `Vec::new()`.

- [ ] **Step 9: Abort watch tasks in `Drop`.** In `Drop for ClientFfi`, after aborting timelines, add `for (_rid, h) in self.thread_lists.drain() { h.abort.abort(); }`.

- [ ] **Step 10: Add tests.** In `mod tests`:

```rust
    #[test]
    fn subscribe_room_threads_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.subscribe_room_threads("!room:server");
        assert!(!r.ok);
    }

    #[test]
    fn list_room_threads_empty_when_not_subscribed() {
        let c = ClientFfi::new();
        assert!(c.list_room_threads("!room:server").is_empty());
    }

    #[test]
    fn thread_info_default_shape() {
        let ti = crate::ffi::ThreadInfo {
            root_event_id: "$root:server".to_owned(),
            num_replies: 4,
            ..Default::default()
        };
        assert_eq!(ti.root_event_id, "$root:server");
        assert_eq!(ti.num_replies, 4);
        assert!(ti.latest_event_id.is_empty());
    }
```

- [ ] **Step 11: Run the Rust tests.**

Run: `cargo test -p tesseract-sdk-ffi`
Expected: PASS.

- [ ] **Step 12: Commit.**

```bash
git add sdk/src/bridge.rs sdk/src/lib.rs sdk/src/client.rs
git commit -m "feat(sdk): per-room thread list via ThreadListService"
```

---

### Task 12: C++ `ThreadInfo`, `on_threads_updated`, and `Client` wrappers

**Files:**
- Modify: `client/include/tesseract/types.h` (`ThreadInfo` C++ struct)
- Modify: `client/src/ffi_convert.h` (`from_ffi(ThreadInfo)`)
- Modify: `client/include/tesseract/event_handler.h` (`on_threads_updated` virtual)
- Modify: `client/include/tesseract/event_handler_bridge.h` + `client/src/event_handler_bridge.cpp` (forwarder)
- Modify: `client/include/tesseract/client.h` + `client/src/client.cpp` (4 wrappers)
- Test: `tests/cpp/test_types.cpp`

- [ ] **Step 1: Add a failing C++ test.** In `tests/cpp/test_types.cpp`:

```cpp
TEST_CASE("ThreadInfo default-initialises", "[types][thread]")
{
    tesseract::ThreadInfo ti;
    CHECK(ti.root_event_id.empty());
    CHECK(ti.num_replies == 0);
    CHECK(ti.latest_event_id.empty());
}

TEST_CASE("ThreadInfo fields are settable", "[types][thread]")
{
    tesseract::ThreadInfo ti;
    ti.root_event_id = "$root:server";
    ti.num_replies = 7;
    ti.latest_sender_name = "Carol";
    CHECK(ti.root_event_id == "$root:server");
    CHECK(ti.num_replies == 7);
    CHECK(ti.latest_sender_name == "Carol");
}
```

- [ ] **Step 2: Build to verify it fails.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | head -20`
Expected: `tesseract::ThreadInfo` not declared.

- [ ] **Step 3: Add the C++ `ThreadInfo` struct.** In `client/include/tesseract/types.h` (near `RoomInfo`):

```cpp
/// One thread in a room. `latest_*` fields are empty/0 when no reply summary
/// is available. Mirror of the FFI ThreadInfo.
struct ThreadInfo
{
    std::string root_event_id;
    std::string root_sender_name;
    std::string root_body;
    uint64_t root_timestamp = 0;
    std::string latest_event_id;
    std::string latest_sender_name;
    std::string latest_body;
    uint64_t latest_timestamp = 0;
    uint64_t num_replies = 0;
};
```

- [ ] **Step 4: Add the `from_ffi` converter.** In `client/src/ffi_convert.h` (near the other `from_ffi` overloads):

```cpp
inline tesseract::ThreadInfo from_ffi(const tesseract_ffi::ThreadInfo& t)
{
    tesseract::ThreadInfo out;
    out.root_event_id = std::string(t.root_event_id);
    out.root_sender_name = std::string(t.root_sender_name);
    out.root_body = std::string(t.root_body);
    out.root_timestamp = t.root_timestamp;
    out.latest_event_id = std::string(t.latest_event_id);
    out.latest_sender_name = std::string(t.latest_sender_name);
    out.latest_body = std::string(t.latest_body);
    out.latest_timestamp = t.latest_timestamp;
    out.num_replies = t.num_replies;
    return out;
}
```

- [ ] **Step 5: Add the `on_threads_updated` virtual.** In `client/include/tesseract/event_handler.h`, near `on_image_packs_updated`:

```cpp
    /// Fired when the cached thread list for `room_id` changes. Re-query via
    /// Client::list_room_threads. Default no-op.
    virtual void on_threads_updated(const std::string& /*room_id*/)
    {
    }
```

- [ ] **Step 6: Add the bridge forwarder.** Declare in `client/include/tesseract/event_handler_bridge.h` and implement in `client/src/event_handler_bridge.cpp`:

```cpp
void EventHandlerBridge::on_threads_updated(rust::Str room_id) const
{
    guard("on_threads_updated",
          [&]
          {
              if (!handler_)
                  return;
              handler_->on_threads_updated(std::string(room_id));
          });
}
```

- [ ] **Step 7: Add the four `Client` wrappers.** In `client/include/tesseract/client.h`:

```cpp
    /// Start watching the thread list for `room_id` (fires
    /// IEventHandler::on_threads_updated on changes).
    Result subscribe_room_threads(const std::string& room_id);
    void unsubscribe_room_threads(const std::string& room_id);
    /// Snapshot of the current thread list for `room_id`.
    std::vector<ThreadInfo> list_room_threads(const std::string& room_id);
    PaginateResult paginate_room_threads(const std::string& room_id);
```

In `client/src/client.cpp`:

```cpp
Result Client::subscribe_room_threads(const std::string& room_id)
{
    return from_ffi(impl_->ffi->subscribe_room_threads(room_id));
}

void Client::unsubscribe_room_threads(const std::string& room_id)
{
    impl_->ffi->unsubscribe_room_threads(room_id);
}

std::vector<ThreadInfo> Client::list_room_threads(const std::string& room_id)
{
    std::vector<ThreadInfo> out;
    auto ffi_threads = impl_->ffi->list_room_threads(room_id);
    out.reserve(ffi_threads.size());
    for (const auto& t : ffi_threads)
        out.push_back(from_ffi(t));
    return out;
}

PaginateResult Client::paginate_room_threads(const std::string& room_id)
{
    return from_ffi(impl_->ffi->paginate_room_threads(room_id));
}
```

- [ ] **Step 8: Build + ctest.**

Run: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure`
Expected: PASS (full suite green).

- [ ] **Step 9: Commit.**

```bash
git add client/include/tesseract/types.h client/src/ffi_convert.h client/include/tesseract/event_handler.h client/include/tesseract/event_handler_bridge.h client/src/event_handler_bridge.cpp client/include/tesseract/client.h client/src/client.cpp tests/cpp/test_types.cpp
git commit -m "feat(client): ThreadInfo, on_threads_updated, thread-list Client wrappers"
```

---

## Final verification

- [ ] **Step 1: Full Rust test run.** `cargo test -p tesseract-sdk-ffi` → all pass.
- [ ] **Step 2: Full C++ build + ctest.** `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure` → all pass.
- [ ] **Step 3: Confirm no `ui/` files changed.** `git diff --name-only origin/master... | grep '^ui/'` → empty.
- [ ] **Step 4: Update STATUS.md** (per repo convention) — note thread FFI/client exposure, refresh test counts + Last updated date. Commit.

> Per repo policy (CLAUDE.md): do **not** push or merge until the user confirms the change has been tested and works.

---

## Notes on API confirmation during implementation

Three accessor shapes must be checked against the on-disk crate
(`~/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/matrix-sdk-ui-0.17.0/`) when
implementing, and the code adapted if they differ — the wire contracts and tests stay the same:

1. `EmbeddedEvent` field/accessor names for sender/content/timestamp (Task 1 Step 6).
2. The exact thread back-pagination method name + return type (Task 8 Step 4) — mirror the room `paginate_back_with_status` path.
3. `ThreadListService::subscribe_to_items_updates()` return shape and `ThreadListService::new` constructor (Task 11 Steps 5–6).

These are read-only lookups; none change the public FFI/C++ surface defined above.
