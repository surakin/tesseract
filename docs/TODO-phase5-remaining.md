# Phase 5 (god-object decomposition) — remaining work

Pick up here next session. Detailed seam maps + ranked extraction lists are in
`docs/superpowers/plans/2026-06-09-phase5-decomposition.md`. Master plan:
`docs/superpowers/plans/2026-06-09-prelaunch-remediation.md`.

## Done so far (10 extractions, all behavior-preserving, 734 C++ / 208 Rust green)
- `start_sync` → 11 named watcher/handler free fns (`sdk/src/client/sync.rs`, 1074→746 LOC).
- `MessageListView` collaborators (`ui/shared/views/`): `TimelineMediaController` (voice+audio — runtime-verified),
  `SpoilerRevealer`, `ReadReceiptTracker`, `LocationMapPanner` (+fixed the `map_rect_geom_` leak),
  `TimelineVideoPlaylist`, `RoomSwitchGateKeeper`, `UrlPreviewCardDisplay`, `LinkLayoutCache`.
  → `MessageListView.cpp` 7219 → 6120 LOC.
- `ShellBase`: `ThreadPanelController` (kept `thread_panel_`/`current_thread_root_` + apply side-effects on
  ShellBase so the 4 shells compile untouched). `PresenceCoordinator` deliberately skipped (already in
  `PresenceTracker`; would be net-negative indirection).

Compile confirmed on Linux (Qt6+GTK4), macOS, and Windows for everything above.

## ⚠️ Before continuing: runtime smoke of the landed collaborators
The 734 tests don't fully cover interactive behavior. Build/run the branch (now on `main`, unpushed) and check:
select text + copy, click an inline link, open a URL-preview card, location-map pan/zoom, inline GIF/video
autoplay, room-switch load-hold (no reflow flash), spoiler reveal, read-receipt avatars, thread panel open/close.
Media playback was already verified.

## Remaining extractions — HARDER tier (do these deliberately, smoke each)
These are woven through the ~700-line `Adapter::paint_row` and the ~500-line `on_pointer_down`/`on_pointer_up`
switch, and touch interactive behavior the test suite only partially covers. Extract one at a time, build+test, and
runtime-smoke the specific interaction before stacking the next.

1. **`TextSelectionModel`** — drag-select, word/line boundaries, copy-to-clipboard. Reads `LinkLayoutCache`
   (already extracted — good foundation). State: `sel_`, `sel_is_dragging_`, `press_sel_`, `click_count_`,
   multi-click detection. Woven into `on_pointer_down`/`up`/`drag` + `copy_selection`. MEDIUM-HIGH risk
   (selection/copy regressions are easy to miss).
2. **`ReactionChipUI`** — per-row reaction chip geometry + overflow pill + chip click/hover dispatch. ~400 LOC,
   tightly in `paint_row` + `hovered_row_geom_`. MEDIUM-HIGH.
3. **`ActionPillUI`** — the hover action buttons (react/reply/thread/edit/more) cell strip + visibility gates +
   hit-test. ~350 LOC. Visibility gates depend on message state (is_own, kind, can_pin, pinned set). MEDIUM-HIGH.
4. **`InlineMediaDisplay`** (optional) — image/sticker/file/video-thumbnail paint + media-suppression (MSC4278) +
   click hit-test. ~500 LOC, 5 media kinds. MEDIUM.

## Remaining ShellBase cuts — SHELL-ENTANGLED (touch unbuildable-here macOS/Windows; flag + on-platform recompile)
5. **`MediaController` / `MediaCache`** — the big one (~1300 LOC): `run_media_fetch_` + `ensure_*` +
   decode-family + the AccountManager-shared caches + ~12 dedup sets + backoff + `active_media_group_`. Calls
   back via `on_media_bytes_ready_`/`decode_image_`/`post_to_ui_alive_`/`request_relayout_`. Highest value, do last.
6. `PaginationRegistry`, `SecondaryWindowRegistry`, MSC4278 preview-gating — MEDIUM, partly shell-touching.

## Recipe (every extraction)
Map the subsystem → collaborator owns its state, host delegates (callbacks injected via std::function),
`press_*` pointer-FSM fields STAY on the host, shell-read ShellBase fields STAY on ShellBase →
`cmake --build build/linux-qt6-debug && build/linux-gtk-debug && ctest` (baseline 734) + `cargo test` for Rust →
commit per extraction → flag macOS/Windows-touching changes for on-platform recompile.

## Other open follow-ups (minor, from the whole remediation)
- Windows window-geometry DPI bug — `docs/TODO-windows-dpi-geometry.md` (Win-only, pick up on Windows).
- `settings.cpp` partial-state on a mid-read `type_error` (parse into a temp, then assign) — minor robustness.
- `kPreviewCardH` (72.0f) duplicated between `MessageListView.cpp` and `UrlPreviewCardDisplay.cpp` — expose the
  card height from the collaborator for a single source of truth.
- The broader Rust-side `client` field sync (`RwLock<Option<Client>>`) so independent FFI reads run concurrently —
  the coarse `ffi_mu` lock (now correct) serializes blocking reads behind writes; larger refactor, optional.
