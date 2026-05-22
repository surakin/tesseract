# Expose Matrix threads to the C++ level — design

**Date:** 2026-05-22
**Scope:** Wire matrix-sdk-ui 0.17 thread support through the `cxx` FFI bridge and
the C++ `client/` layer. **No UI work** — all new C++ `IEventHandler` callbacks are
default no-op virtuals so `ui/` compiles untouched.

## Goal

Give the C++ layer the full thread surface a future UI will need:

- **A.** Thread metadata on existing timeline events (root id on in-thread
  replies; reply count + latest-reply preview on roots).
- **B.** Thread-focused timeline subscription (open one thread; live diffs +
  back-pagination).
- **C.** Per-room thread list (enumerate threads, paginate, live re-query ping).
- **D.** Threaded sending (text, in-thread reply, and media into a thread).

The SDK (matrix-sdk-ui 0.17) already implements all of this; the `cxx` bridge
currently exposes none of it.

## Relevant existing surface

- `sdk/src/bridge.rs` — `#[cxx::bridge]`: plain-data structs (`TimelineEvent`,
  `RoomInfo`, …), `ClientFfi` methods, and the `EventHandlerBridge` C++ callbacks.
- `sdk/src/client.rs` — `ClientFfi` impl. Key reusable pieces:
  - `subscribe_room` / `subscribe_room_at` (`TimelineFocus::Event`) — timeline build
    runs on a runtime worker thread to dodge the libdispatch small-stack overflow.
  - `spawn_timeline_tasks(...)` — the visible-index `VectorDiff` mirror engine that
    emits `on_timeline_reset/inserted/updated/removed`.
  - The `TimelineEvent` converter (~line 7402) incl. the `in_reply_to` snippet logic.
  - `TimelineHandle { timeline, abort_tasks, is_focused }`,
    `self.timelines: HashMap<OwnedRoomId, TimelineHandle>`, `sync_room_subscriptions`.
- `client/src/event_handler_bridge.cpp` — converts FFI structs via `make_event` and
  forwards to `IEventHandler*`.
- `client/include/tesseract/event_handler.h` — `IEventHandler` virtuals; the 4
  timeline callbacks route purely on `room_id`.
- `client/include/tesseract/types.h` — C++ `Event` struct + subtypes.
- `client/src/ffi_convert.h` — Rust `TimelineEvent` → C++ `Event` mapping.

SDK thread APIs confirmed present in matrix-sdk-ui 0.17:

- `TimelineFocus::Thread { root_event_id }`.
- `content().thread_root() -> Option<OwnedEventId>` (set on in-thread replies).
- `content().thread_summary() -> Option<ThreadSummary>` (set on roots);
  `ThreadSummary { latest_event: TimelineDetails<Box<EmbeddedEvent>>, num_replies: u32,
  public_read_receipt_event_id, private_read_receipt_event_id }`.
- `ThreadListService::{new, items, paginate, subscribe_to_items_updates,
  pagination_state, reset}`; `ThreadListItem { root_event, latest_event, num_replies }`,
  `ThreadListItemEvent { event_id, timestamp, sender, is_own, sender_profile, content }`.
- Threaded sending: ruma `Relation::Thread`, `room::reply::{EnforceThread, Reply}`,
  `ReplyWithinThread`; `Timeline::send_reply` infers threading from focus.

## Architectural decision — thread-timeline callback channel

A thread timeline is mechanically identical to the focused-event timeline. The only
real fork is how thread-timeline diffs reach C++, since the existing 4 callbacks route
on `room_id` and shells treat that arg as a real room.

**Chosen: Option A — new thread callbacks, reused Rust engine.**

- Keep the 4 existing room-timeline callbacks byte-for-byte (zero regression on the
  hot path).
- Add 4 thread-scoped twins carrying `(room_id, thread_root, …)`.
- Keep the Rust diff engine single-source: `spawn_timeline_tasks` (and the converter
  call it makes) gain a `channel: TimelineChannel { Room, Thread(OwnedEventId) }`
  parameter; at each of the 4 emit sites it matches to the room vs thread callback.
- New `IEventHandler` virtuals default to no-ops ⇒ `EventHandlerBase` and shells
  compile unchanged.

Rejected: **Option B** (add a `thread_root` param to the existing 4 callbacks) — wide
mechanical edit through the path we most want left alone. **Option C** (duplicate the
engine) — copies subtle diff-mirror logic; violates the repo's no-duplication ethos.

## Design

### Capability A — thread metadata on timeline events

Extend the FFI `TimelineEvent` struct (`bridge.rs` + the `#[cfg(test)]` stub `ffi`
module in `lib.rs`), populate in the converter (`client.rs`), mirror into C++ `Event`
(`types.h`) and `ffi_convert.h`:

| Field | Source | Meaning |
|---|---|---|
| `thread_root_id: String` | `content().thread_root()` (`""` when absent) | non-empty ⇒ in-thread reply |
| `is_thread_root: bool` | `content().thread_summary().is_some()` | event roots a thread |
| `thread_reply_count: u64` | `thread_summary().num_replies` | replies excluding root |
| `thread_latest_sender_name: String` | `thread_summary().latest_event` when `Ready` | last-reply sender display name |
| `thread_latest_body: String` | same | last-reply snippet (reuse reply-snippet logic: text, `(image)`, `(file)`, `(voice)`, `(sticker)`, `(deleted)`, …) |
| `thread_latest_ts: u64` | same | last-reply timestamp (ms) |

These back the **main-timeline last-reply preview** on root events. The exact accessor
shape of `EmbeddedEvent` (to read sender/content of `latest_event`) is verified during
implementation; the snippet helper is factored to be shared with the existing
`in_reply_to` path.

### Capability B — thread-focused timeline subscription

New FFI methods (mirroring the room-timeline trio):

- `subscribe_thread(room_id, root_event_id) -> OpResult` — build
  `TimelineFocus::Thread { root_event_id }` on a runtime worker thread (same workaround
  as `subscribe_room`); fire an immediate empty `on_thread_reset` then the snapshot;
  live diffs via `spawn_timeline_tasks(..., TimelineChannel::Thread(root))`.
- `unsubscribe_thread(room_id, root_event_id)` — abort tasks, drop handle.
- `paginate_thread_back(room_id, root_event_id, count) -> PaginateResult`.

Storage / lifecycle:

- `thread_timelines: HashMap<(OwnedRoomId, OwnedEventId), TimelineHandle>` — reuses
  `TimelineHandle` verbatim.
- `sync_room_subscriptions` unions room ids from both `timelines` and
  `thread_timelines` so the thread's room stays in the sliding-sync set.
- `Drop` / `stop_sync` abort thread-timeline tasks too.

New `EventHandlerBridge` methods + `IEventHandler` virtuals (default no-op):

```
on_thread_reset(room_id, thread_root, snapshot)
on_thread_inserted(room_id, thread_root, index, event)
on_thread_updated(room_id, thread_root, index, event)
on_thread_removed(room_id, thread_root, index)
```

Indices follow the same visible-index `VectorDiff` semantics as the room callbacks.

### Capability C — per-room thread list

Wrap `ThreadListService`. New FFI `ThreadInfo` struct (flattened from `ThreadListItem`):

```
struct ThreadInfo {
    root_event_id: String,
    root_sender_name: String,
    root_body: String,        // snippet of root content
    root_timestamp: u64,
    latest_event_id: String,
    latest_sender_name: String,
    latest_body: String,      // snippet of latest reply
    latest_timestamp: u64,
    num_replies: u64,
}
```

`latest_*` fields are empty/0 when `ThreadListItem.latest_event` is `None`. Sender names
resolve from `ThreadListItemEvent.sender_profile` when `Ready`, else the bare Matrix ID.
Body snippets reuse the shared snippet helper.

Lifecycle (subscription model):

- `subscribe_room_threads(room_id) -> OpResult` — create the service, kick an initial
  `paginate()`, spawn a watch task on `subscribe_to_items_updates()` that fires
  `on_threads_updated(room_id)` (lightweight re-query ping, mirroring
  `on_image_packs_updated`; avoids a second VectorDiff mirror).
- `list_room_threads(room_id) -> Vec<ThreadInfo>` — current snapshot.
- `paginate_room_threads(room_id) -> PaginateResult` — older threads;
  `reached_start == true` ⇒ list exhausted.
- `unsubscribe_room_threads(room_id)` — drop service + abort watch task.

Storage: `thread_lists: HashMap<OwnedRoomId, ThreadListHandle>` (service + abort handle).
New `IEventHandler` virtual `on_threads_updated(room_id)`, default no-op.

*Downscope option (not chosen):* a one-shot blocking `list_room_threads` that builds a
transient service, paginates once, returns a snapshot — lighter but no live updates.

### Capability D — threaded sending

Text (new):

- `send_thread_message(room_id, thread_root, body, formatted_body) -> OpResult` — posts
  into the thread via `Relation::Thread`; uses the thread's latest known event as the
  `m.in_reply_to` fallback (root if unknown).
- `send_thread_reply(room_id, thread_root, in_reply_to_event_id, body, formatted_body)
  -> OpResult` — reply to a specific message within the thread.

Media (extend existing): add a trailing `thread_root: &str` param (empty = unthreaded,
current behavior) to `send_image`, `send_file`, `send_audio`, `send_video`,
`send_voice`. Combined with the existing `reply_event_id`, this expresses a threaded
media reply via `AttachmentConfig::reply(Reply { enforce_thread: Threaded(…) })`. C++
`Client` wrappers and their callers get the new arg (defaulted to `""`).

Threading verification during implementation: confirm `AttachmentConfig` in 0.17
carries the thread relation through the send-queue path.

Unchanged: `send_message`, `send_reply` (unthreaded) stay as-is.

## Out of scope (this pass)

Deferred — pull into the spec during review if wanted now:

- Per-thread read receipts / `mark_thread_as_read` (`ReceiptThread::Thread`).
- Per-thread notification/highlight counts.
- Flipping `subscribe_room` to `hide_threaded_events: true`. The main timeline keeps
  showing replies inline (zero regression); the latest-reply preview comes from the
  capability-A metadata. The flip becomes a one-line change when the thread UI lands.
- All `ui/` work.

## Testing

- Rust: `#[cfg(not(test))]` impls with `#[cfg(test)]` stubs returning `"not logged in"`,
  exactly like `send_reply` today. Unit tests cover argument validation (invalid
  room/event id) and the relation-building helpers (e.g. `m.relates_to` shape for a
  threaded send, asserted like the existing `send_reply` JSON test). The `#[cfg(test)]`
  stub `ffi` module gains the `ThreadInfo` struct and the `on_thread_*` /
  `on_threads_updated` `EventHandlerBridge` methods.
- C++: ctest covers `make_event` thread-field mapping and `ThreadInfo` → C++ conversion.
- Live-homeserver behavior is not unit-testable (matches the existing limitation for
  send/subscribe paths).

## Deliverable boundary

Rust SDK + `cxx` bridge + C++ `Client` wrapper + `IEventHandler` (new no-op virtuals) +
`types.h` (`Event` thread fields + new `ThreadInfo`/thread-summary C++ types) +
`ffi_convert.h`. No `ui/` changes.

## Files touched (anticipated)

- `sdk/src/bridge.rs` — `TimelineEvent` fields; `ThreadInfo` struct; `on_thread_*` +
  `on_threads_updated` callbacks; thread `ClientFfi` methods; `thread_root` on media
  senders.
- `sdk/src/lib.rs` — mirror the above in the `#[cfg(test)]` stub `ffi` module.
- `sdk/src/client.rs` — converter thread fields; `TimelineChannel`; parameterize
  `spawn_timeline_tasks`; `thread_timelines` + `thread_lists` maps; subscribe/paginate/
  unsubscribe impls; threaded-send impls; `sync_room_subscriptions` union; `Drop`/
  `stop_sync` aborts; `#[cfg(test)]` stubs + tests.
- `client/include/tesseract/event_handler.h` — new no-op virtuals.
- `client/include/tesseract/event_handler_bridge.h` + `client/src/event_handler_bridge.cpp`
  — new forwarders.
- `client/include/tesseract/types.h` — `Event` thread fields; `ThreadInfo` C++ type.
- `client/src/ffi_convert.h` — map new fields + `ThreadInfo`.
- `client/include/tesseract/client.h` + `client/src/client.cpp` — `Client` wrappers for
  the new FFI methods; `thread_root` arg on media senders.
- Tests under the existing Rust test module and the C++ ctest target.
