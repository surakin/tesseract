# Tesseract Branch Code Review — 2026-06-10

Whole-branch review of `development` ahead of merging to `main`
(`git diff main...development`, 8 feature/fix commits, ~2,880 insertions across
50 files). Conducted with seven parallel finder angles — three correctness
(line-by-line diff scan, removed-behavior audit, cross-file trace) plus reuse,
simplification, efficiency, and altitude — then each surviving candidate
verified against the code.

Severity legend: **High** = correctness/safety, **Medium** = should fix,
**Low** = cleanup/robustness polish.

Each finding records its **resolution**. The fixes landed in commit
`985a2c65` ("fix: address whole-branch code-review findings"); the original
features are in `819e2fe6` (media priority + stall reclamation) and `154b0884`
(room-list unread highlighting).

---

## Executive summary

The branch is in good shape. The structurally risky changes were verified
**clean**: the new `RoomInfo` fields propagate to all six construction sites
(build_room_info, the cxx `Clone`, the `cfg(test)` mirror, `from_ffi`,
`types.h`, and the fingerprint); the `fetch_media_async` priority parameter is
ordered consistently across the cxx bridge, the C++ wrapper, and the Rust impl;
the `prioritize_media` slice lifetime is sound; and the `&mut`→`&self` /
`std::mutex`→`std::shared_mutex` FFI refactor keeps every mutating bridge method
under the exclusive lock (no method demoted to a shared lock mutates plain
state — the Rust borrow checker enforces it).

Ten findings survived verification. Five were correctness/behavior issues
(roster staleness, off-screen voice playback, a thread-subscription watcher
race, an over-concurrency tradeoff, and a permalink-stash leak); the rest were
efficiency/robustness/altitude. Seven were fixed; three were deferred or
declined with rationale.

---

## Correctness / behavior

### 1. Roster goes stale on a net-zero room-set change — **High — Fixed**
`ShellBase::push_rooms_` invalidated the `@`-switcher known-users roster only
when `rooms_.size()` changed. A sync that joins one room and leaves another
(count unchanged) left the roster stale, so members of the newly-joined room
never surfaced in partial-name `@` search (only a fully-typed mxid live-resolved
them).
**Fix:** invalidate on a change to the joined-room-id *set* via an
order-independent XOR fingerprint (`known_users_room_set_hash_`). +2 regression
tests (`test_shell_roster_invalidation.cpp`).

### 2. Cold-clicked voice clip can auto-play off-screen — **Medium — Fixed**
`TimelineMediaController` arms a pending play on a cache-miss click and
`MessageListView::arrange()` retried it on *every* relayout, with no visibility
or TTL guard. Clicking a cold clip then scrolling away (or receiving a new
message) before the bytes warmed would auto-start audio for a clip the user is
no longer looking at.
**Fix:** `arrange()` retries only while the armed clip is still within the
visible range (`is_event_visible_`); otherwise it abandons the pending play.

### 3. Thread-subscription is a non-atomic read-modify-write — **Medium — Fixed**
The FFI refactor made `subscribe_room_threads` / `subscribe_thread` `&self` under
a shared lock, but each did `map.write().remove()` → spawn watcher → (later)
`map.write().insert()` with the guard dropped in between. Two concurrent
subscribes for the same room could both insert and orphan a live
`ThreadListService` watcher task that fires `on_threads_updated` forever (latent
today — callers are UI-thread-only).
**Fix:** install via a single guarded `insert` that returns and aborts the
replaced handle (`subscribe_thread` keeps its intentional cancel-before-build
remove and additionally aborts any racing insert).

### 4. Stall reclamation can exceed the lane concurrency cap — **Medium — Noted**
A media slot held past `STALL_DEADLINE` (8 s) stops counting against the lane
`limit`, so on a slow link a legitimate but slow (>8 s) download is treated as
stuck and its slot reclaimed, letting the bulk lane run up to its `ceiling`
(2× base) concurrent real downloads — more connection pressure exactly when the
network is slow. This is an inherent consequence of the approved 2×-ceiling
design.
**Resolution:** accepted as a deliberate tradeoff. If it bites in practice, tune
`STALL_DEADLINE` up or make reclamation progress-aware. No code change.

### 5. matrix-uri event-scroll stash leaks / mis-fires — **Low — Fixed (partial)**
`pending_event_scroll_after_join_` is keyed by room id and consumed on any join
of that room, erased only on success — so a failed join left it resident
forever, to fire on some later unrelated join and yank the timeline to a stale
event.
**Fix:** evict the stash on join failure. The rarer "cancel the permalink
dialog, then join the same room later by unrelated means" mis-fire is left
documented (a full fix needs dialog-cancel plumbing out of proportion to the
risk).

---

## Efficiency / robustness / altitude

### 6. Per-waiter polling drove stall reclamation — **Low — Fixed**
Each parked gate waiter armed its own 1 s `sleep` and ran the lane-global
`dispatch()`, so a backlog of N parked fetches meant N timers + N mutex
acquisitions per second.
**Fix:** a single gate-owned recheck task (`recheck_running` + `spawn_recheck_task`)
spawned when the first waiter parks and self-terminating when the queue drains —
O(1) timers; waiters just await their slot.

### 7. Reverse-map erase asymmetry — **Low — Fixed**
`media_key_to_req_` is maintained alongside `pending_media_`; the guarded erase
existed in `handle_media_ready_ui_` and `cancel_media_group_` but not in the
URL-preview completion path, so a future preview that set a `priority_key` could
leave a dangling key→dead-request entry.
**Fix:** mirror the same guarded erase in `handle_url_preview_ready_ui_`, so
every `pending_media_` removal site keeps the map in lockstep.

### 8. Switcher rows re-shape text every frame — **Low — Deferred**
`QuickSwitcher::paint_user_row` rebuilds both `TextLayout`s on every paint,
unlike the room-mode `paint_row` which caches them — full text shaping per
visible row per frame on hover/scroll in user mode.
**Resolution:** deferred (perf polish; user mode is transient). Worth adopting
the room-row layout cache for user rows in a follow-up.

### 9. Roster build re-sorts the full set per batch — **Low — Deferred**
The `@`-roster worker re-filters + `O(n log n)`-sorts the entire accumulated
`known_users_` after every 8-room batch (quadratic-ish across the sweep), and
fully marshals over-cap rooms' member lists before discarding them.
**Resolution:** deferred. Sort once at the end / maintain a top-100
incrementally; apply the size cap before materializing the member list.

### 10. `contains_ci` was a 5th copy of the same matcher — **Low — Fixed (branch copy)**
The new `ShellBase::contains_ci` duplicated the identical ASCII
case-insensitive-substring logic already present as `name_matches` /
`icontains` in five places.
**Fix:** extracted a canonical `tk::ci_contains` (`tk/text_util.h`) and migrated
the shell roster filter. The pre-existing copies (MentionEngine, ShortcodeEngine,
StickerPicker, QuickSwitcher, RoomListView) are left for a follow-up sweep —
they predate this branch and the helper now exists for them to adopt.

---

## Declined cleanups (with rationale)

- **Switcher placeholder string duplicated across the four shells** — the
  placeholder is set on each shell's *native* text field, and `tr()` / `_()`
  require literal arguments for translation-string extraction; folding it into a
  shared constant would break per-platform i18n. (The real cleanup is making the
  GTK shell use `_()` consistently — a separate i18n task.)
- **Open room briefly renders bold + dot until its read receipt clears** — the
  correct fix is SDK-side (`num_unread_messages` reflecting the just-advanced
  read marker); a renderer band-aid that suppresses the dot for the selected
  room misbehaves when the user is scrolled up in a room with genuine unread.
- **Move `GateInner.seq` into `MediaQueue`** — would remove the explicit `seq`
  the pure-queue tests pass to control ordering deterministically; net-negative.

---

## Verified clean (no change needed)

- `RoomInfo` field propagation across all six sites + the room-list fingerprint.
- `fetch_media_async` priority-parameter ordering across the cxx boundary, and
  the `prioritize_media` `rust::Slice` lifetime.
- The `&mut`→`&self` + `shared_mutex` FFI refactor: every remaining writer keeps
  the exclusive lock; no shared-locked method mutates plain (non-synchronized)
  state.
- The `on_visible_range_changed` wiring reaches both the main and thread-panel
  message lists, and `active_media_group_` matches both (they share the room's
  media group).
- The room-list fingerprint incorporates the quiet-unread state, so the dot
  appears and clears live.

## Status

Verified after fixes: **222 Rust + 758 C++ tests pass**, all four targets link.
Findings 4, 8, 9 and the declined cleanups are tracked here as follow-ups; nothing
remaining is a merge blocker.
