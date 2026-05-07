# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Tesseract is a cross-platform desktop Matrix/chat client. The core networking is Rust (using `matrix-sdk`), exposed to C++ via a `cxx` FFI bridge. Platform-specific UIs are written in C++ targeting Win32 (Windows), Qt6 Widgets, or GTK4 (Linux).

## Build Commands

**Prerequisites (Linux/Qt6):**

```bash
sudo apt install qt6-base-dev ninja-build cmake libssl-dev sqlite3 libsqlite3-dev
# also requires a Rust toolchain: rustup
```

**Configure and build:**

```bash
cmake --preset linux-qt6-debug          # or windows-debug, linux-gtk-debug, linux-qt6-release
cmake --build build/linux-qt6-debug
./build/linux-qt6-debug/ui/linux-qt/tesseract
```

**Available presets** (in `CMakePresets.json`): `windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-qt6-debug`, `linux-qt6-release`.

**Override UI selection:** `-DTESSERACT_UI=gtk|qt6|win32` (otherwise auto-detected from platform).

Corrosion (CMake↔Cargo bridge) is fetched automatically via `FetchContent` — no global install needed. OpenSSL and SQLite are found via `find_package()`.

## Architecture

```text
sdk/         ← Rust crate: matrix-sdk wrapper + cxx FFI bridge
client/      ← C++ static library: high-level C++ API over the Rust FFI
ui/
  windows/   ← Win32 executable
  linux-qt/  ← Qt6 Widgets executable
  linux-gtk/ ← GTK4 executable
```

### Layer responsibilities

**`sdk/` (Rust)** — All async I/O lives here. A `tokio` runtime runs in background threads. The `cxx` bridge (`sdk/src/lib.rs`) exposes a synchronous C API. `client.rs` wraps `matrix-sdk` for login, sync, and messaging; `oauth.rs` implements the RFC 8252 loopback redirect OAuth flow.

**`client/` (C++)** — `tesseract::Client` (Pimpl) wraps the Rust FFI. `tesseract::IEventHandler` is the interface UIs implement to receive async callbacks (room updates, sync events, session saves). `tesseract::SessionStore` handles platform-specific persistence of the session JSON (`%APPDATA%/Tesseract/` on Windows, `~/.config/tesseract/` on Linux).

**`ui/*/` (C++)** — Each target implements `IEventHandler`. Because Rust callbacks arrive on worker threads, Qt UI must use `QueuedConnection` signals; GTK UI must marshal via glib.

### Key API surface (`client/include/tesseract/`)

- `Client::begin_oauth()` / `Client::await_oauth()` — two-phase OAuth: first call returns the auth URL (open in browser), second blocks until the loopback redirect completes.
- `Client::start_sync()` / `Client::stop_sync()` — background sync loop.
- `Client::send_message()`, `Client::list_rooms()`, `Client::room_messages()` — core messaging.
- `IEventHandler::on_session_saved(json)` — called on token refresh; persist the JSON to resume sessions without re-login.

### Link strategy

The three archives (`tesseract_sdk_bridge_cxx`, `tesseract_client`, `tesseract_sdk_ffi-static`) have a circular dependency through the FFI. They are linked with `WHOLE_ARCHIVE` to guarantee all symbols are present regardless of link order.

## Running Tests

### Rust unit tests (standalone — no C++ required)

```bash
cargo test -p tesseract-sdk-ffi
```

The cxx bridge is excluded from the test build via `#[cfg(not(test))]` so no C++ toolchain is needed.

### C++ tests via CMake / ctest

```bash
cmake --preset linux-qt6-debug
cmake --build build/linux-qt6-debug
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

Catch2 is fetched automatically. Each `TEST_CASE` is registered as a separate ctest test. The `tesseract_tests` executable links the full stack (Rust SDK + bridge + client lib) with the same `WHOLE_ARCHIVE` pattern as the UI targets.

## Key Files

| File | Purpose |
| ---- | ------- |
| `CMakeLists.txt` | Root build: UI detection, Corrosion setup, WHOLE_ARCHIVE linking |
| `CMakePresets.json` | Per-platform build presets |
| `sdk/Cargo.toml` | Rust dependencies (`matrix-sdk`, `cxx`, `tokio`, `tiny_http`) |
| `sdk/src/lib.rs` | `cxx::bridge` — the FFI boundary between Rust and C++ |
| `sdk/src/client.rs` | Matrix SDK wrapper (sync, rooms, messaging) |
| `sdk/src/oauth.rs` | RFC 8252 loopback OAuth implementation |
| `client/include/tesseract/*.hpp` | C++ public API headers |
| `ui/linux-qt/src/MainWindow.h` | Qt UI with EventBridge pattern for thread marshaling |

## Roadmap

The OAuth scaffolding is in place. This is the agreed plan for everything after.

### Status

- **Step 1 done** — OAuth fix-ups: real `logout` (FFI + Rust + C++ + SQLite store wipe), `SessionStore` + restore-or-login on startup, full-`PersistedSession` shape on token refresh, `WHOLE_ARCHIVE` link visibility on Win32 and GTK, `tracing_subscriber` `try_init` hardening.

### Steps in order

**Step 2 — Sliding-sync replacement (SSS-only)**
- Pull `matrix-sdk-ui = "0.11"` with `sqlite` + `e2e-encryption` features.
- Replace the `sync_once` loop with `SyncService` + `RoomListService`; per-room `Timeline` for the active room (kept in `HashMap<RoomId, Timeline>` on `ClientFfi`).
- New FFI: `subscribe_room` / `unsubscribe_room` / `paginate_back`. Drop the `room_messages` stub — push pipeline replaces it.
- `on_room_list_updated` callback fed by `RoomListService` diffs, replacing the manual "after every message, refetch all rooms" hack in the UIs.
- **Mandatory SSS gating**: probe SSS support at the end of `oauth::begin()` (and again inside `restore_session`); if the homeserver doesn't support SSS, fail fast with a distinct error and surface a clear message in `LoginDialog`. No fallback to v2 sync — single code path.

**Step 3 — Spaces (room-list hierarchy)**
- Recognise rooms with `type: m.space`; consume `m.space.child` / `m.space.parent` to build a tree.
- New FFI surface returning the space tree (parent → children → leaf rooms) alongside the flat room list.
- UI: sidebar shifts from a flat list to a tree (or two-pane: spaces / rooms within selected space). Defaults to "All rooms" when no space is selected. Plumbing follows directly from sliding sync — `RoomListService` already exposes spaces; the UI just needs to learn them.
- Space creation / management UI is a follow-up.

**Step 4 — Device verification + key backup (recovery key)**
- `recover(client, key_or_passphrase)` wraps `client.encryption().recovery().recover(...)` and waits for backup steady-state.
- New FFI `needs_recovery`, `recover`, `backup_state`; new callback `on_backup_progress`.
- New `RecoveryDialog` per platform, parallel to `LoginDialog`.
- Launch flow: after login completes, if `needs_recovery()` is true, run `RecoveryDialog`. Skip and Verify both close it; Verify blocks until the SDK reports completion.

**Step 5 — MSC2545 phase A: receive (encrypted-aware)**
- New `sdk/src/image_packs.rs` aggregating `im.ponies.user_emotes`, `im.ponies.emote_rooms`, and per-room `im.ponies.room_emotes` events; live updates via `add_event_handler`.
- FFI carries `MediaSource` as an opaque JSON token (`source_json`) — handles both plain `mxc` and encrypted `EncryptedFile`.
- `fetch_image_bytes(source_json)` routes through `client.media().get_media_content()` for the right plain/encrypted path; cache key = SHA-256 of canonical `MediaSource` JSON.
- Render stickers (`m.sticker`) inline in the timeline; render inline emoticons in HTML body via Qt `QTextDocument::addResource`, GTK `GtkTextChildAnchor`, and (Win32 decision pending) RichEdit or text-only fallback.

**Step 6 — MSC2545 phase B: send**
- `send_emoticon_message(room_id, plain_body, html_body)` and `send_sticker(room_id, shortcode)`.
- Picker UI per platform (popup with tabs + virtualised thumbnail grid).
- `:shortcode:` autocomplete in the input.

**Step 7 — MSC2545 phase C: pack management**
- List enabled packs, toggle subscription via `im.ponies.emote_rooms`, drill into a room's pack from room settings.

**Step 8 — Notifications, layer 1: foreground toasts**
- `tesseract::INotifier` abstraction; per-platform impls (Windows Toast, libnotify via D-Bus, `g_notification_send`).
- Push-rule evaluation through matrix-sdk-ui's `NotificationClient`.
- Suppress notifications for the focused room.

**Step 9 — Notifications, layer 2: server pushers**
- `register_pusher` / `remove_pusher` FFI wrapping `client.pusher().set(...)`.
- Linux: UnifiedPush via D-Bus `org.unifiedpush.Connector1`.
- Windows: deferred (WNS needs Store registration; UnifiedPush distributors on Windows are an option).
- macOS: deferred (APNs).

### Decisions still open

- **Win32 inline images** — RichEdit 4.1 (with `IRichEditOleCallback`) for inline emoticons/stickers, or text-only emoticons + sticker-as-card on Win32 in v1?
- **Disk media cache** — rely on the SDK's sqlite media store, or maintain a separate `<appdata>/tesseract/media-cache/` for faster picker startup?
- **Cross-pack scope in v1** — pinned packs vs all subscribed packs in the picker?
- **Timeline persistence** — opt in to sqlite-backed `Timeline::with_focus(...)`, or memory-only?
- **Room-list window** — `AllRooms` for desktop (recommended), or windowed?
- **Pack-entry encrypted badging** — show a lock glyph on encrypted packs in the picker?
