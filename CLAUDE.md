# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Tesseract is a cross-platform desktop Matrix/chat client. The core networking is Rust (using `matrix-sdk`), exposed to C++ via a `cxx` FFI bridge. Platform-specific UIs are written in C++ targeting Win32 (Windows), AppKit (Objective-C++, macOS), Qt6 Widgets, or GTK4 (Linux).

## Build Commands

**Prerequisites (Linux/Qt6):**

```bash
sudo apt install qt6-base-dev qt6-multimedia-dev ninja-build cmake sqlite3 libsqlite3-dev golang perl
# also requires a Rust toolchain: rustup
# (golang + perl are build-only deps for aws-lc-sys's CMake builder.)
# qt6-multimedia-dev powers MSC3245 voice-message playback via QMediaPlayer.
# For GTK4 builds also install: libgtk-4-dev libgstreamer1.0-dev
#                               libgstreamer-plugins-base1.0-dev
#                               libayatana-appindicator3-dev
# (gst-plugins-base provides the opus decoder voice messages use.)
# (libayatana-appindicator3-dev powers the system tray; it transitively
#  pulls GTK3 into the process — see the system-tray status block below.)
```

**Prerequisites (macOS / AppKit):**

```bash
brew install ninja cmake go
xcode-select --install   # Xcode Command Line Tools
# Rust toolchain + native macOS targets (pick the one matching the preset):
#   rustup toolchain install stable
#   rustup target add aarch64-apple-darwin   # Apple Silicon
#   rustup target add x86_64-apple-darwin    # Intel
# (go is a build-only dep for aws-lc-sys's CMake builder; perl ships with macOS.)
```

**Configure and build:**

```bash
cmake --preset linux-qt6-debug          # or windows-debug, linux-gtk-debug, linux-qt6-release
cmake --build build/linux-qt6-debug
./build/linux-qt6-debug/ui/linux-qt/tesseract

# macOS (pick the preset matching your CPU):
cmake --preset macos-appkit-arm64-debug         # or macos-appkit-x86_64-debug
cmake --build build/macos-appkit-arm64-debug
open build/macos-appkit-arm64-debug/ui/macos/Tesseract.app
```

**Available presets** (in `CMakePresets.json`): `windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-qt6-debug`, `linux-qt6-release`, `macos-appkit-arm64-debug`, `macos-appkit-arm64-release`, `macos-appkit-x86_64-debug`, `macos-appkit-x86_64-release`.

**Override UI selection:** `-DTESSERACT_UI=gtk|qt6|win32|macos` (otherwise auto-detected from platform).

Corrosion (CMake↔Cargo bridge) is fetched automatically via `FetchContent` — no global install needed. SQLite is compiled in-tree via matrix-sdk's `bundled-sqlite` feature; TLS uses rustls (no system OpenSSL).

## Architecture

```text
sdk/         ← Rust crate: matrix-sdk wrapper + cxx FFI bridge
client/      ← C++ static library: high-level C++ API over the Rust FFI
ui/
  shared/    ← tesseract_tk: cross-platform widget toolkit + shared views
    tk/        ← Canvas / Widget / Layout / Host abstractions + per-backend impls
    views/     ← LoginView, RoomListView, MessageListView, EmojiPicker,
                  RecoveryBanner, ComposeBar
  windows/   ← Win32 executable (thin shell)
  macos/     ← AppKit executable (.app bundle, thin shell)
  linux-qt/  ← Qt6 Widgets executable (thin shell)
  linux-gtk/ ← GTK4 executable (thin shell)
```

### Layer responsibilities

**`sdk/` (Rust)** — All async I/O lives here. A `tokio` runtime runs in background threads. The `cxx` bridge (`sdk/src/lib.rs`) exposes a synchronous C API. `client.rs` wraps `matrix-sdk` for login, sync, and messaging; `oauth.rs` implements the RFC 8252 loopback redirect OAuth flow.

**`client/` (C++)** — `tesseract::Client` (Pimpl) wraps the Rust FFI. `tesseract::IEventHandler` is the interface UIs implement to receive async callbacks (room updates, sync events, session saves). `tesseract::SessionStore` handles platform-specific persistence of the session JSON (`%APPDATA%/Tesseract/` on Windows, `~/.config/tesseract/` on Linux).

**`ui/shared/` (C++)** — `tesseract_tk` is the cross-platform UI toolkit. It owns drawing, layout, hit-test, focus, and keyboard. `tk::Canvas` is the abstract 2D backend (D2D on Win32, QPainter on Qt6, Cairo+Pango on GTK4, CoreGraphics+CoreText on macOS). `tk::Host` is the per-platform integration surface (repaint scheduling, post-to-UI, native edit overlays). Shared widget classes live under `tk/`; shared views (LoginView, RoomListView, MessageListView, EmojiPicker, RecoveryBanner, ComposeBar) live under `views/`. Text input stays native via `tk::NativeTextField` / `tk::NativeTextArea` overlays so IME and selection behave correctly per-OS.

**`ui/*/` (C++)** — Each platform target is a thin native shell that owns the window/menu/AX surface and mounts one or more `tk::*::Surface`s hosting shared views. Each shell implements `IEventHandler`. Because Rust callbacks arrive on worker threads, the shells route through `tk::Host::post_to_ui`, which is backed by `QueuedConnection` (Qt6), `g_idle_add` (GTK4), `PostMessage` (Win32), and `dispatch_async(dispatch_get_main_queue())` (macOS).

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
| `ui/shared/tk/canvas.h` | Abstract 2D backend interface (Color/Rect/Point/Image/TextLayout) |
| `ui/shared/tk/host.h` | Per-platform `Host` + `NativeTextField` / `NativeTextArea` overlays |
| `ui/shared/tk/widget.h` | Widget tree base: measure → arrange → paint + pointer dispatch |
| `ui/shared/views/*.h` | Cross-platform views mounted by every native shell |
| `ui/linux-qt/src/MainWindow.h` | Qt6 shell with `EventBridge` for thread marshaling |
| `ui/macos/src/MainWindowController.mm` | AppKit shell (`NSWindowController` + `NSSplitView`) |

## Roadmap

The OAuth scaffolding is in place. This is the agreed plan for everything after.

### Status

- **Step 1 done** — OAuth fix-ups: real `logout` (FFI + Rust + C++ + SQLite store wipe), `SessionStore` + restore-or-login on startup, full-`PersistedSession` shape on token refresh, `WHOLE_ARCHIVE` link visibility on Win32 and GTK, `tracing_subscriber` `try_init` hardening.
- **Step 2 done** — Sliding-sync replacement: `matrix-sdk-ui = "0.16.1"` added; `SyncService` + `RoomListService` replace the `sync_once` loop; per-room `Timeline` held in `HashMap<OwnedRoomId, TimelineHandle>` on `ClientFfi`; new FFI `subscribe_room` / `unsubscribe_room` / `paginate_back` + `on_timeline_reset` callback; `room_messages` stub dropped; room-list driven by `RoomListService` diffs (UIs no longer call `list_rooms` after every message); SSS probed only at `oauth::begin()` (not restore_session).
- **Step 3 done** — Room avatar polish: `kRoomAvatarSize = 36` constant; explicit `setIconSize` on the room list widget so all cells have a uniform slot.
- **Step 4 done** — Message sender identity + media infrastructure: `sender_name` + `sender_avatar_url` fields in `TimelineEvent`; `ImageEvent` / `FileEvent` C++ types with `source_json` / `file_json`, dimensions, and file-size fields; `fetch_media_bytes(mxc_url)` and `fetch_source_bytes(source_json)` FFI (the latter handles encrypted `EncryptedFile` transparently); 21 C++ tests covering all event subtypes and `EventType` enum values; Qt UI shows 24 × 24 inline sender avatar per message row.
- **Stability hardening done** — `soft_logout` flag threaded from `SessionChange::UnknownToken` through `on_error` to all UIs (soft logout retries restore without clearing the store); tombstoned (upgraded) rooms filtered out of the room list; `Drop` impl on `ClientFfi` calls `stop_sync()` for graceful shutdown; `matrix-sdk` upgraded to 0.16.1.
- **Step 5, in progress** — Inline image rendering done: GTK4 now renders `m.image` as a `GtkPicture` (max 320×200) via `GdkPixbufLoader`; Qt6 inline pixmap was already in place. MSC2530 caption rule applied: `body` is shown beneath the image only when the sender supplies a distinct `filename` field. Sticker events (`m.sticker`) done end-to-end: new `StickerEvent` C++ type + `EventType::Sticker`, Rust `MsgLikeKind::Sticker` FFI arm (`StickerMediaSource` matched), borderless 256×256 thumbnail in both Qt6 and GTK4. Scroll-to-bottom timing fixed: Qt6 uses `QScrollBar::rangeChanged`, GTK4 uses `notify::upper` on the vadjustment (both fire after layout, not before). Reply-to indicator ✓, message editing ✓ — see detail below. Remaining Step 5 items: message bubbles/cards, threaded reply panel, emoji reactions, sidebar polish.
- **Step 5a done — shared UI toolkit (`tesseract_tk`)** — Introduced `ui/shared/` as a cross-platform widget toolkit. `tk::Canvas` is the abstract 2D backend with four concrete impls (`canvas_d2d`, `canvas_qpainter`, `canvas_cairo`, `canvas_cg`). `tk::Widget` provides measure/arrange/paint + pointer/wheel dispatch with `dispatch_pointer_down` + `world_to_local` capture semantics. `tk::Host` integrates each platform's window, repaint loop, and post-to-UI channel; native text input stays native via `NativeTextField` (single-line: `QLineEdit` / `GtkEntry` / Win32 EDIT / `NSTextField`) and `NativeTextArea` (multi-line: `QTextEdit` / `GtkTextView` / multi-line EDIT / `NSTextView`). All four platforms now mount the same shared views — `LoginView`, `RoomListView`, `MessageListView`, `EmojiPicker`, `RecoveryBanner`, `ComposeBar` — through a `tk::Surface` parented inside the native shell. 151 C++ tests cover canvas, widget tree, list-view virtualisation, EmojiPicker, RecoveryBanner, ComposeBar, reply UI, and message editing.
- **macOS migration done** — Port moved off Mac Catalyst (UIKit on `aarch64-apple-ios-macabi`) onto native AppKit (`NSWindow` / `NSWindowController` / `NSView`). Two architecture-specific presets (`macos-appkit-arm64-*`, `macos-appkit-x86_64-*`) replace the Catalyst preset. `tk::macos::Surface` is an `NSView` subclass with `isFlipped == YES` so the CG context matches the top-left convention the rest of `canvas_cg.cpp` expects. The legacy `RoomListController` / `MessageListController` / `AvatarCache` / `EventBridge` Obj-C classes were deleted; their behaviour moved into `MainWindowController` + the shared views. The native `ComposeBar.{h,mm}` was also deleted once the shared `ComposeBar` + `NativeTextArea` landed.
- **macOS shell stabilisation + cross-platform fixes (2026-05-13)** — Build: `host_macos.mm` `#import <UniformTypeIdentifiers>` guarded behind `__MAC_OS_X_VERSION_MAX_ALLOWED >= 110000` for old 10.15 CommandLineTools SDK compatibility. macOS sidebar: user identity strip added (48 px NSView at bottom — circular 32×32 `NSImageView` + display name label; async avatar fetch off main thread; initials-circle fallback; right-click "Log Out" via `NSClickGestureRecognizer`); space back-button bar added (36 px NSView at top — `←` `NSButton` + space name label; shown/hidden by `_refreshRoomList` as `_spaceStack` changes; both zones use height-constraint collapse to 0 when hidden so the room surface fills the sidebar). macOS session restore: `SessionStore::save(_client.export_session())` now called immediately in `loginViewDidSucceed` (was missing — app asked for login again on every restart because the session file was never written until the first Rust token-refresh callback). macOS login flash: `_loginView.hidden = YES` before `addSubview:` so it doesn't appear in the frame between `showWindow:` and the `dispatch_async` `beginLogin` call. Emoji/StickerPicker tab centering (all platforms): `halign`/`valign`/`max_width`/`max_height` removed from the tab-glyph `TextStyle` — they were causing double-centering (the canvas backend centred within the max-width frame, then the caller also manually offset by `(tab.w − sz.w) / 2`). Space children hidden from root room list (all platforms): `refreshRoomList` / `_refreshRoomList` / `refresh_room_list` now builds a set of every room ID that is a child of any space and excludes those rooms from the top-level list; they appear only when the user navigates into their parent space.
- **Step 8 partly done — MSC2545 receive + send, Qt6 / GTK4 / Win32 end-to-end** — New `sdk/src/image_packs.rs` aggregator (16 unit tests) parses user pack (`im.ponies.user_emotes` / `m.image_pack`), enabled-rooms list (`im.ponies.emote_rooms` / `m.image_pack.rooms`), and each referenced room state event (`im.ponies.room_emotes` / `m.room.image_pack`). Spec-correct usage semantics: missing/empty `usage` → both sticker + emoticon allowed; per-image `usage` overrides pack-level. Encrypted-sticker decryption enabled via direct `ruma = { features = ["compat-encrypted-stickers"] }` dep; sticker timeline events now emit JSON-encoded `MediaSource` for the encrypted variant; `fetch_source_bytes` actually deserialises that JSON (was previously plain-mxc only despite docs claiming otherwise). New FFI: `list_image_packs`, `list_pack_images`, `list_favorite_stickers`, `send_sticker`, `save_sticker_to_user_pack`, `user_pack_has_sticker`, `toggle_favorite_sticker`; new `IEventHandler::on_image_packs_updated` callback fires whenever the pack cache is rebuilt (piggy-backs on the existing room-info notable-update tick). New shared view `tesseract::views::StickerPicker` (parallel to `EmojiPicker`: Favorites + per-pack tabs, search, virtualised grid via `tk::GridView`). `EmojiPicker` extended with `CustomPack` tabs after the Unicode categories — clicking a custom emoticon fires `on_emoticon_selected(ImagePackImage)`. `ComposeBar` grows a sticker button (🖼) wired through `on_sticker`. `MessageListView` records sticker rects per-paint and exposes `sticker_hit_at(world)` for right-click hit-testing. **Qt6** wired end-to-end: floating `StickerPicker` QFrame, `QMenu` "Add to Saved Stickers" via `customContextMenuRequested`, `EventBridge::imagePacksUpdated` signal refreshes the picker. **GTK4** wired end-to-end: `GtkPopover` for the sticker picker, `GtkPopoverMenu` + `GMenu` model for the context menu, `GtkGestureClick` (button = `GDK_BUTTON_SECONDARY`) on the message surface, `g_idle_add` marshalling for `on_image_packs_updated`. **Win32** wired: floating `WS_POPUP` HWND parents a `tk::win32::Surface` hosting the shared `StickerPicker`; `EventHandler::on_image_packs_updated` posts `WM_TESSERACT_IMAGE_PACKS`; selection routes through `Client::send_sticker`; picker image provider hits `tk_anim_images_` / `tk_images_` first and falls back to `request_sticker_image` (worker thread → `fetch_source_bytes` → `WM_TESSERACT_STICKER_BYTES` → decode + cache + repaint) for stickers that haven't appeared in any message yet. Right-click context menu still pending. Tests: 3 Catch2 cases (`test_tk_sticker_picker.cpp`); 125/125 ctest pass under all four presets, 42 Rust unit tests pass.
- **Read receipts + hover timestamps — all four platforms** — New `ReadReceipt` shared struct (FFI + `tesseract::ReadReceipt` + `MessageRowData::read_receipts`); Rust side aggregates `EventTimelineItem::read_receipts()` via a `collect_read_receipts(event_item, room, me)` helper that resolves display name + avatar through `room.get_member_no_sync` and filters out the current user. `MessageListView` paints up to 5 mini-avatar discs (16 px, 5 px overlap) at the bottom-right of each row, with a `+N` overflow pill anchored to the left of the cluster when more users are caught up. The always-visible right-aligned `HH:MM` timestamp is gone; in its place a hover-only `HH:MM` paints under the sender avatar (left column, otherwise empty) using the existing `hovered_row_index()` state. Receipts share the row's existing chip-strip height — no extra rows of chrome. Avatars feed through the same `tk_avatars_` cache the sender avatar uses; every shell (`ensure_row_media`/`ensureRowMedia`/`_ensureRowMedia`) loops `ev.read_receipts` and primes the cache through `ensure_user_avatar`. Tests: `MessageListView read receipts paint inside existing row bounds`, `… cluster + overflow at bottom-right`, `… hover timestamp under the avatar` (130/130 ctest under Qt6 + GTK4, 42 Rust unit tests pass).
- **Voice messages (MSC3245) — receive + playback** — `m.audio` events tagged with `org.matrix.msc3245.voice` now decode end-to-end. New `MessageType::Audio` arm in `timeline_item_to_ffi` (gated on the `unstable-msc3245-v1-compat` feature on ruma); voice events surface as `msg_type = "m.voice"` carrying `audio_source_json` (full MediaSource for plain or encrypted clips), `audio_duration_ms`, `audio_waveform` (MSC1767 amplitudes 0..=1024), and `audio_mime`. Plain `m.audio` (no voice marker) folds through the existing file-card path so non-voice audio still renders. New C++ `VoiceEvent` / `EventType::Voice`; shared `MessageListView` paints a 280×48 rounded voice card with play/pause circle, waveform strip (flat placeholder bars when the sender omits MSC1767 data), and mm:ss duration label (remaining-time during playback). The waveform strip is **scrubbable**: click or drag anywhere on the bars to seek; clicking on a non-active row's waveform starts playback at the chosen position. A **speed pill** (`1×` / `1.5×` / `2×`) appears on the active row to the left of the duration label and cycles the global playback rate on tap. **Background prefetch**: each shell kicks off a worker (`QThreadPool` / `std::thread` / `dispatch_async`) when a Voice row is first seen, warming the SDK media cache so the first play tap is instant. Per-platform `tk::AudioPlayer` backend: Qt6 = `QMediaPlayer` + `QAudioOutput` fed from a `QBuffer` (`setPosition`/`setPlaybackRate`); GTK4 = GStreamer `giostreamsrc` ! `decodebin` ! `audioconvert` ! `autoaudiosink` pipeline (`gst_element_seek` with a non-1.0 rate for both seek and speed); macOS = `AVAudioPlayer initWithData:` + `NSTimer` (`currentTime` for seek, `enableRate`/`rate` for speed); Win32 = no backend yet (`make_audio_player()` returns `nullptr`, voice card renders but play is a no-op — prefetch still runs so a future backend benefits from a warm cache). 4 new C++ tests (`VoiceEvent` default/round-trip/empty-waveform/reactions) + 3 new Rust tests (MSC3245 JSON parse, plain-audio rejection, amplitude saturation); 132/132 ctest under Qt6 + GTK4, 45 Rust unit tests pass.
- **Recent emoji (MSC4356) — receive + write** — Dropped the `experimental-element-recent-emojis` matrix-sdk feature; recent-emoji state now lives in MSC4356's `m.recent_emoji` / `io.github.johennes.msc4356.recent_emoji` account-data, dual-written on every `recent_emoji_bump` and read with stable → unstable → legacy (`io.element.recent_emoji`) precedence so existing Element users don't lose their picker rank. New self-contained `sdk/src/recent_emoji.rs` module owns parse/bump/serialize semantics: array-of-objects wire format (`{ "emoji", "total" }`), move-to-front-and-increment on bump, 100-entry cap trimmed from the tail, top-N selection via stable count-desc sort. Both legacy shapes are tolerated on read (Element's documented nested-array and the object-map fork). 16 new Rust unit tests cover round-trip, malformed-blob rejection, dedup, cap, ties, and `u64::MAX` saturation; the C++ FFI surface (`recent_emoji_top` / `recent_emoji_bump`) and every existing `EmojiPicker` consumer is untouched (132/132 ctest under Qt6 + GTK4, 61 Rust unit tests pass).
- **Media fetches moved off the UI thread — all four platforms** — `Client::fetch_avatar_bytes` and `Client::fetch_media_bytes` are sync FFI wrappers that `block_on` a tokio future inside; calling them from `ensure_room_avatar` / `ensure_user_avatar` / `ensure_media_image` ran N serial network round-trips on the UI thread during room-list paint, freezing the message pump for minutes on accounts with many rooms (the Windows callstack landed deep inside `parking_lot::Condvar::wait_until_internal` from `tokio::runtime::scheduler::multi_thread::block_on` on every room avatar). Each shell now spawns a worker per cache miss and bounces decoded bytes back to the UI thread, mirroring the existing sticker-picker pattern: **Win32** adds `WM_TESSERACT_MEDIA_BYTES` + a `MediaBytesPayload` (`MediaKind { RoomAvatar, UserAvatar, MediaImage }`) routed through `std::thread` + `PostMessage`; decode + cache + `InvalidateRect` runs in the WM handler. **Qt6** emits a new `mediaBytesLoaded_` signal from `QRunnable`s on the global `QThreadPool`; `Qt::QueuedConnection` lands the bytes back on the UI thread, where `QImage::loadFromData` / `QImageReader` decode and `mediaImageSizes_` pins the `(max_w, max_h)` for `MediaImage` requests so the slot can scale them. **GTK4** spawns `std::thread` + `g_idle_add`; the idle callback runs `decode_image_to_cairo_surface` / `decode_animation` and `relayout()`s the room/message surface. **macOS** mirrors the existing `_ensureStickerImageAsync` shape: `std::thread` + `dispatch_async(dispatch_get_main_queue(), ...)`, decode reuses `_decodeAndCache` / `_decodeMediaBytes`. Each shell has its own `media_fetches_in_flight_` (`_mediaFetchesInFlight` on macOS) set to dedupe concurrent requests; the views already paint initials/placeholder when the cache misses, so the room list now renders instantly and avatars fade in as bytes land.
- **Internationalisation (i18n) — Qt6 + GTK4** — Per-platform approach: Qt6 uses `QObject::tr()` + `QTranslator` (loads `share/translations/tesseract_<locale>.qm` at startup; `i18n_extract_qt` CMake target runs `lupdate` to produce a `.ts` template); GTK4 uses GNU gettext (`#define _(s) gettext(s)`, `bindtextdomain` + `textdomain` in `main()`; `i18n_extract_gtk` target runs `xgettext` to produce `i18n/gtk/tesseract.pot`). All user-visible shell strings wrapped on both platforms (status bar, error messages, menus, placeholders). Shared views (`ui/shared/views/`) stay in English — translated via each platform's mechanism when the host passes strings in. macOS (`NSLocalizedString`) and Win32 (`LoadString`) not yet wired.

- **Room list sections — all four platforms** — `RoomListView` now classifies rooms into four collapsible sections (Favorites / Direct Messages / Rooms / Spaces). Each section header is a 28 px clickable strip with a `▾`/`▸` chevron; clicking toggles collapse. Empty sections are hidden. When a search query is active the collapse state is ignored so all matches are visible. `is_favorite: bool` added to the `RoomInfo` FFI struct (backed by `room.tags().await` checking `TagName::Favorite`). All 4 shells see the new classification automatically through `RoomListView`. 4 new Catch2 tests (`test_tk_lists.cpp`); 157/157 ctest pass, 65 Rust unit tests pass.

- **Account-data prefs (`im.gnomos.tesseract`) — all four platforms** — Application preferences (currently `last_room`) are now stored as a Matrix global account-data event keyed `im.gnomos.tesseract` instead of a local `last_room.txt` file. This makes prefs per-account and cross-device. New FFI: `load_prefs` (reads from SDK SQLite cache, no network), `save_prefs` (fire-and-forget PUT identical to `recent_emoji_bump`), `on_account_prefs_updated` callback fired once at startup (before the first `on_rooms_updated` so `pendingRestoreRoom_` is set in time) and again whenever a sync delta delivers a changed value. C++ layer: `PrefsData` struct + `Prefs::parse` / `Prefs::serialize` helpers replace the old file-based `Prefs` class; `Client::load_prefs_json` / `save_prefs_json` wrap the FFI. All 4 shells wire `on_account_prefs_updated` (Qt6 queued signal, GTK4 `g_idle_add`, Win32 `WM_TESSERACT_ACCOUNT_PREFS = WM_APP + 16`, macOS `dispatch_async`); the load callsites at session-restore / login use `load_prefs_json()` for the fast path (returning users hit the SQLite cache before sync starts). 6 new C++ tests (`test_prefs.cpp`) + 2 Rust unit tests; 165/165 ctest pass, 67 Rust unit tests pass.

- **OAuth session flush on shutdown** — `stop_sync()` now calls `full_session()` and persists the result via `on_session_refreshed` before tearing down the SyncService. The session-watcher task (spawned in `start_sync`) saves new tokens whenever `TokensRefreshed` fires, but its `JoinHandle` is discarded so it can be cancelled mid-flight when the tokio runtime drops on shutdown. If a token rotation happened near app-close the new refresh token was lost, causing `invalid_grant` on the next startup. The flush runs while the C++ `EventHandler` is still alive (called from `on_destroy` / `stop_sync` before `~MainWindow` tears down handlers).

- **Animated stickers (GIF / APNG / animated WebP) — all four platforms** — Per-URL animated cache parallel to `tk_images_`: each entry holds a vector of pre-decoded frames + per-frame delay (ms) + current index + monotonic next-advance deadline. A 60 Hz frame tick walks the cache, advances `current` when the deadline elapses (catch-up capped at one full loop), and triggers a single repaint when any frame changed. Image-provider lambdas check the animated cache before the static one. Per-platform decode + tick: **Qt6** uses `QImageReader::supportsAnimation()` + `nextImageDelay()` driven by a `QTimer`; **GTK4** walks a `GdkPixbufAnimationIter` clocked by a synthesised `GTimeVal` (bracketed in `G_GNUC_BEGIN_IGNORE_DEPRECATIONS` — the GTK 4.44 successor API doesn't yet enumerate frames), `pixbuf_to_premultiplied_argb32` factored out for reuse; **Win32** adds `tk::d2d::decode_animation` (`IWICBitmapDecoder::GetFrameCount` ≥ 2 + per-frame `IWICMetadataQueryReader` for `/grctlext/Delay` (GIF, 1/100 s), `/fcTL/delay_num` + `/delay_den` (APNG on Win10 1809+), and `/ANMF/{FrameDuration,Duration}` (animated WebP if the Microsoft Store WebP codec is installed)), driven by a `SetTimer`-based `WM_TIMER`; macOS path still TODO. Delays clamped ≥ 20 ms to match browsers and avoid CPU-spin on 0 ms encoders. The Qt sticker picker also pauses its timer on `hideEvent` and re-bases the schedule on `showEvent` so revisits resume from where they were instead of jump-cutting.

- **Video messages (`m.video`) — receive + playback, all four platforms** — `m.video` events now render end-to-end. Rust FFI: three new fields on `TimelineEvent` (`video_thumbnail_json`, `video_duration_ms`, `video_mime`) + `MessageType::Video` arm in `timeline_item_to_ffi` (reuses `source_json`/`width`/`height`/`image_filename` slots; thumbnail MediaSource, duration ms, and MIME in the new slots); `"(video)"` fallback in the reply-body match. New C++ `VideoEvent` / `EventType::Video` in `types.h`; `"m.video"` case in `make_event()`. `MessageListView` gains `Kind::Video`: thumbnail card (max 320×200, `fit_media` aspect-correct, server thumbnail via `image_provider_`), centred play-triangle disc, bottom-right duration badge (mm:ss), `VideoHit` struct + `video_geom_` map + `video_hit_at()` + `on_video_clicked` callback (parallel to `ImageHit`/`on_image_clicked`). New `tk::VideoPlayer` pure-virtual interface (`ui/shared/tk/video.h`): `play/pause/resume/stop/seek`, `position_ms/duration_ms/is_playing`, `current_frame() → const tk::Image*`, `on_frame`/`on_progress` callbacks; `Host::make_video_player()` pure virtual added to `host.h`. Per-platform backends: **Qt6** (`video_qt.cpp`) — `QMediaPlayer` + `QVideoSink`, frames converted via `QVideoFrame::toImage()` → `tk::qt6::make_image`, 60 ms `QTimer` progress tick; **GTK4** (`video_gtk.cpp`) — GStreamer `giostreamsrc ! decodebin ! videoconvert ! video/x-raw,format=BGRA ! appsink`, `new-sample` → BGRA → `cairo_image_surface_create_for_data` → `tk::cairo_pango::make_image`, `g_idle_add` dispatch; **Win32** (`video_win32.cpp`) — `IMFSourceReader` (software decode), `ReadSample` on worker thread → BGRA → `tk::d2d::make_image_from_bgra`, `alive_` shared-ptr guard; **macOS** (`video_macos.mm`) — `AVPlayer` + `AVPlayerItemVideoOutput`, 60 Hz `NSTimer` polls `hasNewPixelBufferForItemTime:`, `CVPixelBuffer` → `CGBitmapContext` (BGRA) → `tk::cg::make_image`, temp-file workaround for `AVURLAsset`. New `VideoViewerOverlay` shared widget: full-window lightbox with dark backdrop, video frame (or thumbnail/placeholder during load), "Loading…" label, controls bar (play/pause circle, scrub bar + playhead knob, `1×`/`1.5×`/`2×` speed pill, × close button). Scrub supports immediate seek on pointer-down. Outside-click and Escape both dismiss. All four shells wired: overlay surface created and parented over the message surface; `on_video_clicked` opens the overlay and launches an async byte-fetch thread; `load_bytes` called back on the UI thread to start playback; `set_repaint_requester` wired to `surface->relayout()` so `on_frame`/`on_progress` drive repaints; `WM_TESSERACT_VIDEO_BYTES = WM_APP+19` (Win32). Client-side first-frame thumbnail generation when the server omits a thumbnail: Qt6 = `QMediaPlayer`+`QVideoSink` on `QThreadPool`, JPEG-encoded → `mediaBytesLoaded_` signal; GTK4 = GStreamer `appsink` on `std::thread` + `g_idle_add`; Win32 = `IMFSourceReader` worker + `WM_TESSERACT_VIDEO_BYTES`; macOS = `AVAssetImageGenerator` + temp file on detached thread + `dispatch_async`. Cache key `"thumb::" + event_id` stored in `tk_images_`. 10 new Catch2 tests (`test_tk_video_viewer.cpp`) — state machine, load/close, pointer interactions, `MessageListView` video hit-test and click; 185/185 ctest pass, 66/66 Rust unit tests pass.

- **System tray + minimize-to-tray — all four platforms** — New shared `tesseract::ITrayIcon` interface ([client/include/tesseract/tray_icon.h](client/include/tesseract/tray_icon.h)) mirroring `INotifier`. Each platform impl is created after login (mirrors the notifier lifecycle) with two `std::function<void()>` callbacks — `on_show` (unhide + foreground the window) and `on_quit` (real exit). When `is_available()` returns true the shell intercepts the window-close event and hides the window; when false the close falls through to the existing quit path. **Qt6** — `QSystemTrayIcon` + `QMenu`; `MainWindow::closeEvent` hides on close; `qApp->setQuitOnLastWindowClosed(false)` once the tray is up. **GTK4** — `libayatana-appindicator3` (probed at construction via `org.kde.StatusNotifierWatcher.IsStatusNotifierHostRegistered` to gate creation on hosts without an SNI); built as a separate static library `tesseract_gtk_tray` to keep its GTK3 include paths out of the GTK4 translation units; type-erased header (`void*` indirection) so the rest of the shell can include the tray header without dragging in GTK3 types. `close-request` signal handler hides the window when the tray is up; `g_application_hold` keeps the GApplication alive while the window is hidden. **Win32** — `Shell_NotifyIcon(NIM_ADD)` against a hidden helper HWND (`HWND_MESSAGE` class), `WM_TESSERACT_TRAY = WM_APP + 20` callback message, `TrackPopupMenuEx` for the right-click menu; `MainWindow`'s wnd_proc gains a `WM_CLOSE` case that `ShowWindow(SW_HIDE)`s when the tray is available and not `quitting_`; the Quit menu item sets `quitting_ = true` then `DestroyWindow` so `WM_DESTROY → PostQuitMessage` runs once. Reuses the existing `IDI_TESSERACT` icon. **macOS** — `NSStatusItem` with a template `NSImage` (18×18) so the menu bar tints it for light/dark mode; `NSMenu` with Show / Quit; `MainWindowController` sets itself as `NSWindowDelegate` and implements `windowShouldClose:` which `[window orderOut:nil]`s and returns `NO` when the tray is up. Quit invokes `[NSApp terminate:nil]` so the existing `applicationWillTerminate:` → `stop_sync` chain still runs. New files: `LinuxQtTrayIcon.{h,cpp}`, `LinuxGtkTrayIcon.{h,cpp}`, `Win32TrayIcon.{h,cpp}`, `MacOSTrayIcon.{h,mm}`. Linux pkg-config gains `ayatana-appindicator3-0.1`. No new shared-toolkit tests — tray code is shell-only integration; 189/189 ctest pass under Qt6 + GTK4.

**Step 5 — UI redesign**

Goal: a visually polished, modern chat layout that forms the shell for threads, stickers, and emoji in later steps.

- **Thread support** — reply-to indicator ✓: `in_reply_to_*` FFI fields, quote block in `MessageListView`, hover "↩ Reply" button, `ComposeBar` reply-preview banner (`kReplyBandH = 44 px`), `send_reply` FFI, reply relation threaded through image/file sends (`AttachmentConfig::reply`), click on quote scrolls to original (in-list) or fires `on_scroll_to_original`, all 4 shells wired; **message editing ✓**: `send_edit` FFI, `is_edited` field + `(edited)` badge, hover "✏" button, `ComposeBar` edit-mode banner (`kEditBandH = 44 px`) + `on_send_edit`, mutually exclusive with reply mode, all 4 shells wired; 19 new C++ tests (165 total as of latest), 4 new Rust unit tests (67 total as of latest). Threaded reply panel (slide-in sidebar) is still deferred.
- **Emoji reactions** — reaction bar below each message showing emoji + count; tap to toggle; new FFI `send_reaction(room_id, event_id, key)` / `redact_reaction`; reactions carried in `TimelineEvent` as `Vec<(emoji, count, reacted_by_me)>`.
- **Emoji picker** — bottom-bar button opens a floating picker (tabbed: frequently-used + Unicode categories). Inserts into the compose field. Separate from sticker packs (MSC2545, Step 9).
- **Inline image / file rendering** ✓ — `m.image` renders as a thumbnail (max 320×200) in Qt6 and GTK4; MSC2530 caption rule applied (`body` shown only when sender supplies a distinct `filename` field). `m.sticker` pulled forward from Step 8: borderless 256×256 inline thumbnail, no caption. Files still render as a card with icon, name, and size.
- **Voice messages (MSC3245) — receive + playback** ✓ — `m.audio` events with the `org.matrix.msc3245.voice` marker render as a 280×48 voice card with play/pause, scrubbable waveform, `1×`/`1.5×`/`2×` speed pill, and mm:ss remaining-time label. Per-platform `tk::AudioPlayer` backend (QMediaPlayer / GStreamer / AVAudioPlayer; Win32 stubbed). Background prefetch warms the SDK media cache on row-first-paint so play taps are instant. Plain (non-voice) `m.audio` folds through the file-card path. Recording / sending is deferred to a later step.
- **Compose bar polish** ✓ — Shared `tesseract::views::ComposeBar` mounted on every platform via `tk::*::Surface`. Multi-line expanding input through `tk::NativeTextArea` (auto-grows 56→160 px clamped); send-on-Enter, Shift+Enter inserts a newline; emoji + send buttons paint in the toolkit; send button gates on trimmed non-empty content. Still TODO: `/` command hints, typing indicator sent to the room, placeholder painting for GTK4 / macOS `NativeTextArea` (`GtkTextView` and `NSTextView` lack built-in placeholders — see Known gaps).
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

**Step 8 — MSC2545 phase A: receive (encrypted-aware)** ✓ partly done — see status block above for the full delivery. Remaining for Step 8 phase A:

- **Inline emoticons in HTML message bodies** — when a peer sends an `m.text` whose `formatted_body` contains `<img data-mx-emoticon ...>` (or the `mx-emoticon` shortcode form), render the inline image instead of the alt text. Per-platform paths: Qt `QTextDocument::addResource`, GTK `GtkTextChildAnchor`, macOS `NSTextAttachment`, Win32 via the RichEdit overlay (see Step 8b).
- ~~**macOS shell wiring**~~ ✓ **done** — `StickerPickerPanel` (NSPanel singleton, mirrors `EmojiPickerPanel`) wired through `on_sticker`; animated stickers decode via `CGImageSource` driven by a 16 ms block-based `NSTimer`; async sticker fetch via detached `std::thread` + `dispatch_async`; right-click "Add to Saved Stickers" via `NSMenu` on `rightMouseDown:` hit-tested by `sticker_hit_at`; `on_image_packs_updated` forwarded through `EventBridge` → `handleImagePacksUpdated` → `refreshPacks`.
- **Win32 shell wiring** for `StickerPicker` (child `WS_POPUP` surface), right-click menu (`TrackPopupMenu` on `WM_RBUTTONUP`), and the underlying RichEdit overlay (Step 8b).
- **Async image cache for the pickers** — both `StickerPicker` and `EmojiPicker`'s custom tabs currently reuse the host's existing `tk_images_` map. That's pre-populated by message-list inline media, so the picker's first paint of a never-seen sticker shows a placeholder. Promote `ensure_media_image` into a shared `tk::AsyncImageCache` (host-side; `Client::fetch_source_bytes` on a worker → decode → post-to-UI → cache + repaint) and feed it through `set_image_provider`. Per-pack tab avatars and Favorites land on the same path.

**Step 8b — Win32 RichEdit inline media overlay (new)**

- New `tk::InlineMediaSurface` abstraction: per-message-row optional native overlay, no-op default (canvas paints as today). Win32 implementation backs it with a per-row `RichEdit 4.1` (`MSFTEDIT.DLL`) HWND pooled by `event_id`.
- `IRichEditOleCallback` + `OleCreatePictureIndirect` over a WIC-decoded `HBITMAP` (same WIC pipeline `canvas_d2d.cpp` already uses) to insert each image / sticker as an OLE object.
- `MessageListView` publishes inline-media rects via a new `set_on_inline_media_layout` callback; the overlay subsystem `SetWindowPos`es children in the same `set_on_layout` pass that the existing `NativeTextArea` overlay uses. `Surface::supports_inline_media_overlay()` virtual gates the skip-canvas-paint path; returns false on Qt6 / GTK4 / macOS so they continue painting through `tk::Canvas`.
- LRU pool keyed by `event_id`, hard cap ~32 active children, `SetWindowPos(SWP_HIDEWINDOW)` for offscreen rows. Fail-safe fallback to canvas paint when `MSFTEDIT.DLL` is absent or `IOleObject` insertion fails.
- Lays the groundwork for inline-emoticon-in-HTML-body rendering on Win32 (an inline emoticon is just another OLE picture object spliced into the row's RichEdit text).

**Step 9 — MSC2545 phase B: send (rich emoticons + autocomplete)** ✓ partly done

- `send_sticker(room_id, body, image_url, info_json)` ✓ landed (matrix-sdk handles E2EE rooms transparently; outgoing stickers always reference plain `mxc://`).
- `:shortcode:` insertion from the EmojiPicker's custom tabs ✓ landed (clicking a custom emoticon currently inserts `:shortcode:` plain-text via the new `on_emoticon_selected` callback — hosts wire it as they wish).
- Remaining:
  - **`send_emoticon_message(room_id, plain_body, html_body)` FFI** + composer integration to emit the `<img data-mx-emoticon ...>` HTML body when a custom emoticon is picked (rather than the `:shortcode:` plain-text fallback).
  - **`:shortcode:` autocomplete in the compose input** — popup suggestion list as the user types `:abc`, sourced from the same image-pack cache the picker uses.

**Step 10 — MSC2545 phase C: pack management**

- `save_sticker_to_user_pack` + `toggle_favorite_sticker` + `user_pack_has_sticker` ✓ landed (FFI + Qt6 + GTK4 right-click "Add to Saved Stickers" wired).
- Remaining: list enabled packs UI, toggle subscription via `im.ponies.emote_rooms` (room settings drill-in), pack creation / removal flow, sticker delete/rename inside the user pack, manual order/sort.

**Step 11 — Notifications, layer 1: foreground toasts** ✓ done (Win32 + Qt6 + GTK4)

- `tesseract::INotifier` abstraction (`client/include/tesseract/notifier.h`) + `Notification` struct; per-platform impls added as shells are wired.
- Push-rule evaluation: `evaluate_push_rules` in `sdk/src/client.rs` builds a synthetic raw event envelope from live `TimelineEvent` fields, fetches `m.push_rules` from the local account-data cache, constructs `PushConditionRoomCtx` (member count, user ID, display name), and calls `Ruleset::get_actions().await`. Fires only on `VectorDiff::PushBack` (live tail events); pagination is skipped. `is_mention` set from `Action::is_highlight()`.
- New `IEventHandler::on_notification` (optional virtual, default no-op); dispatched from `EventHandlerBridge` via the existing Rust→C++ bridge pattern. Qt6 and GTK4 now also override it.
- **Win32**: `Win32Notifier : INotifier` uses WinRT `Windows.UI.Notifications.ToastNotificationManager` (works Win 8.1+, not Win11-only); adaptive `ToastGeneric` XML with sender, optional room name (omitted for DMs), and 120-char body preview; UTF-8 → wide via `MultiByteToWideChar`; XML-escaped text; `SetCurrentProcessExplicitAppUserModelID(L"io.gnomos.Tesseract")` + `winrt::init_apartment` in `main.cpp`. `WM_TESSERACT_NOTIFY = WM_APP+17` / `WM_TESSERACT_NOTIFY_CLICK = WM_APP+18`; suppressed when the window is focused and the source room is already active; click restores + foregrounds the window and navigates to the room via `navigate_to_room`. CMake auto-detects the `cppwinrt` SDK include dir and links `windowsapp.lib`.
- **Qt6**: `LinuxNotifierQt : QObject, INotifier` uses `QDBusInterface` against `org.freedesktop.Notifications` (session bus); `ActionInvoked` / `NotificationClosed` via `QDBusConnection::sessionBus().connect`; replace-per-room via `id_to_room_` / `room_to_id_` maps; Flatpak portal path (`FLATPAK_ID` set) calls `org.freedesktop.portal.Notification.AddNotification` with the room_id as the stable string ID. `EventBridge::on_notification` emits `notificationTriggered`; queued connection to `MainWindow::onNotificationTriggered`; suppressed when window is active + source room open; click calls `navigate_to_room` → `raise()`/`activateWindow()`. `Qt6::DBus` added to CMakeLists.
- **GTK4**: `LinuxNotifierGtk : INotifier` uses `GDBusConnection` (`g_bus_get_sync(SESSION)`); `ActionInvoked` / `NotificationClosed` via `g_dbus_connection_signal_subscribe`; same replace-per-room and portal (`g_getenv("FLATPAK_ID")`) patterns as Qt6. `EventHandler::on_notification` uses the `cpp_window_for(window_)` pattern to marshal via `g_idle_add` to `MainWindow::push_notification` → `handle_notification`; suppressed when `gtk_window_is_active` + source room open; click calls `navigate_to_room` → `gtk_window_present`. Explicit `gio-2.0` added to CMakeLists. 185/185 ctest pass (no new tests — notifications are integration-level only).
- **macOS (NSUserNotificationCenter / UNUserNotificationCenter)** impl deferred — `INotifier` abstraction is ready.

**Step 12 — Notifications, layer 2: server pushers**
- `register_pusher` / `remove_pusher` FFI wrapping `client.pusher().set(...)`.
- Linux: UnifiedPush via D-Bus `org.unifiedpush.Connector1`.
- Windows: deferred (WNS needs Store registration; UnifiedPush distributors on Windows are an option).
- macOS: deferred (APNs).

### Known gaps from the shared-toolkit migration

- ~~**macOS build is unverified end-to-end**~~ ✓ verified and fixed — built and tested on macOS 15 (Intel, 10.15 CommandLineTools SDK); SDK compat guard added to `host_macos.mm`; session restore, login flash, user strip, and space back-button all confirmed working.
- **Win32 MinGW build is unverified** — `canvas_d2d.cpp` compiles under MSVC; MinGW depends on whether its DirectWrite/user32 headers expose `DWRITE_FONT_WEIGHT_SEMI_BOLD` and `GetDpiForWindow` (the latter needs `_WIN32_WINNT=0x0A00`, which is now the floor). Cross-compiling from Linux still needs a verification pass.
- **`NativeTextArea` placeholder is a no-op on GTK4 + macOS** — `GtkTextView` and `NSTextView` ship without built-in placeholders. The shared `ComposeBar` paints the input outline but the empty-state hint is missing. Fix: paint a `current_text().empty()`-gated label in the shared widget instead of pushing it through `NativeTextArea`.
- **`set_password` is a no-op on macOS** — toggling password mode on `NSTextField` requires swapping for `NSSecureTextField`. The recovery-key field on macOS still shows the secret in plaintext.
- **Win32 `NativeTextArea::natural_height()` undercounts wrapped lines** — heuristic is `EM_GETLINECOUNT × tmHeight`, which ignores the EDIT control's soft-wrap. Auto-grow lags by one keystroke when wrapping in.
- **`TestSurface` doesn't cover CoreGraphics** — Catch2 backend tests now exercise QPainter, Cairo, and D2D (via `CreateWicBitmapRenderTarget`); the macOS CGBitmapContext surface is still TODO and `tests/CMakeLists.txt` flags it.
- **Sticker picker shows placeholders for unfetched bitmaps (Qt6 / GTK4 only)** — `StickerPicker` and `EmojiPicker`'s custom-pack tabs reuse the host's `tk_images_` map, which on Qt6 / GTK4 is populated on demand by message-list inline media (`ensureMediaImage`). Stickers the user hasn't seen in a timeline yet paint as a grey placeholder until the host pre-fetches them. Win32 already runs its own per-picker async fetch via `request_sticker_image`; Qt6's `StickerPicker` does the same via `QThreadPool` + `imageLoadedSignal_`. Remaining: GTK4 still needs the worker-fetch hookup; the `tk::AsyncImageCache` consolidation also still pending.
- ~~**macOS still lacks StickerPicker wiring**~~ ✓ done — see Step 8 macOS shell wiring note above.
- **Win32 sticker right-click context menu still missing** — Qt6 + GTK4 offer "Add to Saved Stickers" via right-click on a sticker row, gated by `user_pack_has_sticker`. Win32 currently has no equivalent — `WM_RBUTTONUP` on the message surface should hit-test via `MessageListView::sticker_hit_at` and pop a `TrackPopupMenu` calling `save_sticker_to_user_pack`. Tracked under Step 8 "Win32 shell wiring".
- ~~**Animated stickers on macOS still TODO**~~ ✓ done — `CGImageSource`-based multi-frame decode (GIF + APNG; animated WebP probed at runtime on macOS 11+ via fallback string constants), 16 ms `NSTimer` tick.
- **`save_sticker_to_user_pack` posts an empty `info` object** — the Qt6 / GTK4 right-click handlers currently call `save_sticker_to_user_pack(body, body, mxc_url, "{}")` because the inline-sticker hit doesn't carry the original `info` JSON (width/height/mimetype). The pack entry round-trips correctly, but fallback clients won't see dimensions. Fix: thread `info_json` (or w/h/mimetype) through `MessageListView::StickerHit`.

### Decisions still open

- ~~**Win32 inline images** — RichEdit 4.1 (with `IRichEditOleCallback`) for inline images/stickers, or text-only emoticons + attachment-card on Win32 in v1?~~ **Decided 2026-05-12**: RichEdit 4.1 for all inline images + stickers + (future) HTML emoticons. See Step 8b for the overlay design; implementation pending.
- ~~**Cross-pack scope in v1** — pinned packs vs all subscribed packs in the sticker/emoji picker?~~ **Decided 2026-05-12**: all subscribed packs (from `im.ponies.emote_rooms`) plus the user pack. No per-pack pinning in v1; the picker's Favorites tab handles individual-sticker pinning instead.
- **Timeline persistence** — opt in to sqlite-backed `Timeline::with_focus(...)`, or memory-only?
- **Room-list window** — `AllRooms` for desktop (recommended), or windowed?
- **Pack-entry encrypted badging** — show a lock glyph on encrypted packs in the picker?
- **Thread panel layout** — slide-in panel (Telegram style) vs inline thread expansion (Discord style) vs separate window?
