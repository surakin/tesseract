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
#  pulls GTK3 into the process.)
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

**Available presets** (in `CMakePresets.json`): `windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-gtk-release`, `linux-qt6-debug`, `linux-qt6-release`, `macos-appkit-arm64-debug`, `macos-appkit-arm64-release`, `macos-appkit-x86_64-debug`, `macos-appkit-x86_64-release`.

**Override UI selection:** `-DTESSERACT_UI=gtk|qt6|win32|macos` (otherwise auto-detected from platform).

Corrosion (CMake↔Cargo bridge) is fetched automatically via `FetchContent` — no global install needed. SQLite is compiled in-tree via matrix-sdk's `bundled-sqlite` feature; TLS uses rustls (no system OpenSSL).

## Architecture

```text
sdk/         ← Rust crate: matrix-sdk wrapper + cxx FFI bridge
client/      ← C++ static library: high-level C++ API over the Rust FFI
ui/
  shared/    ← tesseract_tk: cross-platform widget toolkit + shared views
    tk/        ← Canvas / Widget / Layout / Host abstractions + per-backend impls
    views/     ← MainAppWidget (root widget tree); LoginView, BrandView;
                  RoomListView, RoomView, MessageListView, ComposeBar;
                  EmojiPicker, StickerPicker, AccountPicker;
                  ImageViewerOverlay, VideoViewerOverlay, ShortcodePopup;
                  SettingsView, JoinRoomView, UserInfo;
                  RecoveryBanner, VerificationBanner;
                  markdown (Markdown→HTML), html_spans (HTML→TextSpan),
                  map_tiles, media_utils
  windows/   ← Win32 executable (thin shell)
  macos/     ← AppKit executable (.app bundle, thin shell)
  linux-qt/  ← Qt6 Widgets executable (thin shell)
  linux-gtk/ ← GTK4 executable (thin shell)
```

### Layer responsibilities

**`sdk/` (Rust)** — All async I/O lives here. A `tokio` runtime runs in background threads. The `cxx` bridge (`sdk/src/lib.rs`) exposes a synchronous C API. `client.rs` wraps `matrix-sdk` for login, sync, and messaging; `oauth.rs` implements the RFC 8252 loopback redirect OAuth flow.

**`client/` (C++)** — `tesseract::Client` (Pimpl) wraps the Rust FFI. `tesseract::IEventHandler` is the interface UIs implement to receive async callbacks (room updates, sync events, session saves). `tesseract::SessionStore` handles platform-specific persistence of the session JSON (`%APPDATA%/Tesseract/` on Windows, `~/.config/tesseract/` on Linux).

**`ui/shared/` (C++)** — `tesseract_tk` is the cross-platform UI toolkit. It owns drawing, layout, hit-test, focus, and keyboard. `tk::Canvas` is the abstract 2D backend (D2D on Win32, QPainter on Qt6, Cairo+Pango on GTK4, CoreGraphics+CoreText on macOS). `tk::Host` is the per-platform integration surface (repaint scheduling, post-to-UI, native edit overlays). Shared widget classes live under `tk/`; shared views live under `views/` (see architecture diagram above for the full list). Text input stays native via `tk::NativeTextField` / `tk::NativeTextArea` overlays so IME and selection behave correctly per-OS.

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
| `ui/shared/app/RoomWindowBase.h` | Base for secondary (popout) room windows; each platform shell provides a concrete subclass |
| `ui/linux-qt/src/MainWindow.h` | Qt6 shell; `EventBridge` wraps `EventHandlerBase` as a `QObject` for thread marshaling |
| `ui/linux-gtk/src/MainWindow.h` | GTK4 shell; uses `g_idle_add` for UI-thread dispatch |
| `ui/windows/src/MainWindow.h` | Win32 shell; uses `PostMessage` for UI-thread dispatch |
| `ui/macos/src/MainWindowController.mm` | AppKit shell (`NSWindowController` + `NSSplitView`); uses `MacShell : ShellBase` composition |

## Roadmap

See [ROADMAP.md](ROADMAP.md) for pending work, known gaps, and open design decisions. Completed work is in [CHANGES.md](CHANGES.md).

## Code Style

See [STYLE.md](STYLE.md) for formatting and naming conventions that apply across all C++ and Rust code in this repo.
