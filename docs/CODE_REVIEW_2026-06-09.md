# Tesseract Pre-Launch Code Review — 2026-06-09

Full-tree review across all modules (Rust SDK, C++ client library, shared toolkit,
shared views, shared app/shell layer, and the four native UI shells). Conducted by
six parallel review agents, one per module, then synthesized. All findings are
read-only observations with `file:line` references; nothing was modified.

Severity legend: **High** = correctness/safety/launch risk, **Medium** = should fix
before or soon after launch, **Low** = cleanup/hygiene.

---

## Executive summary

The codebase is in good shape overall: the shared layer is real (Qt/GTK genuinely
delegate to it), the FFI surface has no dead exports, and the HTML/markup XSS posture
is solid. The issues cluster into a few **structural themes** rather than scattered
one-offs:

1. **Thin-shell rule is violated at scale.** Account restore/login/switch/logout, tray
   aggregation, and sync-error handling are each reimplemented in all four shells
   (~1,250+ LOC of removable duplication). This is also the *root cause* of theme #2.
2. **Drift bugs from that duplication.** Windows and macOS override the shared
   timeline/message handlers and reintroduce bugs Qt/GTK don't have (in-thread reply
   leak, lost scroll restore, room-subscription leak on account switch). These are
   real correctness bugs, not just smell.
3. **Lifetime/threading safety gaps** around worker→UI callbacks — and these now bite
   harder because of the just-shipped multi-window feature (secondary windows can be
   destroyed while continuations are in flight).
4. **Two god-objects** — `ShellBase` (6.6k LOC) and `MessageListView` (7.2k LOC) — that
   should be decomposed into collaborators.
5. **Duplication inside the shared layer too** — pointer dispatch, scrollable views,
   font mapping, popups, pickers, media overlays.
6. **A cross-platform functional gap** — popup input/hover routing exists only in the
   Qt host, so `ComboBox` dropdowns are effectively broken on macOS/Win32/GTK.
7. **Robustness/security polish** — one uncapped remote-driven download, JSON parsing
   fragility, a startup-crash path, and lock-poison panics across the FFI boundary.

---

## Launch blockers / High-risk (fix before going public)

These are the items where the risk is a crash, UAF, data race, security exposure, or a
user-visible broken feature.

### Correctness — drift bugs (Windows/macOS bypass the shared handlers)
- **In-thread replies leak into the main timeline (Win + macOS).** `ShellBase::handle_message_inserted_ui_`
  guards `in_thread` (`ui/shared/app/ShellBase.cpp:2994,3006`); the Windows override
  (`ui/windows/src/MainWindow.cpp:4404-4421`) and macOS override
  (`ui/macos/src/MainWindowController.mm:1326-1340`) drop that guard and insert thread
  replies into the main list with main-timeline indices. Same gap in the `_updated`
  overrides. **Fix: delete the overrides, route through the base** (Qt/GTK already do).
- **Lost scroll/focus restore on timeline reset (Win + macOS).** Base re-arms deferred
  scroll + restores saved tab offset (`ShellBase.cpp:2948-2975`); Windows
  (`ui/windows/src/MainWindow.cpp:4356-4391`) and macOS (`MainWindowController.mm:6913`)
  overrides omit it → returning to a scrolled-up tab snaps to bottom.
- **Room-subscription leak on account switch (Windows).** Qt/GTK/macOS unsubscribe the
  previous room before swapping (e.g. `ui/linux-qt/src/MainWindow.cpp:4569-4573`);
  Windows `switch_active_account` clears `current_room_id_` at
  `ui/windows/src/MainWindow.cpp:5748` without ever calling `unsubscribe_room` → the old
  account keeps streaming.

> All three are fixed at once by **deleting the Windows/macOS overrides of the four
> timeline/message handlers and the divergent switch/logout bodies, and routing through
> `ShellBase`** (~180 LOC deleted, three High bugs gone). See "Cross-shell" below.

### Lifetime / threading (UAF & races)
- **Logout destroys the `Client` without draining the worker pools.** e.g.
  `ui/linux-qt/src/MainWindow.cpp:4754-4767`: `stop_sync()` + `remove_account()` drops
  the last `shared_ptr<AccountSession>` and frees the `Client`, but `pool_`/`mut_pool_`
  are not drained on this path. In-flight worker lambdas captured `Client*` →
  use-after-free. Drain/quiesce pools (or capture a session `shared_ptr`/liveness token)
  before destroying the session.
- **`ShellBase` has no liveness token for its `post_to_ui_([this]…)` continuations**
  (pervasive in `ShellBase.cpp`). Safe for the primary window (outlives the loop) but a
  **UAF for spawned/secondary main windows** and account-switch teardown — directly
  relevant to the new multi-window feature. `RoomWindowBase` already solved this with an
  `alive_` `shared_ptr<bool>` (`RoomWindowBase.h:254`); mirror it in `ShellBase`.
- **`clear_all_caches_` races on the cross-window-shared caches.**
  `ui/shared/app/ShellBase.cpp:3885` clears `media_disk_cache()` from a worker thread
  while other windows' UI threads call `load/store` on the same shared `AccountManager`
  object. Confirm `MediaDiskCache`/`PixmapCache` are internally synchronized; if not,
  it's a data race.

### Security / robustness
- **Uncapped notification avatar download (Rust).** `sdk/src/client/media.rs:165-169`
  fetches the sender avatar with `get_media_content(...).unwrap_or_default()` and **no
  size cap**, then hands bytes across the FFI. Every sibling download uses
  `cap_media_bytes`/`NOTIF_IMAGE_CAP`. A malicious/oversized avatar mxc is a
  remote-driven unbounded allocation on a sync worker. One-line fix.
- **Startup crash on a wrong-typed settings field.** `client/src/settings.cpp:20-27`
  catches only `json::parse_error`; a structurally valid `app_settings.json` with a
  wrong field type (e.g. `"notifications_enabled": "yes"`) throws `json::type_error`,
  which escapes `load_from_disk` and crashes at startup. Catch `json::exception` (base).
- **Cross-FFI lock-poison panics (Rust).** 20 sites use `.read().unwrap()` /
  `.lock().unwrap()` (`sdk/src/client/timeline.rs` ×8, `mod.rs` ×4, …). A panic while a
  lock is held poisons it; every later FFI call that touches it then panics, and
  unwinding across the cxx boundary is UB. Prefer `parking_lot` (no poison) or
  `.unwrap_or_else(|e| e.into_inner())`.
- **Unlocked `&self` FFI reads (client).** Many `const`/read methods call through
  `impl_->ffi->…` without `ffi_mu` (e.g. `export_session()` `client/src/client.cpp:170`),
  relying on an undocumented, compiler-unchecked "this Rust method is `&self`" invariant.
  A single Rust signature flipping to `&mut self` silently introduces a data race.
  Document/enforce the invariant or take the lock and measure.

### Functional gap
- **Popup input/hover routing exists only in the Qt host.** Qt routes clicks/hover into
  an open popup and dismisses on outside-click (`ui/shared/tk/host_qt.cpp:1133-1147,
  1193-1214`); macOS/Win32/GTK register and *paint* popups but their `on_pointer_down/move`
  never consult `popup_`. Net effect: `ComboBox` dropdowns (used in `RoomInfoPanel`)
  don't dismiss and may not receive clicks on 3 of 4 platforms. Fix via the shared input
  core below.

---

## Theme 1 — Cross-shell duplication (move logic UI → shared)

The single biggest structural problem. Per the repo's own "Shared vs Platform Code" rule,
these belong in `ShellBase` with thin native virtuals. Estimated **~1,250+ LOC removable
across the four shells**, and it eliminates the drift-bug family above.

| Logic | Qt | GTK | Win | macOS | Shared home | ~LOC/shell |
|---|---|---|---|---|---|---|
| Account restore loop | `MainWindow.cpp:2353` | `2767` | `3462` | `MainWindowController.mm:5504` | `ShellBase::restore_all_accounts_()` + `make_notifier_` hook | 80–110 |
| `on_login_succeeded` add-account core | `2564` | `2898` | `3640` | `loginSucceeded` | `ShellBase::finalize_login_()` | ~70 |
| `switch_active_account` | `4555` | `6077` | `5702` | `5899` | `ShellBase::switch_active_account_impl_()` | ~90 |
| `logout_active_account` | `4740` | `6237` | (peer) | `6509` | `ShellBase::logout_active_account_impl_()` | ~70 |
| `rebuild_tray_` (byte-identical loop) | `5400` | `6755` | `7494` | `7246` | `ShellBase::build_tray_items_()` | ~22 |
| `handle_sync_error` state machine | `4238` | — | `581` | `~6900` | `ShellBase::handle_sync_error_impl_()` | ~50 |
| `set_on_begin_oauth` temp-dir lambda (×~14 total) | `2382,2479,4799` | ×3 | ×3 | ×3 | `ShellBase::arm_pending_login_()` | ~15/site |
| SettingsController build+wire (×2/shell) | `2515,2723` | `2748,3005` | `3580,3754` | `_buildSettingsController` | `ShellBase::ensure_settings_controller_()` | ~10/site |

Plus: the **Windows/macOS overrides of `handle_timeline_reset_ui_` /
`handle_message_inserted_/updated_/removed_ui_`** (`ui/windows/src/MainWindow.cpp:550,
4356,4393,4424,4455`; `ui/macos/src/MainWindowController.mm:1305-1370,6913`) are dead
reimplementations of working base logic — delete them (~180 LOC, fixes 3 drift bugs).

---

## Theme 2 — Duplication inside the shared layer

### tk/ toolkit
- **[High]** Pointer-dispatch state machine copy-pasted across all four hosts
  (`on_pointer_move/up/leave` + `pressed_/hovered_` fields): `host_qt.cpp:1176-1269`,
  `host_win32.cpp:3651-3709`, `host_macos.mm:1723-1787`, `host_gtk.cpp:~1446-1548`.
  Extract a `HostInputCore` — also the clean fix for the popup-routing gap above.
- **[High]** `ListView` vs `GridView` scrollbar machinery byte-identical
  (`list_view.cpp:555-590` vs `1007-1040`, paint `687-696` vs `1098-1107`). Extract
  `ScrollableBase`.
- **[High]** `FontRole → size/weight` mapping reimplemented in all four canvas backends
  (`canvas_cairo.cpp:27-84`, `canvas_qpainter.cpp:52-103`, `canvas_cg.cpp`, `canvas_d2d.cpp:131+`).
  Weight policy is identical app logic; add a shared `font_metrics_for(FontRole)`.
- **[Medium]** `initials_of()` reimplemented per backend; avatar-initials font ratio even
  drifted (`0.36` on Qt vs `0.42` elsewhere — `canvas_qpainter.cpp:573` vs siblings).

### views/
- **[High]** Three list-popups ~90% duplicated scaffolding — `MentionPopup`,
  `ShortcodePopup`, `SlashCommandPopup` (identical measure/arrange/hit/border/wheel,
  e.g. border draw `MentionPopup.cpp:131-140` == `ShortcodePopup.cpp:123-132`). Extract
  `ListPopupBase<Item>` with one `paint_row` virtual.
- **[High]** `EmojiPicker` vs `StickerPicker` are near-identical tabbed-virtualised grids
  (own `GridAdapter`, tab strip, search mode, image cache). Extract `TabbedGridPicker`.
- **[Medium]** `ImageViewerOverlay` vs `VideoViewerOverlay` share scrim + close/save
  chrome + outside-tap dismiss (`ImageViewerOverlay.cpp:304` ~ `VideoViewerOverlay.cpp:456`).
  Extract `MediaOverlayBase`.
- **[Medium]** "fetch avatar via `image_provider_` else `draw_initials_circle`" copy-pasted
  at ~10 call sites. Add a `draw_avatar(...)` helper in `media_utils`.

### app/
- **[High]** Slash-command dispatch ladder duplicated **six times** — twice in
  `RoomWindowBase.cpp:785/823` and once per shell compose handler
  (`linux-qt:653`, `linux-gtk:1134`, `windows:1595`, `macos:2429`). One
  `ShellBase::dispatch_room_send_()`.
- **[Medium]** The async media-fetch pipeline is hand-rolled four times in `ShellBase.cpp`
  (`fetch_media_pipeline_:235`, `ensure_viewer_fullres_:472`, `ensure_picker_image_:960`,
  `ensure_tile_async:1076`) — differ only in cache target/decode bound; one templated
  pipeline removes ~200 LOC and a divergence class.

### Rust SDK
- **[High]** The 8 media-send paths (`send_image/file/audio/video` + `_async` twins,
  `sdk/src/client/send.rs:727-1481`) are near-identical; extract
  `build_attachment_config()` + `do_send_attachment()`.
- **[High]** `timeline_convert.rs:602-760` destructures a **29-element positional tuple**
  and each match arm re-lists ~25 placeholder values in positional order — extremely
  silent-bug-prone. Rewrite as partial-struct `+ ..ffi_event_defaults()` (already used at
  line 590).

### client
- **[Medium]** Three coexisting JSON parsers, two hand-rolled with surrogate decoding
  (`client/src/client.cpp:979-1262`, `session_store.cpp:38-124`) while `nlohmann/json` is
  already a dependency. Consolidate; deletes ~350 lines of bespoke, security-sensitive code.
- **[Low]** All 36 `event_handler_bridge.cpp` methods repeat the same
  `guard(...) + slot_->load() + null-check` preamble; a `with_handler()` wrapper removes
  ~150 LOC and prevents a future method forgetting the null-check.

---

## Theme 3 — God-objects to decompose

- **`MessageListView`** (`ui/shared/views/MessageListView.cpp`, 7,235 LOC, ~98 methods)
  conflates timeline list, selection model, voice/audio/video players, URL-preview cards,
  the room-switch gate state machine, and 10 hit-test maps. `paint_row` (~700 LOC) and the
  pointer handlers (~480–550 LOC each) should be split by row-Kind; the media-player
  concerns (`:5073-5435`) should move to a `TimelineMediaController`. #1 maintainability
  liability.
- **`ShellBase`** (`.h` 1,957 + `.cpp` 4,656) holds ~10 responsibilities as flat
  `protected` state. Extract a `MediaController`/`MediaCache` (the pipeline + ~12 dedup
  sets + backoff), the thread-panel state machine, and the secondary-window registry into
  collaborators — exactly as `AccountManager` was already split out. Shrinks the ~50-virtual
  hook surface and makes the threading boundary auditable.

---

## Dead code (safe deletions)

**Rust** — `sdk/src/client/send.rs:10,34` self-silencing `build_uia_fallback_url` re-export
shim; run `cargo clippy --fix` for 23 auto-fixable warnings incl. 7 dead
`OwnedMxcUri::into()` in `media.rs`.

**client** — blocking `get_url_preview` (`client.cpp:1480`), `paginate_back`
(`:267`), `fetch_media/source/avatar_thumbnail_bytes` (`:890-902`), `list_rooms`
(`:222`); legacy `SessionStore::load/save/clear` (`session_store.cpp:254-293`); unused
`#include <cassert>` (`client.cpp:9`).

**tk/** — `FillBackground` (`widget.h:296-320`, zero refs); `ToggleButton`
(`controls.h:268`) and `ListView::reset_near_bottom_latch` (`list_view.h:249`) alive only
via tests; `NativeTextField::set_compact` dead on the public interface;
`audio_gtk.cpp:336` `pipeline_` "unused (kept for symmetry)".

**views/** — `EncryptionSetupOverlay::set_passphrase_input/set_key_input` + members
(`.h:131-147`, the fallback can never fire since `ShellBase.cpp:4471` always wires the
getters); `UserInfo::set_avatar_size` / `set_show_user_id`; `LoginView::set_homeserver_label`.

**app/** — `shell_sticker_no_fetch_` (`ShellBase.cpp:641`, zero callers); the
`on_media_preview_config_applied_` virtual (no overrides); two write-only
`typing_bar_visible_` fields (`ShellBase.cpp:3200`, `RoomWindowBase.cpp:709`);
`RoomWindowBase::compose_typing_active_` (never read/written); de-virtualize the five
never-overridden `apply_thread_*` hooks (`ShellBase.h:1046-1057`).

---

## Other correctness / robustness (Medium)

- **`map_rect_geom_` never cleared** (`ui/shared/views/MessageListView.cpp:3153`) — unlike
  its 8 sibling geometry maps cleared each `paint()`, this grows one entry per
  location-message ever painted → slow unbounded leak + stale hit-tests. One-line fix in
  the paint reset block.
- **Numeric HTML entities lack validity filtering** (`ui/shared/views/html_spans.cpp:123`)
  — surrogates / NUL / C0 controls reach `append_codepoint` and emit invalid UTF-8 that
  Pango/DirectWrite/CoreText may mishandle. Clamp to U+FFFD.
- **Formatting-stack asymmetry under depth cap** (`html_spans.cpp:738-770`) — once
  `kMaxTagDepth` is hit, a dropped open tag's matching close still pops a legitimate outer
  frame → silent formatting corruption (no crash). Track dropped opens.
- **`persist_session` blocks a sync worker on keychain + fsync with no timeout**
  (`client/src/event_handler_bridge.cpp:284`) — a slow/prompting credential store can stall
  sync.
- **`oauth.rs:133,176` `expect()` in the login path** — latent panic-across-FFI; convert to
  `?` with context.
- **`start_sync` is a single ~1,000-line method** nested ~11 levels deep
  (`sdk/src/client/sync.rs:28-1079`) — hardest-to-audit function in the crate; decompose
  the VectorDiff dispatch / room-list / notification paths.
- **Silent `fprintf(stderr)` error handling** on failed join/leave/upload
  (`ShellBase.cpp:2389`) — route through `show_status_message_` for user-visible feedback.
- **Windows session blob persisted `CRED_PERSIST_LOCAL_MACHINE`** vs macOS
  `…ThisDeviceOnly` — confirm the exposure scope is intended for OAuth tokens
  (`secret_store_windows.cpp:49`).

---

## Suggested sequencing

**Before launch (correctness/safety):**
1. Delete Windows/macOS timeline-handler overrides → route through `ShellBase` (fixes 3
   drift bugs, −180 LOC).
2. Windows account-switch `unsubscribe_room` (standalone fix even before the refactor).
3. Logout pool-drain / liveness token; add `ShellBase::alive_` token for `post_to_ui_`
   continuations (matters for the new multi-window feature).
4. Uncapped notification avatar download (one-liner); `settings.cpp` widen catch.
5. Popup input/hover routing for non-Qt hosts (ideally via the shared `HostInputCore`).

**Shortly after (structural, high ROI):**
6. Extract account restore/login/switch/logout/tray/sync-error into `ShellBase` impl
   methods (~1,250 LOC out of shells; kills the drift-bug class permanently).
7. Shared `HostInputCore` + `ScrollableBase` + `ListPopupBase` + `TabbedGridPicker` +
   `MediaOverlayBase`.
8. Rust: lock-poison → `parking_lot`; collapse the 8 media-send paths; replace the
   29-tuple in `timeline_convert`.

**Cleanup (low risk):**
9. Delete the dead-code list above; `cargo clippy --fix`; consolidate the C++ JSON parsers
   onto `nlohmann`.

**Larger projects (schedule deliberately):**
10. Decompose `MessageListView` and `ShellBase` into collaborators.

---

## Notes / positives
- The `extern "Rust"` FFI surface has **no dead exports** — all bridge fns are referenced.
- The HTML/markup path is XSS-safe: `href` honored only for `http(s)` on `<a>`, all other
  tags stripped, tag-depth capped, numeric-entity accumulation clamped against overflow.
  The gaps above are correctness polish, not injection holes.
- `CLAUDE.md` is stale on one point: it lists `markdown (Markdown→HTML)` under
  `ui/shared/views/`, but that conversion actually lives in `client/src/markdown.cpp`
  (Rust-backed). Only `html_spans.cpp` (HTML→TextSpan) is in views.
