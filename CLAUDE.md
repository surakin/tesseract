# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Tesseract is a cross-platform desktop Matrix/chat client. The core networking is Rust (using `matrix-sdk`), exposed to C++ via a `cxx` FFI bridge. Platform-specific UIs are written in C++ targeting Win32 (Windows), AppKit/Objective-C++ (macOS), Qt6 Widgets, or GTK4 (Linux).

## Build Commands

**Prerequisites (Linux/Qt6):**

```bash
sudo apt install qt6-base-dev ninja-build cmake libssl-dev sqlite3 libsqlite3-dev
# also requires a Rust toolchain: rustup
```

**Prerequisites (macOS/AppKit):**

```bash
brew install openssl@3 ninja cmake
xcode-select --install   # Xcode Command Line Tools
# also requires a Rust toolchain: rustup
```

**Configure and build:**

```bash
cmake --preset linux-qt6-debug          # or windows-debug, linux-gtk-debug, linux-qt6-release
cmake --build build/linux-qt6-debug
./build/linux-qt6-debug/ui/linux-qt/tesseract

# macOS:
cmake --preset macos-native-debug
cmake --build build/macos-native-debug
open build/macos-native-debug/ui/macos/Tesseract.app
```

**Available presets** (in `CMakePresets.json`): `windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-qt6-debug`, `linux-qt6-release`, `macos-native-debug`, `macos-native-release`.

**Override UI selection:** `-DTESSERACT_UI=gtk|qt6|win32|macos` (otherwise auto-detected from platform).

Corrosion (CMake↔Cargo bridge) is fetched automatically via `FetchContent` — no global install needed. OpenSSL and SQLite are found via `find_package()`.

## Architecture

```text
sdk/         ← Rust crate: matrix-sdk wrapper + cxx FFI bridge
client/      ← C++ static library: high-level C++ API over the Rust FFI
ui/
  windows/   ← Win32 executable
  macos/     ← AppKit / Objective-C++ executable (.app bundle)
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
| `client/include/tesseract/*.h` | C++ public API headers |
| `ui/linux-qt/src/MainWindow.h` | Qt UI with EventBridge pattern for thread marshaling |

## Roadmap

The OAuth scaffolding is in place. This is the agreed plan for everything after.

### Status

- **Step 1 done** — OAuth fix-ups: real `logout` (FFI + Rust + C++ + SQLite store wipe), `SessionStore` + restore-or-login on startup, full-`PersistedSession` shape on token refresh, `WHOLE_ARCHIVE` link visibility on Win32 and GTK, `tracing_subscriber` `try_init` hardening.
- **Step 2 done** — Sliding-sync replacement: `matrix-sdk-ui = "0.16.1"` added; `SyncService` + `RoomListService` replace the `sync_once` loop; per-room `Timeline` held in `HashMap<OwnedRoomId, TimelineHandle>` on `ClientFfi`; new FFI `subscribe_room` / `unsubscribe_room` / `paginate_back` + `on_timeline_reset` callback; `room_messages` stub dropped; room-list driven by `RoomListService` diffs (UIs no longer call `list_rooms` after every message); SSS probed only at `oauth::begin()` (not restore_session).
- **Step 3 done** — Room avatar polish: `kRoomAvatarSize = 36` constant; explicit `setIconSize` on the room list widget so all cells have a uniform slot.
- **Step 4 done** — Message sender identity + media infrastructure: `sender_name` + `sender_avatar_url` fields in `TimelineEvent`; `ImageEvent` / `FileEvent` C++ types with `source_json` / `file_json`, dimensions, and file-size fields; `fetch_media_bytes(mxc_url)` and `fetch_source_bytes(source_json)` FFI (the latter handles encrypted `EncryptedFile` transparently); 21 C++ tests covering all event subtypes and `EventType` enum values; Qt UI shows 24 × 24 inline sender avatar per message row.
- **Stability hardening done** — `soft_logout` flag threaded from `SessionChange::UnknownToken` through `on_error` to all UIs (soft logout retries restore without clearing the store); tombstoned (upgraded) rooms filtered out of the room list; `Drop` impl on `ClientFfi` calls `stop_sync()` for graceful shutdown; `matrix-sdk` upgraded to 0.16.1.
- **Step 5, in progress** — Inline image rendering done: GTK4 now renders `m.image` as a `GtkPicture` (max 320×200) via `GdkPixbufLoader`; Qt6 inline pixmap was already in place. MSC2530 caption rule applied: `body` is shown beneath the image only when the sender supplies a distinct `filename` field. Sticker events (`m.sticker`) done end-to-end: new `StickerEvent` C++ type + `EventType::Sticker`, Rust `MsgLikeKind::Sticker` FFI arm (`StickerMediaSource` matched), borderless 256×256 thumbnail in both Qt6 and GTK4. Scroll-to-bottom timing fixed: Qt6 uses `QScrollBar::rangeChanged`, GTK4 uses `notify::upper` on the vadjustment (both fire after layout, not before). 24 C++ tests pass. Remaining Step 5 items: message bubbles/cards, thread support, emoji reactions/picker, compose bar polish, sidebar.

**Step 5 — UI redesign**

Goal: a visually polished, modern chat layout that forms the shell for threads, stickers, and emoji in later steps.

- **Thread support** — reply-to indicator on each message row (show quoted snippet + sender); threaded reply panel slides in from the right when a thread is opened; new FFI `send_reply(room_id, event_id, body)` wrapping `Timeline::send_reply`.
- **Emoji reactions** — reaction bar below each message showing emoji + count; tap to toggle; new FFI `send_reaction(room_id, event_id, key)` / `redact_reaction`; reactions carried in `TimelineEvent` as `Vec<(emoji, count, reacted_by_me)>`.
- **Emoji picker** — bottom-bar button opens a floating picker (tabbed: frequently-used + Unicode categories). Inserts into the compose field. Separate from sticker packs (MSC2545, Step 9).
- **Inline image / file rendering** ✓ — `m.image` renders as a thumbnail (max 320×200) in Qt6 and GTK4; MSC2530 caption rule applied (`body` shown only when sender supplies a distinct `filename` field). `m.sticker` pulled forward from Step 8: borderless 256×256 inline thumbnail, no caption. Files still render as a card with icon, name, and size.
- **Compose bar polish** — multi-line expanding input, send-on-Enter (Shift+Enter for newline), `/` command hint, typing indicator sent to the room.
- **Sidebar** — room list gets unread badge, last-message preview, and last-activity sort; DM rooms show the other user's avatar; spaces/groups deferred to Step 7.

**Step 6 — Device verification + key backup (recovery key)**
- `recover(client, key_or_passphrase)` wraps `client.encryption().recovery().recover(...)` and waits for backup steady-state.
- New FFI `needs_recovery`, `recover`, `backup_state`; new callback `on_backup_progress`.
- New `RecoveryDialog` per platform, parallel to `LoginDialog`.
- Launch flow: after login completes, if `needs_recovery()` is true, run `RecoveryDialog`. Skip and Verify both close it; Verify blocks until the SDK reports completion.

**Step 7 — Spaces (room-list drill-in navigation)**
- `is_space: bool` added to `RoomInfo` (Rust FFI + C++ type). Spaces appear at the **bottom** of the room list with a `#` prefix in Qt6/GTK4.
- New FFI `space_children(space_id) -> Vec<String>`: returns direct children of a space (rooms/sub-spaces the client is joined to) via `m.space.child` state events.
- UI: stack-based drill-in model — selecting a space replaces the room list with that space's children; a **Back** button (`←`) + space name appear at the top of the sidebar; back pops one level (returns to "All rooms" when stack is empty). Sub-spaces can be drilled into recursively.
- `onRoomsUpdated` / `push_rooms` now call `refreshRoomList()` / `refresh_room_list()` to preserve the current navigation level on live updates.
- Space creation / management UI is a follow-up.

**Step 8 — MSC2545 phase A: receive (encrypted-aware)**
- New `sdk/src/image_packs.rs` aggregating `im.ponies.user_emotes`, `im.ponies.emote_rooms`, and per-room `im.ponies.room_emotes` events; live updates via `add_event_handler`.
- FFI carries `MediaSource` as an opaque JSON token (`source_json`) — handles both plain `mxc` and encrypted `EncryptedFile`. `fetch_source_bytes` (already in place) is the download path.
- Render stickers (`m.sticker`) inline in the timeline ✓ done (pulled forward to Step 5; see status above); render inline emoticons in HTML body via Qt `QTextDocument::addResource`, GTK `GtkTextChildAnchor`, and (Win32 decision pending) RichEdit or text-only fallback.

**Step 9 — MSC2545 phase B: send**
- `send_emoticon_message(room_id, plain_body, html_body)` and `send_sticker(room_id, shortcode)`.
- Picker UI per platform (popup with tabs + virtualised thumbnail grid). Dovetails with the emoji picker from Step 5.
- `:shortcode:` autocomplete in the compose input.

**Step 10 — MSC2545 phase C: pack management**
- List enabled packs, toggle subscription via `im.ponies.emote_rooms`, drill into a room's pack from room settings.

**Step 11 — Notifications, layer 1: foreground toasts**
- `tesseract::INotifier` abstraction; per-platform impls (Windows Toast, libnotify via D-Bus, `g_notification_send`).
- Push-rule evaluation through matrix-sdk-ui's `NotificationClient`.
- Suppress notifications for the focused room.

**Step 12 — Notifications, layer 2: server pushers**
- `register_pusher` / `remove_pusher` FFI wrapping `client.pusher().set(...)`.
- Linux: UnifiedPush via D-Bus `org.unifiedpush.Connector1`.
- Windows: deferred (WNS needs Store registration; UnifiedPush distributors on Windows are an option).
- macOS: deferred (APNs).

### Decisions still open

- **Win32 inline images** — RichEdit 4.1 (with `IRichEditOleCallback`) for inline images/stickers, or text-only emoticons + attachment-card on Win32 in v1?
- **Disk media cache** — rely on the SDK's sqlite media store, or maintain a separate `<appdata>/tesseract/media-cache/` for faster picker startup?
- **Cross-pack scope in v1** — pinned packs vs all subscribed packs in the sticker/emoji picker?
- **Timeline persistence** — opt in to sqlite-backed `Timeline::with_focus(...)`, or memory-only?
- **Room-list window** — `AllRooms` for desktop (recommended), or windowed?
- **Pack-entry encrypted badging** — show a lock glyph on encrypted packs in the picker?
- **Thread panel layout** — slide-in panel (Telegram style) vs inline thread expansion (Discord style) vs separate window?
