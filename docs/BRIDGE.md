# The Rust core and the bridge

All Matrix networking lives in Rust, in `sdk/`, on top of `matrix-sdk`. C++ never speaks Matrix directly. This page explains how a call crosses from the C++ side to Rust and how results come back.

Background: [ARCHITECTURE.md](ARCHITECTURE.md) has the diagrams. [SHELLS.md](SHELLS.md) covers the UI side of the boundary.

## The four pieces

```text
ui/shared/app/        ShellBase — decides what to do
        │ calls
client/               tesseract::Client — a plain C++ API (client.h), Pimpl
        │ calls
sdk/src/bridge.rs     the cxx bridge — declared Rust methods and shared types
        │ calls
sdk/src/client/*.rs   ClientFfi — the real work, using matrix-sdk and tokio
```

The boundary is an in-process function call. There is no IPC, no HTTP server of Tesseract's own, and no separate process. [cxx](https://cxx.rs) generates the glue from the declarations in `bridge.rs`.

## Rules of the bridge

1. **The bridge is synchronous.** Every `ClientFfi` method blocks until its result is ready, with one exception: `fetch_media_async` returns at once and reports through the `on_media_ready` callback. Inside, Rust uses `self.rt.block_on(...)` on a per-account tokio runtime. So **never call a `Client` method on the UI thread.** Use a worker pool ([SHELLS.md](SHELLS.md)).
2. **Results come back in two ways.**
   - *Direct:* the return value. Fallible calls return `OpResult { ok, message }`, which the C++ wrapper turns into a `Result`. Reads return plain data structs.
   - *Asynchronous:* Rust calls the `IEventHandler` interface (`client/include/tesseract/event_handler.h`) from tokio threads. Examples are timeline changes, room-list updates, call events and progress. The UI-side handling is in [SHELLS.md](SHELLS.md).
3. **Shared types are declared once**, in the `#[cxx::bridge]` module in `bridge.rs`. The public C++ types in `client/include/tesseract/types.h` mirror them; conversion lives in `client/src/ffi_convert.h`. Views and the app layer use only the `tesseract::` types, never the generated ones.
4. **Locking.** Wrappers in `client.cpp` take a lock on the FFI object. Methods that need exclusive access (`MUT_FFI`) are serialized, which is why state-changing calls run on the single-thread pool. Methods taking `&self` use a shared lock (`SH_FFI`) and may run in parallel.
5. **No panics, no exceptions across the boundary.** C++ callbacks into Rust are wrapped so an exception is logged and dropped. Rust mostly uses `parking_lot` locks, which cannot be poisoned. A few `std::sync` locks remain (for example in `oauth.rs` and `rtc/`). Either way, Rust must not panic on an FFI entry point.
6. **Opaque ids and plain strings.** Room ids, event ids and user ids cross as strings. Rust parses them and returns `err("invalid event_id")` rather than trusting them.

## What lives where in `sdk/`

`ClientFfi` (`sdk/src/client/mod.rs`) holds one account's runtime, Matrix client, sync service, and open timelines. The rest of `sdk/src/client/` is `impl ClientFfi` blocks split by subject. The OAuth login flow itself is `sdk/src/oauth.rs`, outside `client/`; other crate-level helpers (`url_preview.rs`, `markdown.rs`, `net_proxy.rs`, …) sit beside it.

| Subject | Files |
| --- | --- |
| Login and accounts | `session.rs`, `account.rs`, `identity.rs`, `verification.rs`, `qr_grant.rs`, `recovery.rs`, `crypto_reset.rs`, `legacy_login_ffi.rs` |
| Sync, room list, timelines | `sync.rs`, `room_list.rs`, `timeline.rs`, `timeline_convert.rs`, `thread.rs` |
| Sending and room operations | `send.rs`, `pins.rs`, `tags.rs`, `room_directory.rs`, `knock.rs`, `polls.rs`, `profile_fields.rs`, `image_packs.rs`, `bot_commands.rs`, `mutual_rooms.rs` |
| Media | `media.rs`, `media_queue.rs`, `media_gate.rs`, `media_gate_registry.rs`, `media_origin.rs`, `strip_meta.rs`, `strip_av.rs`, `room_media_store.rs`, `url_preview_gen.rs` |
| Local data and background work | `backfill.rs`, `search.rs`, `history_export/`, `notifications.rs`, `activity.rs` |
| Calls | `rtc/`, `rtc_ffi.rs` |
| Optional integrations and misc | `gif.rs`, `maps.rs`, `server_list.rs`, `update.rs`, `text_code.rs` |
| Matrix spec proposals as raw event content | `sdk/src/msc/<mscNNNN>.rs` |

`timeline_convert.rs` turns SDK timeline items into the plain `TimelineEvent` that C++ displays. Most of the "why does this message look wrong" bugs end up there.

## Example: pinning a message

1. **Rust** (`sdk/src/client/pins.rs`): `ClientFfi::pin_event(&self, room_id, event_id) -> OpResult`. It enters the runtime, checks it is logged in, parses the ids, reads the room's pinned list, adds the event, and writes it back. Errors return `err("…")`.
2. **Bridge** (`bridge.rs`, inside `extern "Rust"`): `fn pin_event(self: &ClientFfi, room_id: &str, event_id: &str) -> OpResult;`
3. **Client** (`client/src/client.cpp`): `Result Client::pin_event(...)` takes the shared FFI lock and converts the result with `from_ffi`.
4. **App** (`ui/shared/app/RoomPane.cpp`): `RoomView::on_pin_requested` is wired in `RoomPane::wire_room_view_` to `RoomPane::pin_event_`. That copies the ids, runs the call on the mutation pool with `run_async_mut_`, and for now only logs a failure to stderr.

Adding a call means touching exactly these four places. The recipe is in [DEVELOPING.md](DEVELOPING.md).

## Events from Rust to C++

The reverse direction is declared in the `unsafe extern "C++"` block of `bridge.rs`, using the `EventHandlerBridge` type (`client/include/tesseract/event_handler_bridge.h`). Rust calls those functions on tokio threads. `event_handler_bridge.cpp` forwards each to the `IEventHandler`. Timeline callbacks are index-based and describe one change (`on_message_inserted(room, index, event)`). The index counts only the items Rust forwards. Virtual items (date dividers, the read marker, timeline start) are forwarded as their own events (`DaySeparatorEvent`, `ReadMarkerEvent`, `TimelineStartEvent`). Items that `timeline_item_to_ffi` has no event for are dropped, and so are membership rows when `show_membership_events` is off.

Because these arrive on arbitrary threads and at any time, treat them like network messages: never assume order across rooms, and always hop to the UI thread before touching a widget.

## Things that bite

- **Do not create a second FFI entry for something matrix-sdk already does.** Look in the SDK source before writing manual HTTP or JSON in `sdk/src/`.
- **Do not patch or reimplement the protocol.** Event-content MSCs may be handled as raw JSON under `sdk/src/msc/`.
- **Features of the SDK are chosen in `sdk/Cargo.toml`.** Keep the default `automatic-room-key-forwarding` feature on: it lets your other devices supply keys for old messages that key backup lacks, and turning it off made old rooms undecryptable.
- **cxx does not see Cargo features.** A bridge function that exists only with a feature still needs a stub when it is off (see `server_list`).
- **Don't use `CxxString` or `CxxVector` anywhere in `bridge.rs`, arguments included.** `webrtc-sys` is built against Chromium's libc++, whose `std::string` layout differs from libstdc++'s, so such values can be read wrongly and crash. Use `String` or `Vec<String>`. The ctest `bridge_no_cxx_strings` (`tests/check_bridge_strings.py`) fails if either appears.
- **Tests:** the bridge is excluded from the Rust test build (`#[cfg(not(test))]`), so logic you want to unit-test in Rust must not depend on the generated types. `cargo test -p tesseract-sdk-ffi` needs no C++ toolchain.
