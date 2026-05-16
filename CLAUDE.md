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

**Available presets** (in `CMakePresets.json`): `windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-gtk-release`, `linux-qt6-debug`, `linux-qt6-release`, `macos-appkit-x86_64-debug`, `macos-appkit-x86_64-release`.

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
                  RecoveryBanner, VerificationBanner, ComposeBar,
                  markdown (Markdown→HTML), html_spans (HTML→TextSpan)
  windows/   ← Win32 executable (thin shell)
  macos/     ← AppKit executable (.app bundle, thin shell)
  linux-qt/  ← Qt6 Widgets executable (thin shell)
  linux-gtk/ ← GTK4 executable (thin shell)
```

### Layer responsibilities

**`sdk/` (Rust)** — All async I/O lives here. A `tokio` runtime runs in background threads. The `cxx` bridge (`sdk/src/lib.rs`) exposes a synchronous C API. `client.rs` wraps `matrix-sdk` for login, sync, and messaging; `oauth.rs` implements the RFC 8252 loopback redirect OAuth flow.

**`client/` (C++)** — `tesseract::Client` (Pimpl) wraps the Rust FFI. `tesseract::IEventHandler` is the interface UIs implement to receive async callbacks (room updates, sync events, session saves). `tesseract::SessionStore` handles platform-specific persistence of the session JSON (`%APPDATA%/Tesseract/` on Windows, `~/.config/tesseract/` on Linux).

**`ui/shared/` (C++)** — `tesseract_tk` is the cross-platform UI toolkit. It owns drawing, layout, hit-test, focus, and keyboard. `tk::Canvas` is the abstract 2D backend (D2D on Win32, QPainter on Qt6, Cairo+Pango on GTK4, CoreGraphics+CoreText on macOS). `tk::Host` is the per-platform integration surface (repaint scheduling, post-to-UI, native edit overlays). Shared widget classes live under `tk/`; shared views (LoginView, RoomListView, MessageListView, EmojiPicker, RecoveryBanner, ComposeBar) live under `views/`. Text input stays native via `tk::NativeTextField` / `tk::NativeTextArea` overlays so IME and selection behave correctly per-OS.

**`ui/shared/app/` (C++)** — `ShellBase` holds all platform-agnostic shell state (accounts, rooms, image caches, worker threads, sync state) as `protected` members with pure-virtual hooks (`post_to_ui_`, `on_rooms_updated_`, `on_media_bytes_ready_`, etc.) and a set of virtual no-ops that each shell overrides (`handle_timeline_reset_ui_`, `on_room_list_state_ui_`, …). `EventHandlerBase : IEventHandler` marshals every SDK callback to the UI thread via `shell->post_to_ui_()` then calls the corresponding `handle_*_ui_()` virtual. Qt6 / GTK4 / Win32 shells inherit `ShellBase` directly; the macOS shell uses composition (`MainWindowController` holds `std::unique_ptr<MacShell>` where `MacShell : public ShellBase`) and exposes protected members to ObjC++ code via C++ `using` declarations in a `public:` section of `MacShell`.

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
| `ui/shared/app/ShellBase.h` | Platform-agnostic shell state + pure-virtual hooks shared by all four shells |
| `ui/shared/app/EventHandlerBase.h` | `IEventHandler` adapter: marshals SDK callbacks → UI thread → `handle_*_ui_()` virtuals |
| `ui/linux-qt/src/MainWindow.h` | Qt6 shell with `EventBridge` for thread marshaling |
| `ui/macos/src/MainWindowController.mm` | AppKit shell (`NSWindowController` + `NSSplitView`); uses `MacShell : ShellBase` composition |

## Roadmap

Completed work is in [CHANGES.md](CHANGES.md). What follows is only the pending and in-progress work.

### Step 5 — UI redesign (in progress)

Done: inline images, stickers, reply-to, message editing, voice messages, ComposeBar, read receipts (display + sending, overlay only — never expand rows), hover timestamps, day separators, typing indicators, inline bold/italic/code/strikethrough via `formatted_body`, URL previews + hyperlinks (Qt6, GTK4, Win32), Markdown-to-HTML for sent messages, last-message preview in sidebar (regular-weight room name, 1px inter-room separator, compact row sizing). Remaining:

- **Emoji reactions** — reaction bar below each message (emoji + count); tap to toggle; `send_reaction` / `redact_reaction` FFI; reactions in `TimelineEvent` as `Vec<(emoji, count, reacted_by_me)>`.
- **Message bubbles / cards** — visual polish pass on the message layout.
- **Threaded reply panel** — slide-in sidebar (deferred from reply-to landing).
- **Sidebar polish** — DM rooms show the other user's avatar.
- **ComposeBar gaps** — `/` command hints; `NativeTextArea` placeholder on GTK4 + macOS (see Known gaps).

### Step 8 — MSC2545 phase A: remaining items

- **Inline emoticons in HTML message bodies** — render `<img data-mx-emoticon ...>` in `formatted_body` instead of alt text. Per-platform: Qt `QTextDocument::addResource`, GTK `GtkTextChildAnchor`, macOS `NSTextAttachment`, Win32 via RichEdit overlay (Step 8b).
- **Win32 shell wiring** — `StickerPicker` child `WS_POPUP` surface and the underlying RichEdit overlay (Step 8b). Right-click "Add to Saved Stickers" is done (`tk::win32::Surface::set_on_right_click` + `WM_RBUTTONUP`).
- **Async image cache for pickers** — promote `ensure_media_image` into a shared `tk::AsyncImageCache` (worker → decode → post-to-UI → cache + repaint) so `StickerPicker` / `EmojiPicker` custom tabs don't show grey placeholders for stickers not yet seen in a timeline. GTK4 worker-fetch hookup still pending; consolidation with Win32/Qt6 paths also pending.

### Step 8b — Win32 RichEdit inline media overlay

- New `tk::InlineMediaSurface` abstraction: per-row optional native overlay (no-op default). Win32 impl: per-row `RichEdit 4.1` (`MSFTEDIT.DLL`) HWND pooled by `event_id`.
- `IRichEditOleCallback` + `OleCreatePictureIndirect` over WIC-decoded `HBITMAP` to insert images/stickers as OLE objects.
- `MessageListView` publishes inline-media rects via `set_on_inline_media_layout`; overlay `SetWindowPos`es children in the same layout pass as `NativeTextArea`. `Surface::supports_inline_media_overlay()` gates the skip-canvas-paint path (returns false on Qt6 / GTK4 / macOS).
- LRU pool keyed by `event_id`, hard cap ~32 active children, `SWP_HIDEWINDOW` for offscreen rows. Fail-safe fallback to canvas paint when `MSFTEDIT.DLL` is absent.

### Step 9 — MSC2545 phase B: send (remaining)

- **`send_emoticon_message(room_id, plain_body, html_body)` FFI** + composer integration to emit `<img data-mx-emoticon ...>` HTML body when a custom emoticon is picked (instead of `:shortcode:` plain-text fallback).
- **`:shortcode:` autocomplete** — popup suggestion list as the user types `:abc`, sourced from the image-pack cache.

### Step 10 — MSC2545 phase C: pack management (remaining)

- List enabled packs UI; toggle subscription via `im.ponies.emote_rooms` (room settings drill-in).
- Pack creation / removal flow; sticker delete/rename inside the user pack; manual order/sort.

### Step 12 — Notifications, layer 2: server pushers

Linux (Qt6 + GTK4) done — see CHANGES.md. Remaining:

- Windows: deferred (WNS needs Store registration; UnifiedPush distributors on Windows are an option).
- macOS: deferred (APNs).

### Known gaps

- **`NativeTextArea` placeholder is a no-op on GTK4 + macOS** — `GtkTextView` and `NSTextView` lack built-in placeholders. Fix: paint a `current_text().empty()`-gated label in the shared widget.
- **`set_password` is a no-op on macOS** — toggling password mode on `NSTextField` requires swapping for `NSSecureTextField`; recovery-key field still shows plaintext.
- **Win32 `NativeTextArea::natural_height()` undercounts wrapped lines** — `EM_GETLINECOUNT × tmHeight` ignores soft-wrap; auto-grow lags by one keystroke when wrapping.
- **`TestSurface` doesn't cover CoreGraphics** — QPainter, Cairo, and D2D are tested; macOS CGBitmapContext surface is still TODO.
- **Sticker picker placeholders on GTK4** — GTK4 still needs the worker-fetch hookup for `StickerPicker` / `EmojiPicker` custom-pack tabs; `tk::AsyncImageCache` consolidation also pending.
- **`tk_avatars_` / `tk_images_` not keyed by `(user_id, mxc)`** — cosmetic ghosting risk when two accounts share an mxc URL that resolves to different bytes.
- **URL preview + hyperlink rendering on macOS** — `get_url_preview` FFI and `MessageListView` preview card are wired on Qt6, GTK4, and Win32; macOS `MainWindowController` still needs `on_url_preview_ready_` and `on_link_clicked` wiring.
- **i18n not wired on macOS (`NSLocalizedString`) or Win32 (`LoadString`)**.

### Decisions still open

- **Timeline persistence** — opt in to sqlite-backed `Timeline::with_focus(...)`, or memory-only?
- **Room-list window** — `AllRooms` for desktop (recommended), or windowed?
- **Pack-entry encrypted badging** — show a lock glyph on encrypted packs in the picker?
- **Thread panel layout** — slide-in panel (Telegram style) vs inline thread expansion (Discord style) vs separate window?
