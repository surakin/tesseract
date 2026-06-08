# Multi-Window Multi-Account Design

**Date:** 2026-06-08  
**Status:** Approved

## Overview

Support multiple simultaneous main windows, one per logged-in account. Ctrl+click on the account picker opens a new dedicated window for the selected account. Closing a window does not quit the app unless it is the last one. Selecting an account in the picker that already has a dedicated window activates that window instead of switching the current one.

## Behavior Specification

### Account picker interactions

| Gesture | Account has dedicated window | Account has no dedicated window |
| --- | --- | --- |
| Regular click | Raise and activate the dedicated window | Switch the current window to that account (existing behavior) |
| Ctrl+click | Raise and activate the existing dedicated window (no second window spawned) | Spawn a new dedicated window for that account |

### Window close

- Closing a window clears its dedicated-window registration and removes it from the window list.
- The account keeps syncing — `AccountManager` retains its `shared_ptr<AccountSession>`.
- If the closed window was the last one, the app quits. This applies on all four platforms, including macOS (deviates from macOS convention intentionally, for cross-platform consistency).

## Architecture

### `AccountManager` (new class)

Created in each platform's `main()` and passed by reference to every `ShellBase` constructor. Not a singleton — lives on the stack in `main()`.

**Owns:**

- `std::vector<std::shared_ptr<AccountSession>> accounts_`
- `SessionStore` (load/save/restore sessions from disk)
- Shared media caches: `thumbnail_cache_`, `image_cache_`, `anim_cache_`, `media_disk_cache_`

**Window registry (also in `AccountManager`):**

- `std::vector<ShellBase*> all_windows_`
- `std::unordered_map<std::string, ShellBase*> dedicated_windows_` — user\_id → window

**Interface:**

```cpp
class AccountManager {
public:
    // Sessions
    void restore_sessions();
    std::shared_ptr<AccountSession> add_account(SessionJson);
    void remove_account(std::string_view user_id);
    std::shared_ptr<AccountSession> find(std::string_view user_id); // nullptr if not found
    std::span<std::shared_ptr<AccountSession> const> accounts() const;

    // Shared caches
    ThumbnailCache& thumbnail_cache();
    ImageCache& image_cache();
    AnimCache& anim_cache();
    MediaDiskCache& media_disk_cache();

    // Window registry
    void register_window(ShellBase* w);
    void unregister_window(ShellBase* w);
    void set_dedicated(std::string_view user_id, ShellBase* w);
    void clear_dedicated(std::string_view user_id);
    ShellBase* dedicated_window(std::string_view user_id) const; // nullptr = no dedicated window
    int window_count() const;
};
```

### `ShellBase` changes

**Removed:**

- `std::vector<std::unique_ptr<AccountSession>> accounts_` → moved to `AccountManager`
- `SessionStore` ownership → moved to `AccountManager`
- Owned shared media caches → moved to `AccountManager`

**Added:**

- `AccountManager& account_manager_` reference (set in constructor)
- `std::shared_ptr<AccountSession> active_account_` — replaces `active_account_index_`; shared ownership so the session stays alive if `AccountManager` removes it (e.g. logout) while the window still holds a reference
- `virtual void raise_and_activate() = 0` — platform-specific bring-to-front
- `virtual void spawn_main_window_(std::shared_ptr<AccountSession> account) = 0` — platform-specific window factory, called from account picker on Ctrl+click

**Unchanged:**

- `switch_active_account()` — now reassigns `active_account_` pointer instead of indexing its own vector; all other logic stays the same
- All UI state: `current_room_id_`, `rooms_`, `invites_`, room view, compose bar, thread panel
- All event handler plumbing

### Account picker (`AccountPicker` view)

No new state. Click handling changes:

**Regular click on account X:**

```cpp
auto* win = shell_->account_manager().dedicated_window(X.user_id)
if (win) win->raise_and_activate()
else      shell_->switch_active_account(X)
```

**Ctrl+click on account X:**

```cpp
auto* win = shell_->account_manager().dedicated_window(X.user_id)
if (win) win->raise_and_activate()   // already dedicated, just focus it
else     shell_->spawn_main_window_(account_manager_.find(X.user_id))
```

`spawn_main_window_` implementations:

- Construct a new platform-specific window (e.g. `new MainWindow` on Qt, new `GtkApplicationWindow` on GTK, new `HWND` on Win32, new `NSWindow` on macOS)
- New window constructor calls `account_manager_.register_window(this)`, sets `active_account_`, and calls `account_manager_.set_dedicated(user_id, this)`

### Platform close handlers

Each platform's close event (Qt `closeEvent`, GTK `delete-event`, Win32 `WM_CLOSE`, macOS `windowWillClose:`):

```cpp
if (account_manager_.dedicated_window(active_account_->user_id) == this)
    account_manager_.clear_dedicated(active_account_->user_id)
account_manager_.unregister_window(this)
if (account_manager_.window_count() == 0) quit_app()
else destroy_this_window()
```

The guard is necessary because a window that reached account X via a regular click (not a dedicated spawn) must not clear the dedicated registration that belongs to a different window.

## Implementation Phases

### Phase 1 — Extract `AccountManager` (pure refactor)

Extract `accounts_`, `SessionStore`, and shared caches from `ShellBase` into the new `AccountManager` class. Update all four platform `main()` functions to construct `AccountManager` and pass it to their shell. Replace `active_account_index_` with `shared_ptr<AccountSession> active_account_`. No behavior change — all existing tests must continue to pass.

### Phase 2 — Window registry + picker behavior

Add the window registry to `AccountManager`. Add `raise_and_activate()` pure-virtual to `ShellBase` and implement it on all four platforms. Update `AccountPicker` click handling to check for a dedicated window before switching. Register and unregister the initial single window at startup and shutdown.

### Phase 3 — New window spawning + quit behavior

Add `spawn_main_window_()` pure-virtual to `ShellBase` and implement it on all four platforms. Wire Ctrl+click in `AccountPicker`. Update each platform's close handler to check `window_count()` before quitting. Update `settings.h` `PopoutEntry` to include `user_id` so popout windows are correctly scoped per account when multiple windows are open.

### Systray menu

The systray menu gains one entry per open main window, allowing the user to raise any window directly from the tray. Each entry shows the active account's display name and user ID. Clicking an entry calls `raise_and_activate()` on that window.

Example menu layout:

```text
Alice (@alice:example.com)
Bob (@bob:example.com)
──────────────────────────
Quit
```

The tray menu is rebuilt whenever a window is opened or closed (i.e. on `register_window` / `unregister_window`). Each platform shell already rebuilds its tray menu on demand; the change is to iterate `account_manager_.all_windows()` and insert an item per window. `ShellBase` exposes `active_account_->display_name` and `active_account_->user_id` for the label.

This is added to Phase 2 (window registry work), since it depends on `all_windows_` being populated.

## What Does Not Change

- `EventHandlerBase` filtering by `user_id` — each window's event handler is already tied to one active account; filtering stays correct without modification
- `RoomWindowBase` / secondary popout windows — each main window continues to own its own `secondary_windows_` map; popouts are already account-scoped through their parent shell
- Sync lifecycle — all accounts continue syncing regardless of whether they have a dedicated window; this was already true before this feature
- Tray icon logic — aggregate unread count is unaffected; `AccountManager::accounts()` gives the full list
