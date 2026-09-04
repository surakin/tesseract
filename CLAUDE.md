## What This Is

Tesseract is a cross-platform desktop Matrix/chat client. The core networking is Rust (using `matrix-sdk`), exposed to C++ via a `cxx` FFI bridge. Platform-specific UIs are written in C++ targeting Win32 (Windows), AppKit (Objective-C++, macOS), Qt6 Widgets, or GTK4 (Linux).

## Build Commands

Per-platform prerequisites, the full preset list, the `-DTESSERACT_UI=` override, and build internals (Corrosion, bundled SQLite, rustls, link strategy) live in [docs/BUILD.md](docs/BUILD.md). The day-to-day loop:

```bash
cmake --preset linux-debug              # full preset list in docs/BUILD.md; builds GTK4 + Qt6
cmake --build build/linux-debug
./build/linux-debug/ui/linux-qt/tesseract    # Qt6 UI
./build/linux-debug/ui/linux-gtk/tesseract   # GTK4 UI

# macOS (pick the preset matching your CPU):
cmake --preset macos-appkit-arm64-debug
cmake --build build/macos-appkit-arm64-debug
open build/macos-appkit-arm64-debug/ui/macos/Tesseract.app
```

## Architecture

```text
sdk/         ← Rust crate: matrix-sdk wrapper + cxx FFI bridge
client/      ← C++ static library: high-level C++ API over the Rust FFI
ui/
  shared/    ← tesseract_tk: cross-platform widget toolkit + shared views
    tk/        ← Canvas / Widget / Layout / Host abstractions (+ genuinely shared
                  GStreamer video/capture); per-platform backend impls
                  (canvas_*/host_*/audio_*/video_*/screen_capture_*) live in each
                  ui/<platform>/tk/, compiled into that platform's own target
    views/     ← MainAppWidget (root widget tree); LoginView, BrandView;
                  RoomListView, RoomView, MessageListView, ComposeBar;
                  ThreadView, ThreadListView (right-side panel inside RoomView);
                  EmojiPicker, StickerPicker, AccountPicker;
                  ImageViewerOverlay, VideoViewerOverlay, ShortcodePopup;
                  SettingsView, JoinRoomView, UserInfo;
                  RecoveryBanner, VerificationBanner;
                  html_spans (HTML→TextSpan / →BodyBlocks), table_layout
                  (Markdown-table grid fitting), map_tiles, media_utils
                  (Markdown→HTML lives in client/src/markdown.cpp, Rust-backed)
                  shared bases: ListPopupBase, MediaOverlayBase, TabbedGridPicker;
                  MessageListView collaborators (per-concern, behavior-preserving
                  split): TimelineMediaController, TimelineVideoPlaylist,
                  LocationMapPanner, RoomSwitchGateKeeper, UrlPreviewCardDisplay,
                  LinkLayoutCache, SpoilerRevealer, ReadReceiptTracker
  windows/   ← Win32 executable (thin shell) + its tk/ backend (+ third_party/bettertext text control)
  macos/     ← AppKit executable (.app bundle, thin shell) + its tk/ backend
  linux-qt/  ← Qt6 Widgets executable (thin shell) + its tk/ backend
  linux-gtk/ ← GTK4 executable (thin shell) + its tk/ backend
```

### Layer responsibilities

**`sdk/` (Rust)** — All async I/O lives here. A `tokio` runtime runs in background threads. The `cxx` bridge (`sdk/src/lib.rs`) exposes a synchronous C API. `client.rs` wraps `matrix-sdk` for login, sync, and messaging; `oauth.rs` drives the RFC 8252 loopback-redirect OAuth flow on top of matrix-sdk's `local-server` feature (its own axum-based redirect listener — Tesseract runs no HTTP server of its own).

**`client/` (C++)** — `tesseract::Client` (Pimpl) wraps the Rust FFI. `tesseract::IEventHandler` is the interface UIs implement to receive async callbacks (room updates, sync events, session saves). `tesseract::SessionStore` handles platform-specific persistence of the session JSON and the per-account matrix-sdk store. Account data lives under `data_dir()` (`%APPDATA%/Tesseract/` on Windows, `~/Library/Application Support/Tesseract/` on macOS, `~/.local/share/tesseract/` on Linux — XDG state, not config); only `app_settings.json` lives in `config_dir()` (`~/.config/tesseract/` on Linux). `data_dir()` equals `config_dir()` on Windows/macOS, which have no such split.

**`ui/shared/` (C++)** — `tesseract_tk` is the cross-platform UI toolkit. It owns drawing, layout, hit-test, focus, and keyboard. `tk::Canvas` is the abstract 2D backend (D2D on Win32, QPainter on Qt6, Cairo+Pango on GTK4, CoreGraphics+CoreText on macOS). `tk::Host` is the per-platform integration surface (repaint scheduling, post-to-UI, native edit overlays). Shared widget classes live under `tk/`; shared views live under `views/` (see architecture diagram above for the full list). Text input stays backed by real native controls for IME/selection correctness per-OS, but `tk::NativeTextField` / `tk::NativeTextArea` render into the canvas rather than floating on top of it: each control's native surface is captured via `rendered_image()` and composited with `Canvas::draw_image()`, so it participates in clipping, opacity, and paint order like any other canvas content.

**`ui/shared/app/` (C++)** — `ShellBase` holds all platform-agnostic shell state (accounts, rooms, image caches, worker threads, sync state) as `protected` members with pure-virtual hooks (`post_to_ui_`, `on_rooms_updated_`, `on_media_bytes_ready_`, etc.) and a set of virtual no-ops that each shell overrides (`handle_timeline_reset_ui_`, `on_room_list_state_ui_`, …). `EventHandlerBase : IEventHandler` marshals every SDK callback to the UI thread via `shell->post_to_ui_()` then calls the corresponding `handle_*_ui_()` virtual. Qt6 / GTK4 / Win32 shells inherit `ShellBase` directly; the macOS shell uses composition (`MainWindowController` holds `std::unique_ptr<MacShell>` where `MacShell : public ShellBase`) and exposes protected members to ObjC++ code via C++ `using` declarations in a `public:` section of `MacShell`. Cohesive subsystems are being split out of `ShellBase` into collaborators (`AccountManager`, `ThreadPanelController`; the media-fetch `MediaController` is the next planned cut — see `docs/TODO-phase5-remaining.md`); when extracting, keep shell-read fields (e.g. `thread_panel_`, `current_thread_root_`) on `ShellBase` so the four shells compile untouched.

**`ui/*/` (C++)** — Each platform target is a thin native shell that owns the window/menu/AX surface and mounts one or more `tk::*::Surface`s hosting shared views. Each shell implements `IEventHandler`. Because Rust callbacks arrive on worker threads, the shells route through `tk::Host::post_to_ui`, which is backed by `QueuedConnection` (Qt6), `g_idle_add` (GTK4), `PostMessage` (Win32), and `dispatch_async(dispatch_get_main_queue())` (macOS).

### Key API surface (`client/include/tesseract/`)

- `Client::begin_oauth()` / `Client::await_oauth()` — two-phase OAuth: first call returns the auth URL (open in browser), second blocks until the loopback redirect completes.
- `Client::start_sync()` / `Client::stop_sync()` — background sync loop.
- `Client::send_message()`, `Client::list_rooms()`, `Client::room_messages()` — core messaging.
- `IEventHandler::on_session_saved(json)` — called on token refresh; persist the JSON to resume sessions without re-login.

## Running Tests

### Rust unit tests (standalone — no C++ required)

```bash
cargo test -p tesseract-sdk-ffi
```

### C++ tests via CMake / ctest

```bash
cmake --preset linux-debug
cmake --build build/linux-debug
ctest --test-dir build/linux-debug --output-on-failure
```

Each Catch2 `TEST_CASE` is registered as a separate ctest test. See [docs/BUILD.md](docs/BUILD.md) for why these need no extra toolchain wiring and how the test executable links the full stack.

## Key Files

| File | Purpose |
| ---- | ------- |
| `CMakeLists.txt` | Root build: UI detection, Corrosion setup, WHOLE_ARCHIVE linking |
| `CMakePresets.json` | Per-platform build presets |
| `sdk/Cargo.toml` | Rust dependencies (`matrix-sdk`, `matrix-sdk-ui`, `cxx`, `tokio`, `livekit`) |
| `sdk/src/lib.rs` | `cxx::bridge` — the FFI boundary between Rust and C++ |
| `sdk/src/client.rs` | Matrix SDK wrapper (sync, rooms, messaging) |
| `sdk/src/oauth.rs` | RFC 8252 loopback OAuth flow (built on matrix-sdk's `local-server` feature) |
| `client/include/tesseract/*.h` | C++ public API headers |
| `ui/shared/tk/canvas.h` | Abstract 2D backend interface (Color/Rect/Point/Image/TextLayout) |
| `ui/shared/tk/host.h` | Per-platform `Host` + `NativeTextField` / `NativeTextArea` canvas-rendered native controls |
| `ui/shared/tk/widget.h` | Widget tree base: measure → arrange → paint + pointer dispatch |
| `ui/shared/views/*.h` | Cross-platform views mounted by every native shell |
| `ui/shared/app/ShellBase.h` | Platform-agnostic shell state + pure-virtual hooks shared by all four shells |
| `ui/shared/app/EventHandlerBase.h` | `IEventHandler` adapter: marshals SDK callbacks → UI thread → `handle_*_ui_()` virtuals |
| `ui/shared/app/RoomWindowBase.h` | Base for secondary (popout) room windows; each platform shell provides a concrete subclass |
| `ui/{linux-qt,linux-gtk,windows}/src/MainWindow.h`, `ui/macos/src/MainWindowController.mm` | Per-platform shells (thin). Each implements `IEventHandler` and marshals to the UI thread via its native primitive — `QueuedConnection` (Qt6), `g_idle_add` (GTK4), `PostMessage` (Win32), `dispatch_async` (macOS, `MacShell : ShellBase` composition) |

## Roadmap

See [ROADMAP.md](ROADMAP.md) for pending work, known gaps, and open design decisions. Completed work is in [CHANGES.md](CHANGES.md).

## Changelog

CHANGES.md entries are terse: 1-2 sentences per bullet — what changed, the one-line reason if it's non-obvious, then the verification tag (build/platform status, test count). Do not write a postmortem: no numbered "(1)/(2)" breakdowns, no background investigation narrative, no external issue links, no restating the full root-cause analysis already covered by the commit message. If a fix needed real investigation, that detail belongs in the commit message, not the changelog bullet.

## Code Style

See [STYLE.md](STYLE.md) for formatting and naming conventions that apply across all C++ and Rust code in this repo.

## Workflow

Never commit or push changes without explicit confirmation from the user that the change has been tested and works as expected. Exception: when a change targets a platform that cannot be built or tested on the current machine (e.g. Windows- or macOS-specific code written from a Linux session, with no cross-compile toolchain to rely on), testing genuinely cannot happen before the code exists somewhere the user can build it — commit is the enabling step, not something to gate on prior test results. In that case, explicit user confirmation to commit is still required (never commit unprompted), but don't withhold that confirmation request pending a test result that isn't obtainable yet; say plainly that the change is unverified and why, and let the user decide.

Never submit anything — commit, push, PR, or any outward-facing action — unless the user explicitly requests it. A request to *make* or *update* something (code, docs, config) is never authorization to submit it. When in doubt, present the changes and wait.

When presenting options or asking a question, there is no timeout. If the user does not answer, wait forever — do not pick an option, assume an answer, or proceed on a default. A question or choice is only resolved by the user's actual response.

When the user reports a bug and a solution is found, always confirm the selected solution with the user first before starting ANY implementation. NEVER implement a fix without the user's input, because the user, who is a developer too, might have a better solution.

When investigating a bug or unexpected behavior, if the first fix attempt fails, always offer the user the chance to set breakpoints before proceeding. Pause and ask: "Would you like to set any breakpoints before I continue?" — this lets the user inspect state at key points rather than relying solely on log output or re-runs.

NEVER run the app yourself (launching a built binary/bundle, driving it, etc.) unless the user explicitly requests it. Taking screen captures or using screen/window automation (e.g. `screencapture`, `osascript`/System Events, accessibility APIs) without the user's authorization is FORBIDDEN — the display is the user's real, live desktop, not an isolated sandbox. A clean build is sufficient confirmation on its own; only launch or drive the app when the user explicitly asks for a live/visual check.

## Internationalisation

Every user-visible string in C++ **must** be wrapped in `tk::tr()` (singular), `tk::trn()` (plural), or `tk::trf()` (interpolated). Never pass a raw string literal directly to a widget or label where it will be displayed to the user.

```cpp
// Wrong
dev_group->add_widget(std::make_unique<tk::Label>("Microphone"));

// Correct
dev_group->add_widget(std::make_unique<tk::Label>(tk::tr("Microphone")));
```

After adding new strings, add the corresponding `msgid`/`msgstr` entries to every `.po` file under `i18n/` (currently `es.po` and `pseudo.po`). Strings added without `.po` entries will silently fall back to English and will never be translated.

Include `ui/shared/tk/i18n.h` to access `tk::tr`, `tk::trn`, and `tk::trf`.

## Shared vs Platform Code

Always implement new functionality in `ui/shared/` (`tk/` or `views/`) rather than duplicating it across platform shells. Platform shells (`ui/windows/`, `ui/linux-qt/`, `ui/linux-gtk/`, `ui/macos/`) should contain only what is genuinely platform-specific: native window/menu management, OS API calls, and thin wiring to the shared layer. If you find yourself writing the same logic in two or more shells, that logic belongs in `ui/shared/`.
