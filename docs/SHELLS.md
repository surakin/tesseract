# The application layer and the platform shells

The shared views draw the interface, but something has to decide what happens when you click. That is the application layer in `ui/shared/app/`, built around a class called `ShellBase`. Each platform's window class inherits from it. This page explains how the pieces fit and how to work in them without breaking the other three platforms.

Background: [ARCHITECTURE.md](ARCHITECTURE.md) (the big picture and threading diagrams), [TOOLKIT.md](TOOLKIT.md) (the views), [BRIDGE.md](BRIDGE.md) (the Rust side).

## Three roles

| Role | Where | Job |
| --- | --- | --- |
| **Views** | `ui/shared/views/` | Draw things; report user actions through callbacks. No app state. |
| **Application layer** | `ui/shared/app/` | Decide what an action means: call the SDK, update state, push results back into views. Identical on every platform. |
| **Shell** | `ui/windows/`, `ui/macos/`, `ui/linux-qt/`, `ui/linux-gtk/` | The native window, menus, tray, notifications, and the toolkit backend. As thin as possible. |

The rule from [DEVELOPING.md](DEVELOPING.md) follows: if logic would be the same on two platforms, it lives in `app/` or `views/`, never in a shell.

## ShellBase

`ShellBase` holds all platform-agnostic state and behaviour: the signed-in accounts, which room is open, tabs and windows, settings, the send queue, calls, search, and more. It is large, so it is split by subsystem:

```text
ShellBase.h                   the class (state, hooks)
ShellBase.cpp                 status messages, the worker pools, debounce helpers
ShellBase_accounts.cpp        sign-in, switching, logout, windows per account
ShellBase_rooms.cpp           room list, joining, invites
ShellBase_timeline.cpp        messages as they arrive
ShellBase_navigation.cpp      tabs, back/forward history, pin/unpin, quick switcher, window title
ShellBase_media_*.cpp         downloading and decoding images
ShellBase_settings.cpp        applying settings
ShellBase_encryption.cpp, _calls.cpp, _search.cpp, _spaces.cpp, _profile.cpp (also jump-to-event), _windows.cpp
ShellBase_wiring.cpp          connect view callbacks to the above
```

When you add a `ShellBase::` member, put it in the file for its subsystem.

Big, self-contained subsystems are being pulled out into **collaborator** classes next to it: `AccountManager`, `RoomPane`, `SendPipeline`, `SettingsController`, `EncryptionFlowController`, `ThreadPanelController`, `HistoryExportController`, `PowerPolicy`, `SearchBackend` and others. (`SettingsController` is the odd one out: it lives in `ui/shared/views/settings/`.) Roadmap items for the remaining cuts are in ROADMAP.md. When you extract one:

- keep the fields that shells read (`client_`, `account_manager_`, `main_room_pane_`, `rooms_`) on `ShellBase`;
- add no new pure virtuals; give the collaborator `std::function` hooks instead;
- keep every shell and `tests/cpp/shell_test_double.h` compiling without changes.

### What a shell must provide

A shell fills in a short list of hooks, nearly all of them about the platform itself:

- run a function on the UI thread (`post_to_ui_`, `post_to_ui_after_`) and read a monotonic clock;
- request a repaint or relayout, and raise and focus the window;
- create the call window and the audio output for calls;
- build the per-account event bridge and notifier;
- decode an image (`decode_image_`) and pick an image file (`pick_image_file_`);
- open another main window for an account (`spawn_main_window_`) and navigate to a room (`navigate_to_room_`);
- report whether Ctrl is held.

Everything else has a working default in `ShellBase`, and a shell overrides a virtual no-op only when it has a real platform effect, such as the tray icon, dock badge or taskbar overlay. Today's Qt shell is `ui/linux-qt/src/MainWindow.cpp`. It inherits from `QMainWindow` and `ShellBase`. On macOS the same role is played by `macos::MainWindow`, and the Objective-C `MainWindowController` only forwards AppKit callbacks to it.

## Wiring: how a click reaches the SDK

1. A view reports an action through a callback (`on_pin_requested(event_id)`).
2. The app layer connects it. Callbacks that behave the same on every shell are wired once, in `ShellBase_wiring.cpp` (`wire_main_app_widget_`, `wire_overlay_nav_`). Callbacks of the open room are wired in `RoomPane::wire_room_view_`. A shell keeps a callback only for a genuine platform effect, through a hook with a default.
3. The handler checks its inputs, **captures what it needs by value**, and hands the blocking work to a worker pool.
4. The worker calls `tesseract::Client`, which calls into Rust.
5. If the UI must change, the worker posts the update back to the UI thread.

`RoomPane::pin_event_` is a short example. `wire_room_view_` connects `on_pin_requested` to it. It copies the session, room id and event id, calls `run_async_mut_`, and checks `r.ok`. For now a failure is only logged to stderr; a status message is a TODO.

## Threads

The UI thread owns widgets and window state. Blocking calls must never run on it. `ShellBase` has three worker pools:

| Pool | Threads | Use |
| --- | --- | --- |
| `pool_` | 2 to 8 | Reads and preparation: image decode, disk-cache I/O, short blocking reads. Started with `run_async_`. |
| `mut_pool_` | 1 | Calls that change state (send, subscribe). One thread on purpose, so those calls are serialized. Started with `run_async_mut_`. |
| `media_prefetch_pool_` | 2 | Only the pre-paint disk-cache prefetch, kept apart so it never waits behind network work. |

Large media downloads are not pool tasks. They run as async tasks inside Rust and complete through a callback.

**Liveness.** A worker may finish after the window, account or shell has gone. Two habits keep this safe:

- capture the `AccountSession` as a `shared_ptr` so it stays alive, and re-check it inside the task;
- use `post_to_ui_alive_` (and `weak_flag()` for your own captures) instead of raw `post_to_ui_`, so the posted function is dropped if the shell is already destroyed.

## Events coming back from Rust

Rust calls the C++ `IEventHandler` from its own worker threads. `EventHandlerBase` implements every callback the same way: copy the arguments, `post_to_ui_` to the owning window, then call a matching `handle_*_ui_()` virtual on `ShellBase`. For example `on_message_inserted` ends up in `handle_message_inserted_ui_`. So all handlers run on the UI thread, in order.

The same bridge also does two safety jobs: it points at the window that currently owns the account (an account can move between windows), and high-rate streams such as call video frames use a "latest frame wins" mailbox so a busy UI never queues megabytes of stale frames.

To add a callback end to end, see the FFI recipe in [DEVELOPING.md](DEVELOPING.md) and [BRIDGE.md](BRIDGE.md).

## Accounts and windows

- **`AccountManager`** is shared by all windows. It stores one `AccountSession` per signed-in account and the caches. Its account list is UI-thread-only. The one exception is a "draining" flag used while an account is being logged out, because a stale worker may still hold the session.
- **`AccountSession`** is one per account: its `Client`, its event bridge and its notifier.
- **Windows.** The main window, extra room windows (`RoomWindowBase`), call windows (`CallWindowBase`) and plain secondary windows such as the Activity Monitor (`AuxWindowBase`) all share one `ShellBase`-driven model. Each account is owned by exactly one window at a time, and the bridge is re-pointed when an account moves. Ctrl+click on a room tab or an account opens another window.

## What goes in a shell

Only the genuinely native parts:

- the window class and its menus, tray icon, dock or taskbar integration;
- native notifications, global shortcuts registered from `shortcut_registry.h`, system search providers and media controls;
- the toolkit backend in `ui/<platform>/tk/` ([TOOLKIT.md](TOOLKIT.md));
- OS-specific build files.

If a shell function is longer than a few lines and has no OS call in it, move it into `app/`.

## Testing this layer

`tests/cpp/shell_test_double.h` is a `ShellBase` with the platform hooks stubbed out, so most application logic is testable without a window. It must drain its worker pools in its destructor, or tests can crash in parallel runs. Tests never touch your real settings or data: a listener redirects the app's directories for each test.
