# Initial Account Encryption Setup — Design Spec

**Date:** 2026-05-30  
**Status:** Approved  

---

## Overview

A modal overlay wizard that guides users through end-to-end encryption setup immediately after login. Handles two distinct scenarios detected automatically from the Matrix SDK's `RecoveryState`:

- **Fresh account** (`Disabled`) — no cross-signing or key backup exists on the server yet; the wizard bootstraps both and displays the generated recovery key.
- **New device** (`Incomplete`) — cross-signing exists on the server but this device does not have the secrets; the wizard lets the user recover using their recovery key/passphrase or hand off to SAS device verification.

The existing `RecoveryBanner` is retired for the `Incomplete` case (replaced by this overlay). The `VerificationBanner` remains unchanged and is used as the SAS hand-off target.

---

## Section 1 — SDK Layer

### New Rust methods in `sdk/src/client/recovery.rs`

**`recovery_state() -> u8`**  
Reads `client.encryption().recovery().state()` synchronously (no `block_on` needed — same pattern as `needs_recovery()`). Returns:

| Value | `RecoveryState` | Meaning |
|-------|----------------|---------|
| 0 | `Unknown` | State not yet determined |
| 1 | `Disabled` | No encryption set up on this account |
| 2 | `Enabled` | Fully set up, nothing to do |
| 3 | `Incomplete` | Exists on server, device missing secrets |

**`enable_recovery(passphrase: &str) -> OpResult`**  
Wraps the `recovery.enable()` builder:
- Empty `passphrase` → `recovery.enable().await` (key-only mode)
- Non-empty → `recovery.enable().with_passphrase(passphrase).await`

Runs via `self.rt.block_on(async { ... })`. Inside the async block, the method polls the `EnableProgress` stream returned by the SDK (via `recovery.enable().subscribe()` or equivalent) and fires `on_enable_recovery_progress` via the bridge for each variant before the `block_on` call returns `OpResult`. The `recovery_key` string in the `Done` variant is passed through to the C++ side as the key to display.

**No changes to `recover()`** — already handles the new-device path.

### New FFI callback in `sdk/src/bridge.rs`

```
on_enable_recovery_progress(step: u8, recovery_key: &str, backed_up: u32, total: u32)
```

`step` encodes `EnableProgress`:

| Value | `EnableProgress` variant | Label shown in UI |
|-------|--------------------------|-------------------|
| 0 | `Starting` | "Starting…" |
| 1 | `CreatingBackup` | "Creating backup…" |
| 2 | `CreatingRecoveryKey` | "Generating recovery key…" |
| 3 | `BackingUp(n)` | "Backing up keys (n / total)…" |
| 4 | `Done` | — triggers ShowKey step |
| 5 | Error (internal) | — triggers error display |

On `Done`, `recovery_key` carries the generated key string (empty string when passphrase mode was chosen by the user).

### EventHandlerBase

New virtual added and marshalled to UI thread via `post_to_ui_`:

```cpp
virtual void handle_enable_recovery_progress_ui_(
    uint8_t step,
    const std::string& recovery_key,
    uint32_t backed_up,
    uint32_t total) {}
```

---

## Section 2 — Detection & Routing (ShellBase)

### Trigger

`check_encryption_setup_()` is called from `handle_rooms_updated_ui_()` on the first sync tick (guarded by the existing `first_sync_done_` flag pattern). It reads `client_->recovery_state()` and decides:

| State | Action |
|-------|--------|
| `Disabled` (1) | Call `show_encryption_setup_overlay_(Mode::Fresh)` |
| `Incomplete` (3) | Call `show_encryption_setup_overlay_(Mode::Recover)` |
| `Enabled` (2) | No-op |
| `Unknown` (0) | No-op; will re-check on next rooms-updated tick |

### Guard flags (per `AccountSession`)

- **`encryption_setup_shown_`** — set `true` the moment the overlay is raised; prevents re-raising if rooms-updated fires again.
- **`encryption_setup_dismissed_`** — set `true` when the user clicks Skip; prevents re-raising for the lifetime of the session on this account.

### Existing banner retirement

`check_and_show_recovery_banner_()` short-circuits (no-op) when `encryption_setup_shown_` is true, retiring the `RecoveryBanner` for the `Incomplete` case. The `RecoveryBanner` widget itself is kept in the codebase for now (it may still be useful as a fallback surface) but is no longer shown for `Incomplete` state.

### New pure-virtual on ShellBase

```cpp
virtual void show_encryption_setup_overlay_(EncryptionSetupOverlay::Mode mode) = 0;
```

### Progress forwarding

`ShellBase::handle_enable_recovery_progress_ui_()` overrides the `EventHandlerBase` virtual and calls `encryption_setup_overlay_->advance_progress(step, recovery_key, backed_up, total)` through the stored overlay pointer. This is a `protected` method on ShellBase, not a per-shell virtual.

---

## Section 3 — `EncryptionSetupOverlay` Widget

**Files:** `ui/shared/views/EncryptionSetupOverlay.h`, `ui/shared/views/EncryptionSetupOverlay.cpp`

### State machine

```
Fresh mode:
  Intro → ChooseMethod → Progress → ShowKey → Done
                                  ↗ (error) → ChooseMethod (with error label)

Recover mode:
  Intro → EnterKey → Progress → Done
        ↘ (use another device) → [dismissed, on_request_sas_ fires]
                               ↗ (error) → EnterKey (with error label)
```

### Steps

**Intro**
- Title: "Secure your messages" (Fresh) / "Verify this device" (Recover)
- Two-line body explaining what will happen
- Primary button: "Set up encryption" / "Verify"
- Secondary link: "Skip for now"
- Backdrop click → Skip

**ChooseMethod** (Fresh only)
- Toggle: "Recovery key" (default) / "Passphrase"
- Passphrase option reveals a `NativeTextField` (password echo)
- Continue button
- Back link → Intro

**EnterKey** (Recover only)
- Label: "Enter your recovery key or passphrase"
- `NativeTextField` (single-line, not password echo — key is visible for ease of entry)
- "Verify with another device instead" link → fires `on_request_sas_`, dismisses overlay
- Verify button (disabled when field empty)
- Inline error label (shown on bad key)

**Progress**
- Spinner
- Status label updated from `advance_progress()` calls
- Backdrop click does **not** dismiss (operation in flight)
- No buttons

**ShowKey** (Fresh only)
- Label: "Save your recovery key"
- Monospace display box containing `recovery_key_` string
- "Copy" button (copies to clipboard via host hook)
- "I've saved this key" checkbox — Continue button disabled until checked
- If passphrase mode was used, this step is skipped (go directly to Done)

**Done**
- Checkmark icon
- "Encryption is set up. Your messages are protected." / "Device verified."
- Close button → fires `on_close_`

### Key members

```cpp
enum class Mode  { Fresh, Recover };
enum class Step  { Intro, ChooseMethod, EnterKey, Progress, ShowKey, Done };

Mode mode_;
Step step_;
std::string recovery_key_;       // received in advance_progress(Done)
std::string error_msg_;
uint32_t backed_up_ = 0;
uint32_t total_     = 0;
bool key_saved_checked_ = false;
bool passphrase_mode_   = false; // ChooseMethod selection

// Callbacks wired by ShellBase
std::function<void()>              on_close_;
std::function<void(std::string)>   on_enable_recovery_;  // passphrase or ""
std::function<void(std::string)>   on_recover_;          // key or passphrase
std::function<void()>              on_request_sas_;
std::function<void(std::string)>   on_copy_to_clipboard_;

// Host hooks for NativeTextField overlay positioning
std::function<tk::Rect()>      passphrase_field_rect_;
std::function<bool()>          passphrase_field_visible_;
std::function<tk::Rect()>      key_field_rect_;
std::function<bool()>          key_field_visible_;
std::function<std::string()>   get_passphrase_;
std::function<std::string()>   get_key_input_;
```

### Visual layout

- Full-surface dim backdrop: `rgba(0, 0, 0, 0.5)`
- Centered card: 480 px wide, height adapts per step (200–320 px)
- Backdrop click dismisses (Skip) except during Progress step
- Same dimming/card pattern as `ImageViewerOverlay`
- Card paints over the room list and timeline; does not interact with them

### ShellBase callback wiring

| Callback | ShellBase action |
|----------|-----------------|
| `on_enable_recovery_(passphrase)` | `run_async_mut_([passphrase]{ client_->enable_recovery(passphrase); })` |
| `on_recover_(key)` | `run_async_mut_([key]{ client_->recover(key); })` |
| `on_request_sas_()` | `run_async_mut_([]{client_->request_self_verification();})` then set `encryption_setup_dismissed_=true` |
| `on_close_()` | Set `encryption_setup_dismissed_=true`, call platform shell to unmount overlay |

---

## Section 4 — Platform Shell Wiring

Each shell stores the overlay as:
```cpp
std::unique_ptr<EncryptionSetupOverlay> encryption_setup_overlay_;
```

`show_encryption_setup_overlay_(Mode mode)` per shell:
1. Constructs `EncryptionSetupOverlay(mode)`.
2. Wires all callbacks (on_close_, on_enable_recovery_, on_recover_, on_request_sas_, on_copy_to_clipboard_, NativeTextField host hooks).
3. Mounts on the main surface via the existing overlay mechanism.
4. Stores in `encryption_setup_overlay_`.

`on_close_` implementation: null out `encryption_setup_overlay_`, unmount surface overlay, set `encryption_setup_dismissed_ = true` on the current account session.

**Qt6** — `NativeTextField` backed by `QLineEdit`; passphrase field uses password echo mode. Overlay mounted via `main_app_surface_->set_overlay(encryption_setup_overlay_.get())`.

**GTK4** — `NativeTextField` backed by `GtkEntry`. Overlay mounted via the existing canvas-overlay mechanism.

**Win32** — `NativeTextField` backed by a child `Edit` HWND. Overlay mounted as a paint-layer child of the main Surface.

**macOS** — `NativeTextField` backed by `NSTextField`. `MacShell::show_encryption_setup_overlay_` constructs and mounts via `[mainAppSurface_ setOverlay:...]`.

---

## Section 5 — Testing

### `tests/cpp/test_encryption_setup_overlay.cpp`

Widget-level tests via `TkTestSurface`. Coverage:

- Fresh mode: full step sequence Intro → ChooseMethod → Progress → ShowKey → Done
- Fresh mode passphrase: ChooseMethod (passphrase) → Progress → Done (ShowKey skipped)
- Recover mode: full sequence Intro → EnterKey → Progress → Done
- SAS hand-off: Intro → EnterKey → "another device" link → `on_request_sas_` fired, overlay in terminal state
- ShowKey: Continue disabled until checkbox checked
- Progress: backdrop click ignored; all other steps dismiss on backdrop click
- `advance_progress()` sequence drives labels and transitions correctly
- Error in Progress (step=5): returns to ChooseMethod / EnterKey with `error_msg_` set

### `tests/cpp/test_shell_encryption_setup.cpp`

ShellBase-level tests via `TestShell`. Coverage:

- `recovery_state()=Disabled` → `show_encryption_setup_overlay_(Fresh)` called once
- `recovery_state()=Incomplete` → `show_encryption_setup_overlay_(Recover)` called once
- `recovery_state()=Enabled` → overlay never shown
- `recovery_state()=Unknown` → overlay not shown on first tick; shown on second tick once state resolves to Disabled/Incomplete
- `encryption_setup_dismissed_=true` → subsequent rooms-updated ticks do not re-show overlay
- `encryption_setup_shown_=true` → `check_encryption_setup_()` is idempotent

---

## Out of Scope

- Resetting existing cross-signing (`reset_identity()`) — separate feature
- Changing recovery key / rotate key (`reset_key()`) — belongs in Settings > Privacy
- SSSS (Secret Storage) passphrase change — Settings feature
- Server-side key backup progress notification after the wizard closes — handled by existing `on_backup_progress` path
