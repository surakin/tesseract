## What This Is

Tesseract is a cross-platform desktop Matrix/chat client. The core networking is Rust (using `matrix-sdk`), exposed to C++ via a `cxx` FFI bridge. Platform-specific UIs are written in C++ targeting Win32 (Windows), AppKit (Objective-C++, macOS), Qt6 Widgets, or GTK4 (Linux).

## Build and Test

Per-platform prerequisites, the full preset list, the `-DTESSERACT_UI=` override and build internals live in [docs/BUILD.md](docs/BUILD.md). Day to day:

```bash
cmake --preset linux-debug               # macOS: macos-appkit-arm64-debug (or the x86_64 preset)
cmake --build build/linux-debug
ctest --test-dir build/linux-debug --output-on-failure   # each Catch2 TEST_CASE is its own ctest test
cargo test -p tesseract-sdk-ffi          # Rust unit tests, no C++ needed
```

## Architecture

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (including its source map) for the full picture.

```text
sdk/         Rust crate: matrix-sdk wrapper + cxx FFI bridge (sdk/src/bridge.rs, sdk/src/client/)
client/      C++ static library: high-level C++ API over the FFI (client/include/tesseract/)
ui/shared/   four static libs, each linking the previous: tesseract_tk (tk/: Canvas/Widget/Layout/Host) <- tesseract_platform (platform/: OS services)
             <- tesseract_views (views/: shared views) <- tesseract_app (app/: ShellBase); shells link only tesseract_app
ui/{windows,macos,linux-qt,linux-gtk}/   thin native shells + their own tk/ backend impls
```

- **`sdk/`**: all async I/O lives here (a `tokio` runtime on background threads); the bridge exposes a synchronous C API. `oauth.rs` drives the RFC 8252 loopback-redirect flow on matrix-sdk's `local-server` feature; Tesseract runs no HTTP server of its own.
- **`client/`**: `tesseract::Client` (Pimpl) wraps the FFI; UIs implement `tesseract::IEventHandler` to receive async callbacks; `SessionStore` persists the session JSON and the per-account matrix-sdk store. Account data lives under `data_dir()` (`%APPDATA%/Tesseract/`, `~/Library/Application Support/Tesseract/`, `~/.local/share/tesseract/`); only `app_settings.json` lives in `config_dir()` (`~/.config/tesseract/` on Linux; equal to `data_dir()` on Windows/macOS).
- **`ui/shared/tk/`** (the `tesseract_tk` library, the bottom of the shared stack): owns drawing, layout, hit-test, focus and keyboard. `tk::Canvas` is the abstract 2D backend (D2D, QPainter, Cairo+Pango, CoreGraphics+CoreText); `tk::Host` is the per-platform integration surface (repaint, post-to-UI, native edit overlays). Text input stays backed by real native controls for IME/selection, but `tk::NativeTextField` / `NativeTextArea` render into the canvas via `rendered_image()` + `Canvas::draw_image()`. Per-platform backend impls live in each `ui/<platform>/tk/`.
- **`ui/shared/platform/`**: non-UI OS services (video/screen capture, geolocation, single instance, GStreamer/portal/UnifiedPush helpers) with per-OS backends in `platform/{linux,win32,macos}/` picked by `TESSERACT_UI`. The `layering_check` ctest (`tests/check_layering.py`) enforces the include direction: tk includes none of platform/views/app, platform includes neither views/ nor app/, views does not include app/. The `tk::Host`-vended media interfaces (audio/video players, audio capture/playback, device listing) stay in `tk/` with their backends in the shells. Put new non-UI OS services here, not in `tk/` or a shell.
- **`ui/shared/app/`**: `ShellBase` holds all platform-agnostic shell state as `protected` members with pure-virtual hooks and virtual no-ops each shell overrides. View callbacks whose logic is the same on every shell are wired once in `ShellBase_wiring.cpp` (`wire_main_app_widget_`, `wire_overlay_nav_`); a shell keeps a callback only for a platform effect, via a defaulted hook. `EventHandlerBase : IEventHandler` marshals every SDK callback to the UI thread via `post_to_ui_()` then calls a `handle_*_ui_()` virtual. Every shell's main window inherits `ShellBase`; on macOS that is `macos::MainWindow`, and the ObjC `MainWindowController` only forwards AppKit delegate/menu callbacks to it.
  - `ShellBase.cpp` is split into `ShellBase_<subsystem>.cpp`: put a new `ShellBase::` member in the file for its subsystem. Standalone types and pure helpers live in `WorkerPool.h`, `AccountLifecycleTypes.h`, `MediaTypes.h`, `shell_helpers.h`.
  - Cohesive subsystems are being split into collaborators (`AccountManager`, `RoomPane`, `SendPipeline`, …; remaining cuts are under *Code health* in ROADMAP.md). When extracting, keep shell-read fields (`client_`, `account_manager_`, `main_room_pane_`, `rooms_`) on `ShellBase`, add no pure virtuals (use `std::function` hooks), and keep the shells and `tests/cpp/shell_test_double.h` compiling untouched.
- **Shells (`ui/{windows,macos,linux-qt,linux-gtk}/`)**: own the window/menu/AX surface and mount `tk::*::Surface`s hosting shared views. Rust callbacks arrive on worker threads and reach the UI thread through `tk::Host::post_to_ui` (`QueuedConnection`, `g_idle_add`, `PostMessage`, `dispatch_async`).

Two non-obvious entry points: `ui/shared/views/shortcut_registry.h` is the one table of keyboard shortcuts (shells bind global accelerators from it, views match with `matches()`, the shortcuts overlay lists it): add a shortcut there, never as a hand-written binding in a shell. Settings: `binder.bind(control, &Settings::field, SettingKey::X)` in `SettingsView`; add a `case` in `ShellBase::handle_setting_changed_` only for a side effect beyond saving.

## Roadmap and Changelog

See [ROADMAP.md](ROADMAP.md) for pending work and open design decisions. Completed work is in [CHANGES.md](CHANGES.md).

### CHANGES.md entries: ONE short sentence

**Hard limit: a bullet is one sentence of at most ~150 characters, then the verification tag.** Long bullets have been written repeatedly; this rule is not optional.

Format: `- type(scope): what changed, in plain words. <verification tag>`

- Say *what changed* for the user. Add a reason only if it is not obvious, in a few words.
- NEVER list implementation details: no class, function or file names beyond one if essential, no "X is now Y, Z is gone" chains, no per-platform breakdowns, no "also".
- No numbered "(1)/(2)" lists, no investigation narrative, no external issue links, no root-cause analysis. All of that belongs in the commit message, not here.
- Several unrelated changes mean several bullets, never one long bullet.
- Before saving, re-read the bullet: if it has a second clause joined by a semicolon, a colon-list, or more than one "and", cut it down.

```text
Bad:  - fix(ui): the space picker stops above the user-info strip, which now runs under it; with the sidebar collapsed or single-pane the picker hides and pinned spaces return to the room list. `UserInfo::measure` takes its line heights from the backend, the strip is as tall as it needs, and `user_strip_height()` is gone: ...
Good: - fix(ui): the space picker no longer covers the user-info strip, and hides when the sidebar is collapsed or single-pane. Linux release build
```

Never mention public GitHub issues (this repo's or upstream's) in commit messages. If an issue reference is needed for context, put it in CHANGES.md's detailed section instead.

## Code Style

See [STYLE.md](STYLE.md) for formatting and naming conventions that apply across all C++ and Rust code in this repo.

## Workflow

Model split: Opus does the planning (exploration, design, writing the implementation plan); Sonnet does the implementing (executing the plan, writing code and tests). When dispatching implementation subagents, pass `model: "sonnet"`.

**Submitting.** Never commit or push changes without explicit confirmation from the user that the change has been tested and works as expected. Never submit anything — commit, push, PR, or any outward-facing action — unless the user explicitly requests it; a request to *make* or *update* something (code, docs, config) is never authorization to submit it. When in doubt, present the changes and wait. Exception: when a change targets a platform that cannot be built or tested on the current machine (e.g. Windows- or macOS-specific code written from a Linux session, with no cross-compile toolchain to rely on), commit is the enabling step, not something to gate on prior test results. Explicit user confirmation to commit is still required; say plainly that the change is unverified and why, and let the user decide.

**Questions.** When presenting options or asking a question, there is no timeout. If the user does not answer, wait forever — do not pick an option, assume an answer, or proceed on a default.

NEVER jump to implementation in the middle of a conversation with the user, or right after the user asks a question. If the user has not agreed with an idea, don't implement it.

**Bugs.** When the user reports a bug and a solution is found, always confirm the selected solution with the user first before starting ANY implementation. NEVER implement a fix without the user's input, because the user, who is a developer too, might have a better solution. If the first fix attempt fails, offer the user the chance to set breakpoints before proceeding: "Would you like to set any breakpoints before I continue?"

**Running and building.**

- NEVER run the app yourself (launching a built binary/bundle, driving it, etc.) unless the user explicitly requests it. Taking screen captures or using screen/window automation (e.g. `screencapture`, `osascript`/System Events, accessibility APIs) without the user's authorization is FORBIDDEN — the display is the user's real, live desktop, not an isolated sandbox. A clean build is sufficient confirmation on its own.
- NEVER do a cross-platform compile check (e.g. the `windows-xwin-*` presets, or any build targeting a platform other than the host) unless the user explicitly requests it. Build and test only for the host platform.
- When `ctest` reports failing tests, do not immediately stash/revert the change and rebuild to prove whether it caused them. Report the failing test names to the user first and wait for instructions.

## Internationalisation

Every user-visible string in C++ **must** be wrapped in `tk::tr()` (singular), `tk::trn()` (plural), or `tk::trf()` (interpolated). Never pass a raw string literal directly to a widget or label where it will be displayed to the user. Include `ui/shared/tk/i18n.h` for `tk::tr`, `trn`, `trf`, `N_`, `format_date` and `format_size`.

```cpp
// Wrong
dev_group->add_widget(std::make_unique<tk::Label>("Microphone"));

// Correct
dev_group->add_widget(std::make_unique<tk::Label>(tk::tr("Microphone")));
```

- **Build sentences with `trf`/`trn`, never by concatenation.** `tk::trf(tk::tr("{0} is typing…"), {name})`, not `name + " is typing…"`; word order differs between languages.
- **Tables of labels:** mark each literal with `tk::N_("…")` so extraction finds it, and pass it through `tk::tr()` where it's shown. An unmarked table entry never reaches the catalogs.
- **Strings outside `ui/`:** `i18n-extract` only scans `ui/`. A literal elsewhere that reaches the UI (e.g. emoji category names in `client/src/emoji.cpp`) needs a local no-op `N_()` marker and its file listed in `cmake/I18n.cmake`.
- **Dates and sizes:** use `tk::format_date(tm, tk::tr("%B %-d, %Y"))` and `tk::format_size(bytes)`, not hand-rolled month tables or `" KB"` suffixes.
- **Native shells translate through `tk::tr` too.** A bare `tr()` in a Qt `QObject` is `QObject::tr` (no translator installed) and GTK's `_()` is unbound gettext; both always return English. On macOS use `TkTr("…")`; on Win32 convert the `tk::tr` result to UTF-16.

After adding new strings, add the corresponding `msgid`/`msgstr` entries to every `.po` file under `i18n/` (currently `de.po`, `es.po`, `fr.po`, `nb.po`, and `pseudo.po`). Strings added without `.po` entries silently fall back to English. The `i18n_catalogs_complete` ctest (`i18n/check_i18n.py`) fails and lists what's missing, and it also flags bare `tr()` / `_()` calls in the shells.

## Shared vs Platform Code

Always implement new functionality in `ui/shared/` (`tk/` or `views/`) rather than duplicating it across platform shells. Platform shells should contain only what is genuinely platform-specific: native window/menu management, OS API calls, and thin wiring to the shared layer. If you find yourself writing the same logic in two or more shells, that logic belongs in `ui/shared/`.
