# Profile Edit — Display Name & Avatar — Design Spec

**Date:** 2026-05-21
**Scope:** Add inline display-name editing and avatar upload/remove to the Account
settings tab, gated on server capability flags.

---

## 1. Architecture

```
Shell (Qt6 / GTK4 / Win32 / macOS)
  │  provides: post_to_ui_, open_file_picker_, Client*
  │  owns: std::unique_ptr<SettingsController>
  ▼
SettingsController          ui/shared/app/SettingsController.h/.cpp
  │  runs async ops, marshals results back to UI thread
  │  holds: Client*, post_to_ui_, open_file_picker_
  ▼
SettingsView / AccountSection   ui/shared/views/SettingsView
  │  purely presentational — busy/error states, hover overlay
  │  calls controller methods on user action
  ▼
Client C++ / Rust SDK
  new: set_display_name(), upload_avatar(bytes, mime), remove_avatar()
```

The shell constructs `SettingsController` with platform-specific hooks and
passes a non-owning pointer to `SettingsView`. No platform code enters the
view; no view code touches the thread pool.

---

## 2. SDK / Client layer

### New Rust methods (`sdk/src/client.rs`)

All three are synchronous FFI calls using `rt.block_on`, matching the pattern
of `set_room_topic`, `send_read_receipt`, etc.

| Method | Matrix API | Returns |
|--------|-----------|---------|
| `set_display_name(name: &str)` | `PUT /_matrix/client/v3/profile/{userId}/displayname` via `matrix_sdk::Account::set_display_name(Some(name))` | `OpResult` |
| `upload_avatar(bytes: &[u8], mime_type: &str)` | Upload bytes via `matrix_sdk::Media::upload`, then `Account::set_avatar_url(Some(mxc_uri))` | `OpResult` |
| `remove_avatar()` | `Account::set_avatar_url(None)` | `OpResult` |

### cxx bridge (`sdk/src/lib.rs`)

Three new entries in the `extern "Rust"` block, mirroring the existing `OpResult`
return pattern.

### C++ Client (`client/include/tesseract/client.h` + `client/src/client.cpp`)

```cpp
OpResult set_display_name(const std::string& name);
OpResult upload_avatar(const std::vector<uint8_t>& bytes,
                       const std::string& mime_type);
OpResult remove_avatar();
```

Thin wrappers that forward to the Rust FFI, same as all other `Client` methods.

### Capability gating

`ServerInfo::can_set_displayname` and `can_set_avatar` are already populated
from the homeserver capabilities response in `get_server_info()`. No new Rust
work required.

---

## 3. SettingsController

**Location:** `ui/shared/app/SettingsController.h` / `SettingsController.cpp`

```cpp
class SettingsController {
public:
    SettingsController(
        tesseract::Client* client,
        std::function<void(std::function<void()>)>          post_to_ui,
        std::function<void(std::function<void(
            std::vector<uint8_t>, std::string)>)>           open_file_picker);

    // Rebind to a different account without recreating the controller.
    void set_client(tesseract::Client* client);

    // Called by SettingsView on user action:
    void upload_avatar();
    void remove_avatar();
    void set_display_name(std::string name);

    // Wired by SettingsView:
    std::function<void(bool ok, std::string error)> on_avatar_result;
    std::function<void(bool ok, std::string error)> on_name_result;
    std::function<void(std::string new_mxc_url)>    on_avatar_changed;
    std::function<void(std::string new_name)>        on_name_changed;

private:
    tesseract::Client*  client_;       // non-owning, rebound on account switch
    // post_to_ui_ and open_file_picker_ stored as members
    std::atomic<bool>   avatar_in_flight_{false};
    std::atomic<bool>   name_in_flight_{false};
};
```

**Async pattern:** each action method checks and sets the relevant in-flight
flag, spawns a detached `std::thread`, calls the blocking client method, then
`post_to_ui_`s the result. No `workers_in_flight_` tracking — these are rare
user-initiated one-shots with no shutdown ordering concern (the controller is
destroyed before the client on account switch via `set_client(nullptr)` called
before the account session is torn down).

**Stale-result guard:** the spawned thread captures a `Client*` snapshot at
dispatch time. On callback, the controller compares it against the current
`client_`; if they differ (account switched mid-flight) the result is silently
dropped and in-flight flags are cleared.

---

## 4. SettingsView / AccountSection

### New widget: `tk::InlineTextField`

**Location:** `ui/shared/tk/inline_text_field.h` / `inline_text_field.cpp`

A lightweight canvas-drawn single-line text widget. Renders as a plain text
label until focused; on focus shows a subtle underline and cursor. No native
overlay — display names are short strings with no IME complexity.

- `set_text(std::string)` / `text() → std::string`
- `set_editable(bool)` — when false renders as a plain label, no focus/cursor
- `set_busy(bool)` — disables input, draws a small spinner to the right
- `set_error(std::string)` — draws red error string below; cleared on next
  keystroke or on `set_error("")`
- `on_submitted` — `std::function<void(std::string)>` fired on Enter or blur
  when the value has changed

### AccountSection changes

**Display name:**
- Replace the static name label with `tk::InlineTextField`.
- `set_editable(server_info.can_set_displayname)` — always visible, editable
  only when the server permits.
- `on_submitted` → `controller_->set_display_name(text)` + `set_busy(true)`.
- `on_name_result(ok, error)`: `set_busy(false)`, on failure `set_error(error)`.
- `on_name_changed(name)`: `set_text(name)`.

**Avatar:**
- Disc always renders (existing behavior).
- When `server_info.can_set_avatar` is true: on pointer-enter draw a
  semi-transparent scrim over the disc with a centered camera icon; if the
  current avatar is non-empty also draw a small ✕ chip in the top-right corner.
- Click on disc → `controller_->upload_avatar()`.
- Click ✕ → `controller_->remove_avatar()`.
- While `avatar_in_flight_`: scrim replaced by a spinner, ✕ hidden.
- `on_avatar_result(ok, error)`: clear busy state; on failure show red error
  string below the disc (cleared on next hover-enter or new action).
- `on_avatar_changed(mxc)`: update the displayed avatar image.
- When `can_set_avatar` is false: hover overlay never activates.

**`set_controller(SettingsController*)`** — new method; wires up all
`on_*` callbacks from the controller and stores the pointer.

**`set_server_info(const ServerInfo&)`** — already called from
`on_server_info_ready_ui_()` on every shell. Before this arrives the fields
render as non-editable (safe default regardless of the `can_set_*` defaults
in the struct).

---

## 5. Shell wiring

Each shell (Qt6, GTK4, Win32, macOS) does three things:

### 5.1 Construct SettingsController

At login time (the same point `start_sync` is called and the `Client*` becomes
valid), create:

```cpp
settings_controller_ = std::make_unique<SettingsController>(
    client_,
    [this](auto fn) { post_to_ui_(std::move(fn)); },
    [this](auto cb) { open_avatar_file_picker_(std::move(cb)); }
);
```

**`open_avatar_file_picker_`** per platform:
- **Qt6:** `QFileDialog::getOpenFileName(…, "Images (*.png *.jpg *.gif *.webp)")`,
  read with `QFile`, call `cb(bytes, mime)` via `QMetaObject::invokeMethod`.
- **GTK4:** `GtkFileChooserNative` with image filter, read with `g_file_load_contents`,
  post result via `g_idle_add`.
- **Win32:** `IFileOpenDialog` with image filter, read with `CreateFile`/`ReadFile`,
  post via `PostMessage`.
- **macOS:** `NSOpenPanel` with image types, read with `NSData`, post via
  `dispatch_async(dispatch_get_main_queue(), …)`.

### 5.2 Pass controller to SettingsView

```cpp
settings_view_->set_controller(settings_controller_.get());
```

Called alongside the existing `set_server_info` call.

### 5.3 Rebind on account switch

In `switchActiveAccount` (Qt6/GTK4/Win32) / the macOS equivalent:

```cpp
settings_controller_->set_client(accounts_[new_idx]->client.get());
```

Called after the new client is active. Any in-flight operation from the
previous account is abandoned (stale-result guard fires, flags clear).

---

## 6. Capability gating and error flow

**Pre-fetch state:** `ServerInfo` arrives asynchronously after login. Until
`set_server_info` is called on `AccountSection`, all profile fields render
non-editable. This prevents a flash of editable UI that immediately becomes
locked if the server denies the capability.

**Error display:**
- `OpResult::message` from the Rust FFI is shown verbatim as the inline error
  string — same approach as sync error display elsewhere.
- Errors are transient: cleared by the next user interaction on the same field.

**In-flight guard:**
- While `avatar_in_flight_` is true: upload and remove actions are both
  disabled in the view (scrim shows spinner, clicks ignored).
- While `name_in_flight_` is true: the name field is disabled.
- Double-submission is therefore impossible without any mutex.

---

## 7. Files changed / created

| File | Change |
|------|--------|
| `sdk/src/client.rs` | Add `set_display_name`, `upload_avatar`, `remove_avatar` |
| `sdk/src/lib.rs` | Expose three methods via cxx bridge |
| `client/include/tesseract/client.h` | Declare three new `Client` methods |
| `client/src/client.cpp` | Implement three FFI wrappers |
| `ui/shared/tk/inline_text_field.h/.cpp` | New `InlineTextField` widget |
| `ui/shared/app/SettingsController.h/.cpp` | New controller class |
| `ui/shared/views/SettingsView.h/.cpp` | Avatar hover overlay, inline name field, `set_controller`, `set_server_info` guard |
| `ui/linux-qt/src/MainWindow.h/.cpp` | Construct controller, file picker, rebind on switch |
| `ui/linux-gtk/src/MainWindow.h/.cpp` | Same |
| `ui/windows/src/MainWindow.h/.cpp` | Same |
| `ui/macos/src/MainWindowController.mm` | Same |
