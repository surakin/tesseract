# Phase 4 — Shared-Layer Dedup Sub-Plan

> Sub-plan of `2026-06-09-prelaunch-remediation.md`, Phase 4. Behavior-preserving dedup inside the shared library (`ui/shared/tk`, `ui/shared/views`, `ui/shared/app`) and the Rust SDK. Branch: `refactor/prelaunch-phase4-shared-dedup` (stacked on Phase 3).

**Goal:** Collapse the remaining intra-shared duplication into reusable bases/helpers, verified by the existing test suite + Qt/GTK builds. Most of this is fully locally verifiable; only `HostInputCore` (Win/macOS host backends) and the canvas font/initials helpers (CoreGraphics/D2D backends) touch code that can't build here.

**Recipe (every task):** map the duplicated sites → introduce the shared base/helper → convert each duplicate → build Qt6+GTK4 + `ctest` (baseline 721) + `cargo test` (208 for SDK tasks) → commit. Behavior-preserving; new behavior gets a test first. Flag any Win/macOS-touching change for on-platform recompile.

## Tasks (execution order — most locally-verifiable / highest-value first)

### 4.9 — SDK `timeline_convert` partial-struct rewrite (Rust, fully verifiable; highest bug-risk removed)
`sdk/src/client/timeline_convert.rs:602-760` destructures a 29-element positional tuple; each match arm re-lists ~25 placeholder values positionally. Rewrite each arm to build a partial `TimelineEvent { <set fields>, ..ffi_event_defaults() }` (the file already uses `ffi_event_defaults()` at ~590) and drop the tuple. Removes the crate's most silent-bug-prone pattern.

### 4.8 — SDK media-send dedup (Rust, fully verifiable)
`sdk/src/client/send.rs:727-1481` — 8 near-identical media-send paths (`send_image`/`send_file`/`send_audio`/`send_video` + `_async` twins). Extract `build_attachment_config(info, caption, reply)` + `do_send_attachment(room, filename, mime, bytes, config, deliver)`; sync/async wrappers differ only in runtime-handle acquisition + result delivery.

### 4.10 — client `with_handler()` wrapper (C++, fully verifiable)
`client/src/event_handler_bridge.cpp` — 36 methods repeat `guard("name", [&]{ auto* h = slot_->load(); if (!h) return; … })`. Add a `with_handler("name", [&](auto* h){…})` wrapper folding guard+load+null-check; convert the 36 sites.

### 4.4 — `ListPopupBase<Item>` (views, fully verifiable)
`MentionPopup` / `ShortcodePopup` / `SlashCommandPopup` share ~90% scaffolding (measure/arrange/hit/border/wheel/selected-index). Extract a `ListPopupBase` owning index/hit/scroll/border + one virtual `paint_row(i, ctx, rect)`.

### 4.7 — `draw_avatar(...)` helper (views, fully verifiable)
~10 copies of "fetch avatar via `image_provider_` else `draw_initials_circle`" (MentionPopup, UserInfo, UserProfilePanel, RoomInfoPanel, RoomListView, RoomHeader, QuickSwitcher, InviteCard, MessageListView). Add one `draw_avatar(canvas, rect, url, name, id, provider, palette)` in `media_utils`.

### 4.6 — `MediaOverlayBase` (views, fully verifiable)
`ImageViewerOverlay` / `VideoViewerOverlay` share scrim + close/save chrome + outside-tap dismiss + icon caching. Extract `MediaOverlayBase`.

### 4.2 — `ScrollableBase` (tk, fully verifiable)
`ListView` vs `GridView` — `thumb_geom`/`thumb_hit`/`clamp_scroll`/scroll-drag/thumb-paint are byte-identical (`list_view.cpp:555-590` vs `1007-1040`). Extract `ScrollableBase` holding `scroll_y_` + drag state + scrollbar geometry/paint/hit.

### 4.5 — `TabbedGridPicker` (views, fully verifiable; large)
`EmojiPicker` vs `StickerPicker` — near-identical tabbed virtualised grids (own `GridAdapter`, tab strip, search mode, image cache, hover/press state). Extract `TabbedGridPicker` base.

### 4.1 — `HostInputCore` (tk; Win/macOS hosts NOT buildable here)
Extract the copy-pasted pointer-dispatch state machine (`on_pointer_move/up/leave` + tracked-pointer fields + popup routing) from all four hosts into a shared core. **Deletes the Phase-1.7 tactical popup copies and the Qt original.** Qt/GTK verifiable; Win/macOS static + flag.

### 4.3 — shared `font_metrics_for(FontRole)` + `initials_of()` (canvas; CG/D2D NOT buildable here)
Collapse the 4 per-backend `FontRole→weight` switches and `initials_of()` into shared helpers; fold the drifted `0.36`/`0.42` avatar-initials ratio into one constant. Cairo/QPainter verifiable; CG/D2D static + flag.

### Deferred within Phase 4 (bigger; do last or push to a follow-up)
- 4.12 — template the 4 hand-rolled `ShellBase` media-fetch state machines into one pipeline (lead-in to Phase 5's MediaController).
- 4.11 — consolidate the two hand-rolled C++ JSON scanners onto `nlohmann/json` (~350 LOC); larger, own commit.

## Verification
Per task: `cmake --build build/linux-qt6-debug && cmake --build build/linux-gtk-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure` (+ `cargo test -p tesseract-sdk-ffi` for SDK tasks). Commit per extraction.
