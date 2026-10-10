# ROADMAP.md

Completed work is in [CHANGES.md](CHANGES.md). This file lists only pending
and in-progress work, as a single backlog ordered by priority/urgency.

## Tier 1 — Finish what's in flight (don't start new things until these are done)

- **Calls**: the real testing tail — more cross-network/cross-platform
  combinations, lifecycle edge cases (drops, rejoins), now with the TWIM
  post out there potentially recruiting real testers. This is now partly
  not solely on us, which is good — but it needs engagement with whatever
  testers show up in the room.
- **Screen sharing**: same testing-tail treatment, at whatever level of
  priority given it's explicitly YMMV/rougher than calls.

## Tier 2 — Next up

- **DM-counterpart avatar picks the bridge bot itself** when the bridge
  doesn't publish `io.element.functional_members` (MSC4171) — heisenbridge
  currently lacks the state event, so 1:1 control rooms show the bot's own
  avatar. Workaround: ship a small allow-list of known bridge user-ID
  prefixes (`@heisenbridge:`, `@_neb_`, etc.) on the Rust side, or
  contribute the missing state-event publication upstream to heisenbridge.

## Tier 3 — Smaller deferred items, pick opportunistically

- **Accessibility — screen-reader verification, then Phase 5.** Every
  view audited on 2026-10-03 is now mapped (see `docs/ACCESSIBILITY-PLAN.md`);
  nothing has been driven with a real screen reader yet (Orca on Qt6 first,
  then GTK4, Narrator/NVDA, VoiceOver). Phase 5's keyboard-only audit is
  done (2026-10-04) bar a live keyboard-only walkthrough per platform; the
  high-contrast theme and reduce-motion still need their own plan.
- **Animated avatars: bound their memory.** Animated avatars live in the
  shared 64 MB `anim_cache_` (stickers, GIFs, pickers). Decoding is capped only
  by `kAnimDecodeMaxFrames` (200 frames × ~25 KB at 80×80 ≈ 5 MB per avatar),
  so a room full of long animated avatars can crowd out other animations.
  Planned fix: in `ShellBase::store_decoded_media_`, keep an avatar animation
  as its first-frame still when its decoded frames exceed ~2 MB (about 80
  frames at 80×80). A separate avatar `AnimImageCache` was ruled out for now:
  its tick (`advance()`) is driven from each shell, so it would touch all four.
  Off-screen avatar entries can still be evicted by `sweep()` and simply reload
  from the disk cache the next time they are painted.
- Cmd/Ctrl+K refinements, room mentions as pills (vs. just user mentions),
  self-mention emphasis, device rename, new-device warnings, edit history
  viewer, GIF picker.
- Room upgrades, alias management beyond viewing.
- **MSC4153 follow-ups.** The MSC4350 bridge-bot annotation, a reason
  tooltip on sends blocked by an encryption check (`pending_error` is
  already on the row), and translating the Rust-side UTD reason strings
  (`utd_message_for_cause`).
- **MSC2545 pack management — manual order/sort.** List/subscribe UI, pack
  creation/removal, and sticker delete/rename inside the user pack all
  shipped 2026-07-11 (global "Emojis & Stickers" settings tab +
  per-room/space editor), including position-aware drag-drop reordering of
  packs themselves. Still missing: reordering images *within* a single pack.
- **Sessions tab — inline rename of device display name.** FFI/Client/Controller
  already plumbed end-to-end (`Client::set_device_display_name`,
  `SettingsController::rename_device`, `on_device_renamed`). Needs a per-row
  `NativeTextField` overlay: either (a) extend `DevicesSection` with a
  `rename_field_rect()` analogous to `AccountSection::name_field_rect()`
  and route each shell's existing single `NativeTextField` over the active
  row, or (b) add a `tk::Host::prompt_text(...)` dialog primitive backed by
  `QInputDialog` / `GtkDialog` / `MessageBoxW` / `NSAlert`.
- **Sessions tab — out-of-band verification trigger.** Each row could carry
  a "Verify" button that fires `request_self_verification()` and pops the
  SAS overlay focused on the chosen device.
- **MSC4391 bot commands — Win32 verification.** Discovery, argument-entry,
  and send are wired into all four shells' main windows and pop-outs alike
  (AppKit's main window was migrated onto `SlashCommandController` to close
  its parity gap), but only Qt6/GTK4/AppKit were verified locally; Win32
  mirrors the same pattern unverified (no Windows toolchain here).
- **Notification preview image is fetched as a full file, not a
  thumbnail** — the SDK downloads the full media (≤ 2 MiB cap) on the sync
  handler regardless of whether the C++ layer will display it (it can't
  see window-focus / lock state). Fix: use `MediaFormat::Thumbnail` and
  skip the fetch when the notification won't be shown. The macOS + Linux
  (Qt6/GTK4) `IScreenLock` impls and notifier-render paths still need
  on-device smoke tests (built only on Win32 here).
- **Win32 title-bar chrome doesn't follow the in-app accent choice** —
  `ui/windows/src/Theme.h`/`.cpp` (`win32::theme`) is a separate,
  GDI/DWM-only system governing native, non-client-area rendering (caption
  colors, immersive-dark-mode toggling); it reads the OS's own registry
  accent color and has no coupling to `tk::Palette` or
  `Settings::theme_accent`. Picking a non-Blue accent (Forest/Sunset/Violet)
  in Settings → Appearance currently only affects in-app widget painting,
  not the window's own title bar. Out of scope for the accent-themes
  feature; a distinct, Windows-only follow-up if wanted.
- **GTK4 message-list CSS not theme-aware** — `apply_theme_ui_()` only
  rebuilds the `.sidebar` / `.sidebar-separator` CSS rules. `.sender-name` /
  `.timestamp` / `.room-header` / `.room-header-topic` and the
  `status_bar_` / `topic_tooltip_label_` `GtkLabel`s keep hardcoded light
  colours. Fix: rebuild all theme CSS rules from `t.palette` and give the
  status / tooltip labels palette-driven CSS classes.
- **`tk_avatars_` / `tk_images_` not keyed by `(user_id, mxc)`** — cosmetic
  ghosting risk when two accounts share an mxc URL that resolves to
  different bytes.
- **`TestSurface` doesn't cover CoreGraphics** — QPainter, Cairo, and D2D
  are tested; macOS CGBitmapContext surface is still TODO.
- **Media viewer follow-ups.** Viewer audio isn't reported to `MediaPlaybackHub` (MPRIS / media keys); gallery audio/file tiles are text-only (no glyph or waveform); the timeline grid paints only 20 gallery cells (`kMaxGalleryItems`) and the viewer browses at most 200 (`kMaxGalleryViewerItems`); with prev/next active the image page keeps its own 64 px margin on top of the nav gutter.
- **Upstream matrix-sdk bug: duplicate-content gallery/media uploads
  permanently wedge the send queue.** *Mitigated locally:* `sdk/src/upsert_media_store.rs`
  wraps the SQLite media store so `replace_media_key` has upsert semantics (new
  sessions no longer hit it; already-wedged queues are not cleaned up). Drop the
  wrapper once upstream fixes it. `RoomSendQueue`'s
  `update_media_cache_keys_after_upload` (matrix-sdk 0.18.0; still present in
  0.19.1 and upstream `main` as of 2026-10-10, `send_queue/upload.rs`) renames a local media cache placeholder key to
  the real post-upload `mxc://` URI via `MediaStore::replace_media_key`,
  which isn't safe against the destination key already existing. When a
  homeserver's content-addressed dedup returns the *same* `mxc://` URI for
  two separately-queued uploads (trivially reproducible: send two galleries
  containing byte-identical file content, e.g. resending the same test
  images), the second rename hits SQLite's `UNIQUE constraint failed:
  media.uri, media.format`. Worse, `apply_dependent_requests`
  (`send_queue/mod.rs`) never removes a dependent request that errored —
  only on success — so the failed request retries forever, every queue
  tick, persisted across app restarts, spamming
  `matrix_sdk::send_queue: error when applying single dependent request`
  indefinitely. Confirmed via `RUST_LOG=matrix_sdk::send_queue=trace`
  session logs 2026-07-27. Not fixable from this repo (matrix-sdk is a
  pinned Cargo dependency, no local patch applied) — needs an upstream fix
  to `replace_media_key`'s conflict handling (upsert semantics) and/or
  `apply_dependent_requests`' permanent-failure handling (giving up and
  surfacing a real send failure instead of retrying forever). Low
  real-world likelihood (requires resending byte-identical content) but a
  genuine, permanently-stuck failure mode when it hits — worth reporting
  upstream and/or revisiting when matrix-sdk is next bumped past 0.19.1 (re-check `replace_media_key`
  in `matrix-sdk-sqlite` for upsert semantics).
- **Code health — god-object decomposition.** Remaining cuts:
  `MessageListView`'s `TextSelectionModel`, `ReactionChipUI`, `ActionPillUI`
  (woven through `paint_row` + the pointer-dispatch switch; smoke-test
  selection/copy, reactions, and the hover action buttons after each);
  `ShellBase`'s `MediaController`/`MediaCache` (the media-fetch pipeline +
  shared caches — the biggest remaining cut, shell-entangled, flag
  macOS/Windows for recompile); `PaginationRegistry` /
  `SecondaryWindowRegistry` / MSC4278 preview-gating (smaller `ShellBase`
  cuts). 2026-09-07/08 moved reply-quote resolution, pinned-banner refresh,
  compose-draft save/restore, and both the MSC3030 focused-timeline
  initiation and completion-side restore (historical-mode gate,
  scroll-to-focus-event, return-to-live scroll-to-bottom) from `ShellBase`
  onto `RoomPane` — a pop-out that jumps to a date now gets the same
  restore-on-reset behavior the main window always had. Linux (Qt6 + GTK4)
  build + full ctest verified; the compose-draft and MSC3030 wiring changes
  touch all four platform shells' `MainWindow.cpp`/`.mm` files — **Windows
  and macOS still need an on-platform build + smoke test** before this is
  considered fully verified.
- **Polls (MSC3381) follow-ups.** (1) The matrix-sdk-ui timeline does not
  check who sent a poll end event (MSC3381: only the creator or a user with
  redaction power may end it), so any member can close a poll as displayed;
  needs an upstream fix. (2) Stable `m.poll.*` events are not displayed (the
  pinned SDK handles the unstable types only). (3) Thread-panel poll cards are
  read-only; `/poll` from a thread composer or pop-out opens the create dialog
  (titled with the target room) and the poll posts to the room's main timeline. (4) Possible bug:
  `visible_in_room` in `sdk/src/client/thread.rs` may filter out
  `LiveLocation` timeline items from the thread-panel counting predicate (its
  comment names only polls/reactions); verify and fix separately.

## Tier 4 — Open questions, decide-don't-build-yet

- Group calls beyond current MatrixRTC support, Multi-SFU tracking as the
  spec evolves.
- **Notifications, layer 2 server pushers** — Windows deferred (WNS needs
  Store registration; UnifiedPush distributors on Windows are an option);
  macOS deferred (APNs).
- **Room-list window** — `AllRooms` for desktop (recommended), or windowed?
- **Pack-entry encrypted badging** — show a lock glyph on encrypted packs
  in the picker?
