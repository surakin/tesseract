# Pre-Launch Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Address every finding in `docs/CODE_REVIEW_2026-06-09.md` — correctness bugs, lifetime/threading hazards, security/robustness gaps, cross-shell and shared-layer duplication, and dead code — in a risk-ordered sequence that keeps `main` shippable at every step.

**Architecture:** Six phases, ordered by risk and dependency. Phases 0–2 are concrete and fully specified here (cleanup, launch-blocking correctness/safety, robustness). Phases 3–5 are large structural refactors; each is scoped here with exact targets and verification, but **each gets its own detailed sub-plan authored at execution start** (its edits depend on the code state after earlier phases). Phase 6 is final verification.

**Tech Stack:** Rust (`matrix-sdk`, `cxx`, `tokio`, `parking_lot`) in `sdk/`; C++20 in `client/` and `ui/`; Catch2 + ctest for C++ tests; `cargo test` for Rust. Build presets: `linux-qt6-debug`, `linux-gtk-debug`.

**Standing rules for every task:**
- Branch off `main` (never commit to `main` directly): `git checkout -b fix/<task-slug>`.
- After each task: build the affected target(s), run the relevant tests, and **only commit after the user confirms** (repo workflow: never commit/push before user tests). End commit messages with the `Co-Authored-By` trailer.
- Behavior-preserving refactors are verified by the **existing** test suite passing plus a manual smoke run; new behavior gets a new test first (TDD).
- Standard verification commands:
  - Qt: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure`
  - GTK: `cmake --build build/linux-gtk-debug`
  - Rust: `cargo test -p tesseract-sdk-ffi` and `cargo clippy -p tesseract-sdk-ffi`
- macOS/Windows are not locally buildable here — changes to those shells must be flagged for testing on-platform.

---

## Phase ordering at a glance

| Phase | Theme | Risk | Gate |
|---|---|---|---|
| 0 | Safe cleanup (dead code, clippy, doc fixes) | Very low | Builds + tests green |
| 1 | Launch-blocking correctness & safety | Medium | Before public launch |
| 2 | Robustness hardening | Low–Medium | Before/just after launch |
| 3 | Cross-shell dedup (UI → shared) | Medium | Own sub-plan |
| 4 | Shared-layer dedup (tk/views/SDK) | Medium | Own sub-plan(s) |
| 5 | God-object decomposition | High effort | Own sub-plan(s), post-launch |
| 6 | Final verification sweep | — | Pre-tag |

Phases 0→2 are strictly ordered. Phases 3–5 can be scheduled independently after Phase 2, but **Phase 1 deliberately makes tactical fixes that Phases 3–4 later supersede** (e.g. per-host popup routing → `HostInputCore`); that is intentional so launch isn't blocked on big refactors.

---

# Phase 0 — Safe cleanup (dead code, clippy, doc fixes)

Do first: removes ~dead surface so later phases have less to read, and every task is independently revertible. Group as a few commits, not 20.

### Task 0.1: Delete dead Rust shim + clear clippy backlog

**Files:**
- Modify: `sdk/src/client/send.rs:10,33-34` (remove `build_uia_fallback_url` re-export alias + its `#[allow(unused_imports)]`)
- Modify: `sdk/src/client/media.rs` (7 dead `OwnedMxcUri::into()` calls flagged by clippy)

- [ ] **Step 1:** Run `cargo clippy -p tesseract-sdk-ffi 2>&1 | tee /tmp/clippy-before.txt` and confirm the 66-warning baseline.
- [ ] **Step 2:** Delete the `use build_uia_fallback_url as _unused_uia;` line and its `#[allow(unused_imports)]` in `send.rs`. Verify the real callers (`account.rs:12`, `mod.rs`) still import from `super` directly.
- [ ] **Step 3:** Run `cargo clippy --fix -p tesseract-sdk-ffi --allow-dirty` for the 23 auto-fixable lints. Review the diff (especially the `media.rs` `.into()` removals).
- [ ] **Step 4:** Run `cargo test -p tesseract-sdk-ffi` — Expected: all pass (204+).
- [ ] **Step 5:** Commit `chore(sdk): remove dead re-export shim, clear auto-fixable clippy lints`.

### Task 0.2: Delete dead C++ public API in client

**Files:**
- Modify: `client/include/tesseract/client.h` + `client/src/client.cpp` — remove blocking `get_url_preview` (`client.cpp:1480`), `paginate_back` (`:267`), `fetch_media_thumbnail_bytes`/`fetch_source_thumbnail_bytes`/`fetch_avatar_thumbnail_bytes` (`:890-902`), `list_rooms` (`:222`), and the now-unused `#include <cassert>` (`:9`).
- Modify: corresponding `extern "Rust"` bindings in `sdk/src/lib.rs` / `sdk/src/bridge.rs` **only if** nothing else references them (grep first).
- Modify: `client/include/tesseract/session_store.h` + `client/src/session_store.cpp:254-293` — remove legacy `SessionStore::load/save/clear`.
- Modify: `tests/cpp/test_paths.cpp` — rewrite the one test that uses `SessionStore::path()` against `account_dir` instead.

- [ ] **Step 1:** For each symbol, `grep -rn "<symbol>" ui/ client/ tests/ sdk/` to confirm zero production callers. Keep `parse_url_preview`, `fetch_source_bytes`, `fetch_url_bytes`, `discover_homeserver`, `media_upload_limit` — they ARE live.
- [ ] **Step 2:** Remove the dead declarations + definitions + their FFI bindings.
- [ ] **Step 3:** Rewrite `test_paths.cpp` to not depend on `SessionStore::path()`.
- [ ] **Step 4:** `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure` — Expected: green.
- [ ] **Step 5:** Check the Windows-only `fetch_avatar_bytes`/`fetch_media_bytes` (`client.cpp:865-875`): do NOT delete (still used by `ui/windows`). Instead add a `// Windows-only: GTK/Qt/macOS use fetch_media_async` comment so they don't read as dead cross-platform API.
- [ ] **Step 6:** Commit `refactor(client): remove dead public API (blocking previews/pagination/thumbnails, legacy SessionStore)`.

### Task 0.3: Delete dead code in tk/, views/, app/

**Files (each independently verifiable; one commit):**
- `ui/shared/tk/widget.h:296-320` — delete `class FillBackground` (zero refs).
- `ui/shared/tk/audio_gtk.cpp:336` — delete unused `GstElement* pipeline_`.
- `ui/shared/views/UserInfo.{h,cpp}` — delete `set_avatar_size`, `set_show_user_id` (no call sites).
- `ui/shared/views/LoginView.{h,cpp}` — delete `set_homeserver_label` (no call sites).
- `ui/shared/views/EncryptionSetupOverlay.{h,cpp}` — delete `set_passphrase_input`/`set_key_input` + the `passphrase_input_`/`key_input_` members and their dead fallback branches (`.cpp:157,167,446`); the getters are always wired (`ShellBase.cpp:4471`).
- `ui/shared/app/ShellBase.{h,cpp}` — delete `shell_sticker_no_fetch_` (`:641`), the `on_media_preview_config_applied_` virtual (no overrides; inline its one call site `:1705` or drop), and the write-only `typing_bar_visible_` field (`:3200`).
- `ui/shared/app/RoomWindowBase.{h,cpp}` — delete write-only `typing_bar_visible_` (`:709`) and never-used `compose_typing_active_`.
- `ui/shared/app/ShellBase.h:1046-1057` — change the five never-overridden `apply_thread_*` hooks from `virtual` to plain members.

- [ ] **Step 1:** For each symbol, `grep -rn` across `ui/ tests/` to re-confirm zero callers/overrides before deleting (some are kept alive only by tests — `ToggleButton`, `reset_near_bottom_latch`; for those, see Step 4).
- [ ] **Step 2:** Delete the confirmed-dead members listed above.
- [ ] **Step 3:** `cmake --build build/linux-qt6-debug && cmake --build build/linux-gtk-debug` — Expected: clean.
- [ ] **Step 4:** For `ToggleButton` (`controls.h:268`) and `ListView::reset_near_bottom_latch` (`list_view.h:249`) — alive only via `tests/cpp/`: decide per-symbol with the user (delete both prod + test, or keep). Default: delete both since no production view uses them. Update `test_tk_widgets.cpp`/`test_tk_lists.cpp` accordingly.
- [ ] **Step 5:** `ctest --test-dir build/linux-qt6-debug --output-on-failure` — Expected: green (test count drops by the removed cases).
- [ ] **Step 6:** Commit `refactor(ui): remove dead widgets, setters, fields, and never-overridden virtuals`.

### Task 0.4: Fix stale docs

**Files:** `CLAUDE.md` (the `ui/shared/views/` list), and add a one-line note where helpful.

- [ ] **Step 1:** In `CLAUDE.md`, correct the architecture list: `markdown (Markdown→HTML)` lives in `client/src/markdown.cpp` (Rust-backed), not `ui/shared/views/`. Only `html_spans` (HTML→TextSpan) is in views.
- [ ] **Step 2:** Commit `docs: correct markdown location in CLAUDE.md architecture list`.

---

# Phase 1 — Launch-blocking correctness & safety

These are crashes, UAF, races, a security exposure, and a broken cross-platform feature. Each is independently shippable. Highest priority.

### Task 1.1: Route Windows & macOS through the shared timeline/message handlers

**Why:** The Windows/macOS overrides reimplement `ShellBase` logic and drop guards Qt/GTK keep — causing (a) in-thread replies leaking into the main timeline and (b) lost scroll/focus restore on timeline reset. Deleting the overrides fixes both and removes ~180 LOC.

**Files:**
- Modify: `ui/windows/src/MainWindow.h:127-136` + `ui/windows/src/MainWindow.cpp:550,4356,4393,4424,4455` — remove the overrides of `handle_timeline_reset_ui_`, `handle_message_inserted_ui_`, `handle_message_updated_ui_`, `handle_message_removed_ui_`.
- Modify: `ui/macos/src/MainWindowController.mm:139-148,1305,1326,1343,1360,6913` — same removals.
- Reference (do not modify): the base implementations `ui/shared/app/ShellBase.cpp:2932,2982,3012,3075` that Qt/GTK already use.

- [ ] **Step 1 (understand):** Read the base handlers and the Qt overrides-of-nothing (Qt relies on base). Enumerate every thin UI virtual the base calls (e.g. `insert_message_row_ui_`, `set_messages_ui_`, scroll/focus virtuals) and confirm Windows & macOS already implement those thin virtuals (they must, since the base path needs them). List any the override used but the base path doesn't call.
- [ ] **Step 2 (TDD where possible):** In `tests/cpp/`, add/extend a shell test asserting that an inserted event with a non-empty `thread_root_id` does NOT appear in the main timeline rows (mirror the existing shared-handler behavior covered for Qt/GTK). This guards the regression for all shells using the base.
- [ ] **Step 3:** Delete the four Windows overrides and the four macOS overrides (and their header decls).
- [ ] **Step 4 (Windows):** `cmake --build build/<win preset>` is not available locally — flag for on-platform build. Statically verify the thin virtuals the base needs all exist in `MainWindow.cpp`.
- [ ] **Step 5 (Qt regression):** `ctest --test-dir build/linux-qt6-debug --output-on-failure` — Expected: green incl. the new test.
- [ ] **Step 6:** Commit `fix(win,macos): delete redundant timeline-handler overrides; use shared ShellBase path (fixes in-thread leak + scroll restore)`. Flag for Windows/macOS runtime testing.

### Task 1.2: Fix Windows account-switch room-subscription leak

**Why:** Qt/GTK/macOS unsubscribe the previous account's open room before swapping; Windows clears `current_room_id_` without unsubscribing, so the old account keeps streaming.

**Files:** Modify `ui/windows/src/MainWindow.cpp` around `switch_active_account` (`:5702`, clears `current_room_id_` at `:5748`).
**Reference:** Qt `ui/linux-qt/src/MainWindow.cpp:4569-4573` for the exact guard.

- [ ] **Step 1:** Before clearing `current_room_id_`, add the same guard Qt uses: if `client_ && !current_room_id_.empty() && room_subscription_refs_.count(current_room_id_) == 0`, call `client_->unsubscribe_room(current_room_id_)`.
- [ ] **Step 2:** Static-verify against the Qt version that the surrounding state ordering matches.
- [ ] **Step 3:** Commit `fix(win): unsubscribe previous account's room on account switch`. Flag for Windows runtime testing.

> Note: Tasks 1.1 and 1.2 are tactical fixes that Phase 3 (moving `switch_active_account` into `ShellBase`) will later subsume. Doing them now de-risks launch without waiting for the refactor.

### Task 1.3: Add a liveness token to ShellBase for UI-thread continuations + drain pools on logout

**Why:** Worker→UI continuations (`run_async_([this]{… post_to_ui_([this]{…}); })`, pervasive in `ShellBase.cpp`) capture raw `this`. Safe for the primary window but a **UAF for spawned/secondary windows** (the new multi-window feature) and account-switch/logout teardown. Separately, logout frees the `Client` while worker lambdas may still hold `Client*`.

**Files:**
- Modify: `ui/shared/app/ShellBase.h` — add `protected: std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);` (mirror `RoomWindowBase.h:254`), set `*alive_ = false;` in `~ShellBase`.
- Modify: `ui/shared/app/ShellBase.cpp` — wrap the `post_to_ui_` continuations so the UI-thread body early-returns if `!*alive`. Provide a helper `post_to_ui_guarded_(fn)` that captures `auto a = alive_;` and checks it.
- Modify: per-shell logout paths — `ui/linux-qt/src/MainWindow.cpp:4754-4767` and peers (gtk `:6237`, win, macos `:6509`) — drain/quiesce `pool_`/`mut_pool_` (or ensure in-flight lambdas hold a `shared_ptr<AccountSession>`) **before** `remove_account` drops the last reference.

- [ ] **Step 1 (design):** Decide the mechanism with the user: (A) `alive_` token guarding continuations + (B) worker lambdas capture `shared_ptr<AccountSession>` (keeping the `Client` alive until the lambda finishes) instead of raw `Client*`. Recommend both: (A) for ShellBase-member access, (B) for `Client` access. **Offer breakpoints here** (this is a threading bug — let the user inspect).
- [ ] **Step 2 (TDD):** Add a `tests/cpp/` test that constructs a `ShellBase` subclass stub, posts a guarded continuation, destroys the shell, then pumps the UI queue and asserts the continuation body did not run (and did not crash). Use the existing test harness pattern from `test_shell_*.cpp`.
- [ ] **Step 3:** Implement `alive_` + `post_to_ui_guarded_`; convert the highest-risk continuations first (media completions `:3134`, account-switch, secondary-window paths).
- [ ] **Step 4:** Implement the logout pool-drain (or session-shared_ptr capture) in each shell's logout path.
- [ ] **Step 5:** `ctest --test-dir build/linux-qt6-debug --output-on-failure` — Expected: green incl. the new test. Manually smoke: open a secondary window, close it while media is loading; switch/logout an account mid-fetch.
- [ ] **Step 6:** Commit `fix(shell): guard UI-thread continuations with a liveness token; drain worker pools before destroying Client on logout`.

### Task 1.4: Cap the notification avatar download (Rust)

**Why:** `sdk/src/client/media.rs:165-169` fetches the sender avatar uncapped — a remote-driven unbounded allocation on a sync worker.

**Files:** Modify `sdk/src/client/media.rs:165-169`.

- [ ] **Step 1 (TDD):** Add a unit test in `media.rs` (or its test module) asserting the avatar-fetch path applies the same cap as the preview path (e.g. assert bytes are truncated/rejected past `NOTIF_IMAGE_CAP`). If the network call can't be unit-tested directly, test the `cap_media_bytes` wrapper is invoked by refactoring the fetch behind a capped helper and testing the helper.
- [ ] **Step 2:** Wrap the `get_media_content(...).await.unwrap_or_default()` in `cap_media_bytes(...)` (or `NOTIF_IMAGE_CAP`), matching the sibling preview fetch at `:90-93`.
- [ ] **Step 3:** `cargo test -p tesseract-sdk-ffi` — Expected: green.
- [ ] **Step 4:** Commit `fix(sdk): cap notification sender-avatar download (was unbounded)`.

### Task 1.5: Stop startup crash on a wrong-typed settings field

**Why:** `client/src/settings.cpp:20-27` catches only `json::parse_error`; a wrong field type throws `json::type_error`, escaping `load_from_disk` → startup crash.

**Files:** Modify `client/src/settings.cpp:20-27`.

- [ ] **Step 1 (TDD):** Add `tests/cpp/test_settings.cpp` (or extend existing) writing an `app_settings.json` with a wrong-typed field (e.g. `"notifications_enabled": "yes"`) and asserting `Settings::load_from_disk` returns defaults rather than throwing.
- [ ] **Step 2:** Run it — Expected: FAIL (throws).
- [ ] **Step 3:** Widen the catch to `catch (const nlohmann::json::exception&)` (the base of both parse_error and type_error).
- [ ] **Step 4:** Run it — Expected: PASS. Then full `ctest`.
- [ ] **Step 5:** Commit `fix(client): survive wrong-typed fields in app_settings.json`.

### Task 1.6: Replace poisoning locks with `parking_lot` across the SDK

**Why:** 20 `.read()/.write()/.lock().unwrap()` sites; a panic while holding one poisons it → every later FFI call panics → UB unwinding across cxx.

**Files:** Modify `sdk/src/client/timeline.rs` (×8), `mod.rs` (×4), `send.rs` (×3), `session.rs`, `backfill.rs`, `notifications.rs`; `sdk/Cargo.toml` (add `parking_lot` if not present).

- [ ] **Step 1:** Add `parking_lot` to `sdk/Cargo.toml`. Replace `std::sync::{Mutex,RwLock}` with `parking_lot::{Mutex,RwLock}` on the affected fields; drop the `.unwrap()` (parking_lot guards don't return `Result`).
- [ ] **Step 2:** Mechanical: each `x.lock().unwrap()` → `x.lock()`, `.read().unwrap()` → `.read()`, `.write().unwrap()` → `.write()`. Watch for any code depending on poison semantics (none expected).
- [ ] **Step 3:** `cargo build -p tesseract-sdk-ffi` then `cargo test -p tesseract-sdk-ffi` — Expected: green.
- [ ] **Step 4:** Commit `fix(sdk): use parking_lot locks to avoid poison-panic cascades across the FFI boundary`.

### Task 1.7: Tactical popup input/hover routing for non-Qt hosts

**Why:** Popup click/hover routing + outside-click dismiss exists only in the Qt host, so `ComboBox` dropdowns are broken on macOS/Win32/GTK.

**Files:**
- Modify: `ui/shared/tk/host_gtk.cpp` (`on_pointer_down`/`move` near `:1446-1480`), `ui/shared/tk/host_win32.cpp` (`:3618-3645`), `ui/shared/tk/host_macos.mm` (`:1692-1717`).
- Reference: the Qt implementation `ui/shared/tk/host_qt.cpp:1133-1147,1193-1214`.

- [ ] **Step 1:** Port the Qt popup branch into each of the three hosts: in `on_pointer_down`, if `popup_` is set, route the press into it when inside its bounds, else call `on_popup_dismiss()` + clear `popup_`; in `on_pointer_move`, clear button hover and route move into an open `popup_`.
- [ ] **Step 2 (GTK build):** `cmake --build build/linux-gtk-debug` — Expected: clean; manually verify a `ComboBox` dropdown (RoomInfoPanel) dismisses on outside-click and highlights on hover.
- [ ] **Step 3:** Win32/macOS — static-port the same branch; flag for on-platform testing.
- [ ] **Step 4:** Commit `fix(tk): route popup clicks/hover and outside-click dismiss in GTK/Win32/macOS hosts`.

> Note: Task 4.1 (`HostInputCore`) will later delete these three copies and the Qt one in favor of a single shared implementation. The tactical port unblocks launch.

### Task 1.8: Investigate & fix the `clear_all_caches_` cross-window data race

**Why:** `ShellBase.cpp:3885` clears the shared `media_disk_cache()` from a worker thread while other windows' UI threads may `load/store` on the same object.

**Files:** Read `ui/shared/tk/media_disk_cache.{h,cpp}`, `tk/pixmap_cache.h`, `tk/anim_image_cache.h`; modify `ui/shared/app/ShellBase.cpp:3872-3909` if needed.

- [ ] **Step 1 (investigate):** Determine whether `MediaDiskCache`/`PixmapCache`/`AnimImageCache` are internally synchronized (mutex-guarded). Report findings. **Offer breakpoints.**
- [ ] **Step 2:** If NOT internally synchronized: move the disk-cache `clear()` off the worker and onto the UI thread (like the in-memory clears already are at `:3893`), or add a mutex to `MediaDiskCache`. Choose with the user.
- [ ] **Step 3:** Build + smoke (clear cache from Settings while a second window loads media).
- [ ] **Step 4:** Commit `fix(shell): avoid data race clearing shared media caches across windows`.

---

# Phase 2 — Robustness hardening

Lower urgency than Phase 1; safe to land before or just after launch. Each independent.

### Task 2.1: Don't block the sync worker on keychain + fsync in `persist_session`

**Files:** `client/src/event_handler_bridge.cpp:284-297`.
- [ ] **Step 1:** Confirm `persist_session` runs on a matrix-sdk worker thread and calls `SessionStore::save_account` (keychain + fsync) synchronously.
- [ ] **Step 2:** Offload the disk/keychain write to a dedicated single-thread queue (or the existing io pool) so the sync callback returns promptly; ensure ordering (last-write-wins per account).
- [ ] **Step 3:** Build + test; smoke a token refresh.
- [ ] **Step 4:** Commit `fix(client): offload session persistence off the sync callback thread`.

### Task 2.2: Replace `expect()` in the OAuth path with `?`

**Files:** `sdk/src/oauth.rs:133,176`.
- [ ] **Step 1:** Convert both `Url::parse(...).expect(...)` to `?` with `anyhow::Context` (the fn returns `anyhow::Result`).
- [ ] **Step 2:** `cargo test -p tesseract-sdk-ffi`; commit `fix(sdk): return errors instead of panicking on URL parse in oauth`.

### Task 2.3: Harden `html_spans` parsing

**Files:** `ui/shared/views/html_spans.cpp` (`decode_entity` `:50-124`; formatting stack `:685-774`).
- [ ] **Step 1 (TDD):** Add `tests/cpp/test_html_spans.cpp` cases: (a) `&#xD800;` / `&#0;` / a C0 control → produces U+FFFD (valid UTF-8), not invalid bytes; (b) deeply nested formatting past `kMaxTagDepth` then closing tags does not corrupt outer-frame styling.
- [ ] **Step 2:** Run — Expected: FAIL.
- [ ] **Step 3:** In `append_codepoint`/`decode_entity`, reject `cp` in `[0xD800,0xDFFF]`, `cp == 0`, `cp > 0x10FFFF` → emit U+FFFD. In the formatting stack, track a "dropped opens" counter and swallow that many closes before popping real frames.
- [ ] **Step 4:** Run — Expected: PASS; full `ctest`.
- [ ] **Step 5:** Commit `fix(views): clamp invalid numeric HTML entities; fix formatting-stack underflow under depth cap`.

### Task 2.4: Validate the accounts index on load

**Files:** `client/src/session_store.cpp:344-369` (`load_index`/`serialize_index`).
- [ ] **Step 1 (TDD):** Test that a malformed/truncated `accounts.json` is distinguishable from "no accounts" (returns an error/empty-with-flag rather than silently dropping all accounts).
- [ ] **Step 2:** Parse the index with `nlohmann/json` with an explicit parse-failure signal; on failure, do not overwrite the existing index and surface the error.
- [ ] **Step 3:** Build + test; commit `fix(client): detect corrupt accounts.json instead of silently dropping accounts`.

### Task 2.5: Surface failed room actions to the user

**Files:** `ui/shared/app/ShellBase.cpp:2359,2373-2397` (join/leave/upload error paths currently `fprintf(stderr)`).
- [ ] **Step 1:** Route these failures through `show_status_message_` (or the existing status mechanism) instead of stderr. For the `Leave` branch (`:2373`), route the room-state teardown through the shared switch helper so thread-panel/media state is cleaned up.
- [ ] **Step 2:** Build + smoke a failed join/leave; commit `fix(shell): show room-action failures in the UI instead of stderr`.

### Task 2.6: Review Windows credential persistence scope

**Files:** `client/src/secret_store_windows.cpp:49` vs `secret_store_macos.cpp:183`.
- [ ] **Step 1 (investigate):** Confirm whether `CRED_PERSIST_LOCAL_MACHINE` matches the macOS `…ThisDeviceOnly, AfterFirstUnlock` intent for OAuth tokens. Document the decision.
- [ ] **Step 2:** If the scope is broader than intended, switch to the appropriate per-user persistence; otherwise add a comment documenting the deliberate platform difference. Flag for Windows testing.
- [ ] **Step 3:** Commit `chore(client): document/align Windows credential persistence scope`.

### Task 2.7: Audit & document the unlocked `&self` FFI-read invariant

**Files:** `client/src/client.cpp` (read methods that skip `ffi_mu`, e.g. `export_session` `:170`).
- [ ] **Step 1 (investigate):** Enumerate every read method that calls `impl_->ffi->…` without `MUT_FFI`. For each, confirm the Rust side is `&self`.
- [ ] **Step 2:** Decide with the user: (A) add a naming/enforcement convention on the Rust side, or (B) take the lock unconditionally and benchmark contention. Recommend (B) unless a measured hot path objects.
- [ ] **Step 3:** Implement the chosen option; build + test; commit `fix(client): make FFI read concurrency safety explicit`.

---

# Phase 3 — Cross-shell dedup (move logic UI → shared)

**This phase gets its own detailed sub-plan** authored at execution start (write it with the writing-plans skill, one task per extracted method, TDD against the existing shell behavior). It is the highest-ROI structural work (~1,250+ LOC out of the shells) and permanently kills the drift-bug class. Scope and order:

1. `ShellBase::restore_all_accounts_()` + a `virtual make_notifier_(uid)` hook — replaces the per-shell account-restore loop (qt `MainWindow.cpp:2353`, gtk `:2767`, win `:3462`, macos `:5504`).
2. `ShellBase::finalize_login_(uid)` — replaces the `on_login_succeeded` add-account core (qt `:2564`, gtk `:2898`, win `:3640`, macos `loginSucceeded`).
3. `ShellBase::switch_active_account_impl_()` — replaces `switch_active_account` (qt `:4555`, gtk `:6077`, win `:5702`, macos `:5899`). **Subsumes Task 1.2.**
4. `ShellBase::logout_active_account_impl_()` — replaces `logout_active_account` (qt `:4740`, gtk `:6237`, macos `:6509`); fixes the logout drift (unsubscribe + result handling + tray).
5. `ShellBase::build_tray_items_()` — replaces the byte-identical `rebuild_tray_` loop (qt `:5400`, gtk `:6755`, win `:7494`, macos `:7246`).
6. `ShellBase::handle_sync_error_impl_()` — replaces the reconnect/soft-logout state machine; fixes the macOS stale-display-name drift.
7. `ShellBase::arm_pending_login_(Client*)` — replaces the ~14 copies of the `set_on_begin_oauth` temp-dir lambda.
8. `ShellBase::ensure_settings_controller_()` — replaces the 2-per-shell SettingsController build.
9. `ShellBase::dispatch_room_send_()` — replaces the 6× slash-command ladder (RoomWindowBase `:785/823` + four shells).

**Per-extraction recipe (applies to each):** define the shared method on `ShellBase` taking the platform-agnostic logic, add thin pure-virtual hooks for the genuinely native bits (status bar, native widgets, notifier/UP construction), delete the shell copies one shell at a time, build+test that shell after each. Verify by existing tests + manual smoke per shell. Each extraction is its own commit.

**Dependencies:** Do this after Phase 1 (so the tactical correctness fixes are in) — items 3 and 4 here delete the code Tasks 1.1/1.2 patched.

---

# Phase 4 — Shared-layer dedup (tk / views / SDK)

**Own sub-plan(s).** Behavior-preserving extractions, each verified by existing tests + smoke.

- **4.1 `HostInputCore`** — extract the copy-pasted pointer-dispatch state machine (`host_qt.cpp:1176-1269`, `host_win32.cpp:3651-3709`, `host_macos.mm:1723-1787`, `host_gtk.cpp:~1446-1548`) into a shared core owning `root_` + tracked pointers + `on_pointer_move/up/leave` + popup routing. **Deletes the Task 1.7 tactical copies and the Qt original.** Highest-value tk dedup; also closes the popup gap permanently.
- **4.2 `ScrollableBase`** — hoist `thumb_geom`/`thumb_hit`/`clamp_scroll`/scroll-drag/thumb-paint shared by `ListView` and `GridView` (`list_view.cpp:555-590` vs `1007-1040`).
- **4.3 Shared `font_metrics_for(FontRole)`** + shared `initials_of()` — collapse the 4 per-backend copies (`canvas_cairo/qpainter/cg/d2d`); fold the drifted `0.36`/`0.42` ratio into one constant.
- **4.4 `ListPopupBase<Item>`** — unify `MentionPopup`/`ShortcodePopup`/`SlashCommandPopup` (one `paint_row` virtual).
- **4.5 `TabbedGridPicker`** — unify `EmojiPicker`/`StickerPicker`.
- **4.6 `MediaOverlayBase`** — unify `ImageViewerOverlay`/`VideoViewerOverlay` chrome.
- **4.7 `draw_avatar(...)` helper** in `media_utils` — replace ~10 copy-pasted avatar-or-initials blocks.
- **4.8 SDK `build_attachment_config()` + `do_send_attachment()`** — collapse the 8 media-send paths (`send.rs:727-1481`).
- **4.9 SDK `timeline_convert` partial-struct rewrite** — replace the 29-element positional tuple (`:602-760`) with `TimelineEvent{ …, ..ffi_event_defaults() }`.
- **4.10 client `with_handler()` wrapper** — fold the 36 `guard + slot_->load + null-check` preambles (`event_handler_bridge.cpp`).
- **4.11 client JSON consolidation** — replace the two hand-rolled scanners (`client.cpp:979-1262`, `session_store.cpp:38-124`) with `nlohmann/json` (~350 LOC deleted). Larger; can be its own plan.
- **4.12 ShellBase media-fetch pipeline** — collapse the 4 hand-rolled fetch state machines (`fetch_media_pipeline_:235`, `ensure_viewer_fullres_:472`, `ensure_picker_image_:960`, `ensure_tile_async:1076`) into one templated pipeline. (Natural lead-in to Phase 5's `MediaController`.)

---

# Phase 5 — God-object decomposition (post-launch, deliberate)

**Own sub-plan(s); schedule explicitly.** Large, invasive, behavior-preserving.

- **5.1 Decompose `MessageListView`** (`ui/shared/views/MessageListView.cpp`, 7,235 LOC): extract a `TimelineMediaController` (voice/audio/video handlers `:5073-5435`), pull the room-switch gate state machine out, split `paint_row` (~700 LOC) and the pointer handlers (~480-550 LOC) by row-Kind. Land incrementally behind passing tests.
- **5.2 Decompose `ShellBase`** (`.h` 1,957 + `.cpp` 4,656): extract `MediaController`/`MediaCache` (builds on Task 4.12), the thread-panel state machine, and the secondary-window registry into collaborators (the way `AccountManager` was already split). Audit the ~50-virtual hook surface; make universally-overridden hooks pure-virtual.
- **5.3 Decompose Rust `start_sync`** (`sdk/src/client/sync.rs:28-1079`, ~1,000 LOC, ~11 levels deep): split the VectorDiff dispatch, room-list-update, and notification paths into free functions taking shared state by reference.

---

# Phase 6 — Final verification sweep

- [ ] **Step 1:** Full build of every locally-buildable target: `cmake --build build/linux-qt6-debug && cmake --build build/linux-gtk-debug`.
- [ ] **Step 2:** Full test run: `ctest --test-dir build/linux-qt6-debug --output-on-failure` and `cargo test -p tesseract-sdk-ffi`. Record the new test count.
- [ ] **Step 3:** `cargo clippy -p tesseract-sdk-ffi` — confirm the warning count dropped from the 66 baseline.
- [ ] **Step 4:** Flag the complete set of Windows/macOS-touching changes (Tasks 1.1, 1.2, 1.7, Phase 3 items, 4.1) for on-platform build + smoke testing.
- [ ] **Step 5:** Update `STATUS.md` (root) — refresh test counts + Last updated date (per repo convention for changes of this size).
- [ ] **Step 6:** Run the whole-branch code-review gate before merging each phase (repo workflow).

---

## Self-review notes

- **Coverage:** Every finding in `docs/CODE_REVIEW_2026-06-09.md` maps to a task: launch-blockers → Phase 1; robustness Mediums → Phase 2; cross-shell dedup table → Phase 3; tk/views/SDK dedup → Phase 4; god-objects → Phase 5; dead code + clippy + doc staleness → Phase 0. The unlocked-`&self` and `clear_all_caches_` items are explicitly Tasks 2.7 and 1.8.
- **Sequencing intent:** Phase 1 includes tactical fixes (1.1, 1.2, 1.7) that Phases 3–4 later supersede — intentional, to avoid blocking launch on big refactors. Each superseding phase notes which earlier task it deletes.
- **Honesty on detail:** Phases 0–2 are fully specified and immediately executable. Phases 3–5 are scoped with exact targets + a per-item recipe but deliberately deferred to dedicated sub-plans, because their concrete edits depend on the code state after earlier phases and on per-extraction interface decisions that should be made at execution time, not invented now.
