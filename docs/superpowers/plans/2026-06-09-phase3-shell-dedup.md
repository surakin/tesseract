# Phase 3 — Cross-Shell Dedup Sub-Plan

> Sub-plan of `2026-06-09-prelaunch-remediation.md`, Phase 3. Moves logic duplicated across the four native shells into `ShellBase`, per the repo's "shared vs platform" rule. Branch: `refactor/prelaunch-phase3-shell-dedup` (stacked on Phase 2).

**Goal:** Eliminate ~1,250+ LOC of cross-shell duplication and permanently kill the drift-bug class by hoisting account restore / login / switch / logout / tray / sync-error / oauth-temp-dir / settings-controller / slash-command logic into `ShellBase`, leaving each shell only its genuinely-native bits behind thin virtuals.

**Hard constraint:** Qt6 and GTK4 build+test locally; **macOS and Windows do NOT build here.** Every extraction follows the same recipe and ends with an explicit "REQUIRES on-platform macOS+Windows compile" flag.

## Per-extraction recipe (applies to every task below)

1. **Map:** read the four shell implementations of the target function side by side. Identify (a) the platform-agnostic core (operates on `ShellBase` protected state) and (b) the genuinely-native bits (status-bar text, native widgets, notifier/UP-connector construction, tray push).
2. **Add to ShellBase:** a concrete method holding the agnostic core, calling new thin `virtual` hooks for the native bits. Give the hooks sensible names and document them.
3. **Convert Qt** (`ui/linux-qt/src/MainWindow.cpp`) to call the shared method + implement the thin virtuals. Build + test. This proves the shared code.
4. **Convert GTK** (`ui/linux-gtk/src/MainWindow.cpp`). Build + test.
5. **Convert Windows + macOS** statically (mirror Qt/GTK), updating their headers/`using` decls. Cannot build — flag.
6. **Delete** the now-dead shell copies. Confirm no orphan references (grep).
7. Verify Qt+GTK build + `ctest` green; commit; flag on-platform.

Order is **lowest-risk first** to validate the pattern before the interrelated account-lifecycle family.

## Tasks

### 3.5 — `build_tray_items_()` (START HERE; trivial, byte-identical)
The tray-rebuild aggregation loop over `account_manager_.all_windows()` is byte-identical in all four shells (qt `:5400`, gtk `:6755`, win `:7494`, macos `:7246`); only the final `tray_->rebuild_menu(items)` is native. Move the loop to `ShellBase::build_tray_items_()` returning the vector; keep a 1-line virtual `push_tray_menu_(items)` (or call the existing native tray directly from each shell's thin `rebuild_tray_`). ~22 LOC/shell.

### 3.9 — `dispatch_room_send_()` (well-contained)
The slash-command ladder (`/myroomavatar`, `/leave`, `/join`, `/invite` → else compose-send) is duplicated 6×: `RoomWindowBase.cpp:785,823` + each shell compose handler (qt `:653`, gtk `:1134`, win `:1595`, macos `:2429`). Hoist into `ShellBase::dispatch_room_send_(room_id, body, formatted)`; have the shell compose paths and `RoomWindowBase` both call it. Also collapses the two `RoomWindowBase::send_message_` overloads.

### 3.7 — `arm_pending_login_(Client*)`
The `set_on_begin_oauth` lambda (compute `pending-<ms>` dir → `create_directories` → `set_data_dir(.../matrix-store)`) repeats ~14× across shells. One `ShellBase::arm_pending_login_(Client*)` with a virtual `now_ms_()` (or pass a timestamp). Also fold the duplicated "no accounts → reset to LoginView" block into a `show_initial_login_view_(restore_error)` helper.

### 3.8 — `ensure_settings_controller_()`
`SettingsController` is constructed + wired in two sites per shell with identical callbacks (`post_to_ui_`, `run_async_`, `pick_image_file_`); only the native avatar picker differs. One `ShellBase::ensure_settings_controller_()` + a virtual for the native settings-widget binding.

### 3.6 — `handle_sync_error_impl_()`
The reconnect / soft-logout / relogin state machine (qt `:4238`, win `:581`, macos `~6900`). Hoist; fixes the macOS stale-display-name drift (macOS skips the display-name/avatar refresh + active-account re-bind that Qt does).

### Account-lifecycle family (do together, most value, highest risk)

#### 3.1 — `restore_all_accounts_()` + `virtual make_notifier_(uid)`
The startup restore loop (`migrate_legacy_layout` → `load_index` → per-uid `load_account`/`restore_session`/set-display+avatar+prefs/`start_sync`/`add_account` → empty fallback) is identical except per-account notifier/UP-connector construction. Hoist; add `virtual std::unique_ptr<...> make_notifier_(uid, on_click)` and `virtual ... make_up_connector_(uid)` for the native bits. Interacts with the Phase-1 `is_secondary_window_startup_()` gate and `finish_login_ui_` — preserve those. ~80–110 LOC/shell.

#### 3.2 — `finalize_login_(uid)`
The `on_login_succeeded` add-account core (reject-if-`find`, `export_session`, temp→final dir rename w/ copy fallback, `save_account`, reopen store, `restore_session`, set display/avatar/prefs, `start_sync`, `add_account`, update index). ~70 LOC/shell.

#### 3.3 — `switch_active_account_impl_()`
The 100+ LOC of agnostic bookkeeping (clear tabs/space_stack/pagination/reply-cache, swap per-account room/invite snapshots, rotate pending-restore, banner save/restore, index persist) with thin native virtuals (`populate_user_strip`/`refresh_room_list`/`show_room`). **Subsumes Phase-1 Task 1.2** (the Windows unsubscribe) — fold the unsubscribe guard into the shared impl so all shells get it. ~90 LOC/shell.

#### 3.4 — `logout_active_account_impl_()`
Identical teardown (up_connector logout, presence logout, client logout/stop_sync, `clear_account`, erase per-account caches, `remove_account`, reset visible state, branch to login-view-or-switch). Fixes the logout drift (qt skips unsubscribe + ignores logout result; gtk does both differently). Builds on the Phase-1 liveness/pool-drain work. ~70 LOC/shell.

## Verification (every task)
- `cmake --build build/linux-qt6-debug && cmake --build build/linux-gtk-debug`
- `ctest --test-dir build/linux-qt6-debug --output-on-failure` (baseline 717)
- Commit per extraction; **flag on-platform macOS+Windows compile + smoke** in every commit that touches those shells.

## Final gate
After all extractions: full build/test, then on-platform macOS+Windows compile + smoke of account add / switch / logout / tray / sync-recovery before merge.
