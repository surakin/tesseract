# ROADMAP.md

Completed work is in [CHANGES.md](CHANGES.md). This file lists only pending
and in-progress work, as a single backlog ordered by priority/urgency.

## Tier 1 — Finish what's in flight (don't start new things until these are done)

- **Room settings**: confirm the lockout-guard fix is fully tested
  (self-lockout and last-other-admin-lockout, per the earlier discussion),
  and finish out any remaining fields flagged as not-yet-done (room alias
  display, leave-room reachable from settings, etc., if those weren't in
  the initial version).
- **Calls**: the real testing tail — more cross-network/cross-platform
  combinations, lifecycle edge cases (drops, rejoins), now with the TWIM
  post out there potentially recruiting real testers. This is now partly
  not solely on us, which is good — but it needs engagement with whatever
  testers show up in the room.
- **Screen sharing**: same testing-tail treatment, at whatever level of
  priority given it's explicitly YMMV/rougher than calls.
- **Legacy username/password login (m.login.password).** Un-parked as a
  fallback for self-hosted homeservers without an OIDC/MAS provider. Gated
  behind a new build-time flag, `TESSERACT_ENABLE_LEGACY_LOGIN` (default
  `ON`), modeled on `TESSERACT_ENABLE_CALLS`. Session storage is now a tagged
  `SessionEnvelope{OAuth, Native}` so both auth mechanisms share
  `restore_session`/`export_session`/`logout`. `LoginView` auto-detects via
  homeserver discovery and shows both the OAuth button and username/password
  fields whenever detection is inconclusive. GTK4/Qt6 build-verified (1163
  ctest passing, both `=ON` and a from-scratch `=OFF` configuration). Still
  need: a Windows/macOS build check (edits mirror the verified Linux shells
  but weren't compiled), and a real end-to-end test against a self-hosted
  Synapse with no OIDC/MAS configured, including the refresh-token behavior
  on a server without MSC2918 support (flagged as unverified from source
  alone during design).
- **File drop / drag-hover widget-tree dispatch on macOS/Windows.** The
  per-Surface `FileDropHandler` callback was replaced with `tk::Widget`
  virtuals (`on_file_drop`/`on_drag_hover`) so each drop target claims its
  own drop and paints its own hover highlight, also fixing native text
  fields swallowing file drags on Qt6/macOS/GTK4. Verified on Linux (GTK4 +
  Qt6, full test suite, confirmed working on-platform); macOS and Windows
  compile against the same shared code but haven't been built/run here —
  need an on-platform drag-drop smoke test on both (compose bar, room
  editor, personal pack editor, drag-hover highlight).

## Tier 2 — The room-admin cluster, still incomplete

- Room creation (public/private) — the most-missed capability, not yet built.
- Invite UI (member list → invite), beyond the `/invite` slash command.
- Member list with moderation actions (kick/ban) — related to but distinct
  from the power-levels editor already built.
- Room directory browsing.
- **DM-counterpart avatar picks the bridge bot itself** when the bridge
  doesn't publish `io.element.functional_members` (MSC4171) — heisenbridge
  currently lacks the state event, so 1:1 control rooms show the bot's own
  avatar. Workaround: ship a small allow-list of known bridge user-ID
  prefixes (`@heisenbridge:`, `@_neb_`, etc.) on the Rust side, or
  contribute the missing state-event publication upstream to heisenbridge.

## Tier 3 — Smaller deferred items, pick opportunistically

- Global default notification level (per-room exists; global doesn't).
- Cmd/Ctrl+K refinements, room mentions as pills (vs. just user mentions),
  self-mention emphasis, device rename, new-device warnings, edit history
  viewer, GIF picker.
- Room upgrades, alias management beyond viewing.
- **MSC2545 pack management — manual order/sort.** List/subscribe UI, pack
  creation/removal, and sticker delete/rename inside the user pack all
  shipped 2026-07-11 (global "Emojis & Stickers" settings tab +
  per-room/space editor). No way to reorder packs or images within a pack
  yet.
- **Message bubbles / cards** — visual polish pass on the message layout.
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
- **`m.location` send** — receive + display is done; composing and sending
  location messages hasn't been built.
- **Notification preview image is fetched as a full file, not a
  thumbnail** — the SDK downloads the full media (≤ 2 MiB cap) on the sync
  handler regardless of whether the C++ layer will display it (it can't
  see window-focus / lock state). Fix: use `MediaFormat::Thumbnail` and
  skip the fetch when the notification won't be shown. The macOS + Linux
  (Qt6/GTK4) `IScreenLock` impls and notifier-render paths still need
  on-device smoke tests (built only on Win32 here).
- **GTK4 message-list CSS not theme-aware** — `apply_theme_ui_()` only
  rebuilds the `.sidebar` / `.sidebar-separator` CSS rules. `.sender-name` /
  `.timestamp` / `.room-header` / `.room-header-topic` and the
  `status_bar_` / `topic_tooltip_label_` `GtkLabel`s keep hardcoded light
  colours. Fix: rebuild all theme CSS rules from `t.palette` and give the
  status / tooltip labels palette-driven CSS classes.
- **Native context menus / dialogs unthemed on Win32 + Qt6** — Win32
  `TrackPopupMenu` / `MessageBoxW` and Qt6 `QMenu` use the OS / default
  palette, so right-click menus and message boxes don't follow the in-app
  dark/light theme. (macOS follows via `NSApp.appearance`; GTK4 via
  `gtk-application-prefer-dark-theme`.) Fix: owner-draw the Win32 menus and
  apply the theme palette to the Qt `QMenu`s.
- **`tk_avatars_` / `tk_images_` not keyed by `(user_id, mxc)`** — cosmetic
  ghosting risk when two accounts share an mxc URL that resolves to
  different bytes.
- **`TestSurface` doesn't cover CoreGraphics** — QPainter, Cairo, and D2D
  are tested; macOS CGBitmapContext surface is still TODO.
- **Code health — god-object decomposition.** Remaining cuts:
  `MessageListView`'s `TextSelectionModel`, `ReactionChipUI`, `ActionPillUI`
  (woven through `paint_row` + the pointer-dispatch switch; smoke-test
  selection/copy, reactions, and the hover action buttons after each);
  `ShellBase`'s `MediaController`/`MediaCache` (the media-fetch pipeline +
  shared caches — the biggest remaining cut, shell-entangled, flag
  macOS/Windows for recompile); `PaginationRegistry` /
  `SecondaryWindowRegistry` / MSC4278 preview-gating (smaller `ShellBase`
  cuts).

## Tier 4 — Open questions, decide-don't-build-yet

- Group calls beyond current MatrixRTC support, Multi-SFU tracking as the
  spec evolves.
- Flathub distribution (flagged as the likely most-requested Linux
  packaging addition).
- matrix.org client-list PR — awaiting review, nothing to do but respond
  if asked.
- **Notifications, layer 2 server pushers** — Windows deferred (WNS needs
  Store registration; UnifiedPush distributors on Windows are an option);
  macOS deferred (APNs).
- **Room-list window** — `AllRooms` for desktop (recommended), or windowed?
- **Pack-entry encrypted badging** — show a lock glyph on encrypted packs
  in the picker?

## Tier 5 — The big structural gaps, acknowledge and schedule loosely, don't start soon

- Accessibility (screen reader support) — large, its own project, not a
  quick add.
- Localization beyond English/Spanish — content work, opportunistic/
  contributor-driven.
- **i18n not fully wired on macOS or Win32** — both shells use `tk::tr()`
  for the shared views, but a handful of native-menu strings still go
  through `NSLocalizedString`/raw literals instead of the shared catalog
  (e.g. the AppKit context-menu "Copy" item); Win32 has no `LoadString`
  usage at all yet for anything outside the shared views.
