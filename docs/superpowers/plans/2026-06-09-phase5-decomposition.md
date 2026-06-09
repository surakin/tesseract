# Phase 5 — God-Object Decomposition Sub-Plan

> Sub-plan of `2026-06-09-prelaunch-remediation.md`, Phase 5. Behavior-preserving decomposition of the two C++ god-objects (`MessageListView` 7.2k LOC, `ShellBase` 5.3k+2.3k LOC) and the Rust `start_sync` (~1k-line fn). Branch: `refactor/prelaunch-phase5-decomposition` (off `main`). Post-launch scale; the riskiest phase.

**Goal:** Extract cohesive subsystems into collaborator classes / free functions, lowest-risk first, each verified by the full test suite (734 C++ / 208 Rust) + Qt/GTK build. Public APIs (MessageListView's ~30 callbacks + methods used by RoomView; ShellBase's shell-facing surface) must NOT change. macOS/Windows-touching changes flagged for on-platform recompile.

**Recipe (every extraction):** map the subsystem's fields+methods → introduce the collaborator (owns the state, exposes a minimal interface: setters, event hooks, paint/geometry recorders) → move the logic, leaving the host calling the collaborator → build Qt6+GTK4 + ctest (+ cargo for Rust) → commit. The host keeps cross-cutting concerns; the collaborator gets a self-contained one.

## Targets & ranked extraction lists (from the seam maps)

### A. Rust `start_sync` (`sdk/src/client/sync.rs`) — DO FIRST (lowest risk, fully buildable)
The fn is a registration routine spawning ~10 async tasks; state flows via Arc clones (no mutable-borrow-across-await). Extract mechanically:
- **A1** Hoist the two nested `fn`s (`refresh_dm_counterparts` ~341, `emit_snapshot` ~357) to module scope taking their captured state as params. Trivial.
- **A2** Extract the simple stream-watchers as free `async fn`s: session-refresh watcher (~56-99), backup/recovery/imported-keys watchers (~662-822), room-list-state + verification-state observers (~824-912). Each takes `(handler, client, stop_rx, …)` clones.
- **A3** Extract the 3 notification event-handler bodies (message/sticker/typing ~101-235) and the presence-poll loop (~237-279) as free fns.
- **A4 (DEFER)** The ~350-line room-list watcher / SyncService state machine (~281-659) — circular state deps; refactor in-place into smaller blocks only if time permits, do NOT extract wholesale.

### B. `MessageListView` (`ui/shared/views/MessageListView.{h,cpp}`)
Public API (set_messages/insert/update/remove, the ~30 `on_*` callbacks, hit_at queries, scroll control) is STABLE — do not change. Tier order:
- **B1 (Tier 1, lowest risk):**
  - `TimelineMediaController` — voice+audio playback (handlers ~5057-5340, paint ~3338-3665, state ~996-1047). Self-contained (~95%); owns `audio_player_` + bytes provider; interface = `on_play_clicked/on_scrub/on_speed_clicked/record_*_geometry/paint_*_card/update_position`. **Flagship; highest value, low risk.**
  - `SpoilerRevealer` — revealed-spoiler set + press/reveal (~6200-6337). 100% self-contained.
  - `ReadReceiptTracker` — receipt-disc paint + `maybe_notify_receipt_` (~5569-5590, paint in paint_row). 100% self-contained.
  - `LocationMapPanner` — Kind::Location pan/zoom (~3134-3240, pointer/wheel). ~98% self-contained.
  - `TimelineVideoPlaylist` — inline video player pool (`start_inline_video` ~4833-4912, paint ~3665-3748). Needs the `alive_` weak-ptr liveness for async fetch callbacks.
- **B2 (Tier 2, medium):** `RoomSwitchGateKeeper` (gate machine ~4090-4286), `UrlPreviewCardDisplay`, `LinkLayoutCache`.
- **B3 (Tier 3, high/cross-cutting — only if warranted):** `ReactionChipUI`, `TextSelectionModel`, `InlineMediaDisplay`, `ActionPillUI` — deeply woven into paint_row + pointer dispatch; defer.

### C. `ShellBase` (`ui/shared/app/ShellBase.{h,cpp}`)
- **C1 (low risk):** `ThreadPanelController` — the thread-panel state machine (`compute_thread_transition_`/`apply_thread_transition_`/ThreadTransition + thread_panel_ fields, ~200 LOC). Pure logic, clean boundary.
- **C2 (low risk):** `PresenceCoordinator` — thin wrapper over the existing PresenceTracker (~150 LOC).
- **C3 (medium):** Pagination registry; secondary-window registry; MSC4278 preview gating.
- **C4 (high value, medium-high risk):** `MediaController`/`MediaCache` — the media fetch pipeline (`run_media_fetch_`/`ensure_*`/decode-family/caches/dedup sets/backoff/`active_media_group_`, ~1300 LOC). Calls back via `on_media_bytes_ready_`/`decode_image_`/`post_to_ui_alive_`/`request_relayout_`. The biggest, do last.

## Execution order (this branch)
1. **A1–A3** (sync.rs) — safest, validates the approach in Rust.
2. **B1** MessageListView Tier-1 collaborators (TimelineMediaController first, then the four small ones).
3. **C1–C2** ShellBase ThreadPanel + Presence.
4. Reassess scope; B2 / C3 / C4 as warranted. B3/A4/C4 are the deep cuts — schedule deliberately; each its own careful pass.

## Verification (every extraction)
`cmake --build build/linux-qt6-debug && cmake --build build/linux-gtk-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure` (baseline 734) + `cargo test -p tesseract-sdk-ffi` (208) for Rust. Commit per extraction. Flag macOS/Windows-touching changes (most MessageListView/ShellBase code is shared and built by Qt/GTK, so the buildable shells exercise it; the four shells only call the stable public API).
