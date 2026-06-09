# ROADMAP.md

Completed work is in [CHANGES.md](CHANGES.md). What follows is only the pending and in-progress work.

## Code health — god-object decomposition (in progress)

The 2026-06-09 pre-launch pass began breaking up the two largest classes; nine
collaborators have been extracted so far (`TimelineMediaController`,
`SpoilerRevealer`, `ReadReceiptTracker`, `LocationMapPanner`,
`TimelineVideoPlaylist`, `RoomSwitchGateKeeper`, `UrlPreviewCardDisplay`,
`LinkLayoutCache` from `MessageListView`; `ThreadPanelController` from
`ShellBase`). Remaining, with seam maps + recipe in
[`docs/TODO-phase5-remaining.md`](docs/TODO-phase5-remaining.md):

- **`MessageListView` cross-cutting cuts** — `TextSelectionModel`, `ReactionChipUI`,
  `ActionPillUI` (woven through `paint_row` + the pointer-dispatch switch; smoke-test
  selection/copy, reactions, and the hover action buttons after each).
- **`ShellBase MediaController`/`MediaCache`** — the ~1,300-LOC media-fetch pipeline +
  shared caches; the biggest cut, shell-entangled (flag macOS/Windows for recompile).
- **`PaginationRegistry` / `SecondaryWindowRegistry` / MSC4278 preview-gating** — smaller
  ShellBase cuts.

## Step 5 — UI redesign (in progress)

Done: inline images, stickers, reply-to, message editing, voice messages (receive + send), ComposeBar, read receipts (display + sending, overlay only — never expand rows), hover timestamps, day separators, typing indicators, inline bold/italic/code/strikethrough via `formatted_body`, URL previews + hyperlinks (Qt6, GTK4, Win32), Markdown-to-HTML for sent messages, last-message preview in sidebar (regular-weight room name, 1px inter-room separator, compact row sizing), emoji reactions (reaction chips, toggle, `send_reaction` / `redact_reaction` FFI), slash-command popup (`/` autocomplete, `/me` / `/shrug` / `/slap` / `/spoiler`), pinned-message banner + pin/unpin action, hover action pill, tab session restore, threads panel (list + open states, in-panel `ComposeBar`), @mention autocomplete + pills (`m.mentions`), encryption-setup overlay (guided cross-signing wizard). Remaining:

- **Message bubbles / cards** — visual polish pass on the message layout.

## Step 8 — MSC2545 phase A: remaining items

- **Inline emoticons in HTML message bodies** — render `<img data-mx-emoticon ...>` in `formatted_body` instead of alt text. Per-platform: Qt `QTextDocument::addResource`, GTK `GtkTextChildAnchor`, macOS `NSTextAttachment`, Win32 via RichEdit overlay (Step 8b).
- **Win32 shell wiring** — `StickerPicker` child `WS_POPUP` surface and the underlying RichEdit overlay (Step 8b). Right-click "Add to Saved Stickers" is done (`tk::win32::Surface::set_on_right_click` + `WM_RBUTTONUP`).

## Step 8b — Win32 RichEdit inline media overlay

- New `tk::InlineMediaSurface` abstraction: per-row optional native overlay (no-op default). Win32 impl: per-row `RichEdit 4.1` (`MSFTEDIT.DLL`) HWND pooled by `event_id`.
- `IRichEditOleCallback` + `OleCreatePictureIndirect` over WIC-decoded `HBITMAP` to insert images/stickers as OLE objects.
- `MessageListView` publishes inline-media rects via `set_on_inline_media_layout`; overlay `SetWindowPos`es children in the same layout pass as `NativeTextArea`. `Surface::supports_inline_media_overlay()` gates the skip-canvas-paint path (returns false on Qt6 / GTK4 / macOS).
- LRU pool keyed by `event_id`, hard cap ~32 active children, `SWP_HIDEWINDOW` for offscreen rows. Fail-safe fallback to canvas paint when `MSFTEDIT.DLL` is absent.

## Step 9 — MSC2545 phase B: send (remaining)

- **`send_emoticon_message(room_id, plain_body, html_body)` FFI** + composer integration to emit `<img data-mx-emoticon ...>` HTML body when a custom emoticon is picked (instead of `:shortcode:` plain-text fallback).

## Step 10 — MSC2545 phase C: pack management (remaining)

- List enabled packs UI; toggle subscription via `im.ponies.emote_rooms` (room settings drill-in).
- Pack creation / removal flow; sticker delete/rename inside the user pack; manual order/sort.

## Step 12 — Notifications, layer 2: server pushers

Linux (Qt6 + GTK4) done — the toggle is now also wired to `IUpConnector::set_enabled` so enabling/disabling notifications in Settings registers/removes the server pusher live. Remaining:

- Windows: deferred (WNS needs Store registration; UnifiedPush distributors on Windows are an option).
- macOS: deferred (APNs).

## Settings — Sessions tab follow-ups

The "Sessions" tab landed with list + verification badges + this-device marker + sign-out via UIAA fallback. Remaining UI work:

- **Inline rename of device display name.** FFI/Client/Controller already plumbed end-to-end (`Client::set_device_display_name`, `SettingsController::rename_device`, `on_device_renamed`). Needs a per-row `NativeTextField` overlay: either (a) extend `DevicesSection` with a `rename_field_rect()` analogous to `AccountSection::name_field_rect()` and route each shell's existing single `NativeTextField` over the active row, or (b) add a `tk::Host::prompt_text(...)` dialog primitive backed by `QInputDialog` / `GtkDialog` / `MessageBoxW` / `NSAlert`.
- **Out-of-band verification trigger.** Each row could carry a "Verify" button that fires `request_self_verification()` and pops the SAS overlay focused on the chosen device.

## Known gaps

- **`m.location` send not yet implemented** — receive + display is done (see CHANGES.md 2026-05-17); composing and sending location messages is out of scope for this iteration.
- **`TestSurface` doesn't cover CoreGraphics** — QPainter, Cairo, and D2D are tested; macOS CGBitmapContext surface is still TODO.
- **`tk_avatars_` / `tk_images_` not keyed by `(user_id, mxc)`** — cosmetic ghosting risk when two accounts share an mxc URL that resolves to different bytes.
- **DM-counterpart avatar picks the bridge bot itself when the bridge doesn't publish `io.element.functional_members` (MSC4171)** — heisenbridge currently lacks the state event, so 1:1 control rooms show the bot's own avatar. Workaround: ship a small allow-list of known bridge user-ID prefixes (`@heisenbridge:`, `@_neb_`, etc.) on the Rust side, or contribute the missing state-event publication upstream to heisenbridge.
- **i18n not wired on macOS (`NSLocalizedString`) or Win32 (`LoadString`)**.
- **Notification preview image is fetched as a full file, not a thumbnail** — image/sticker notification previews now work (incl. a lock-screen privacy gate via `IScreenLock`), but the SDK downloads the full media (≤ 2 MiB cap) on the sync handler regardless of whether the C++ layer will display it (it can't see window-focus / lock state). Fix: use `MediaFormat::Thumbnail` and skip the fetch when the notification won't be shown. The macOS + Linux (Qt6/GTK4) `IScreenLock` impls and notifier-render paths still need on-device smoke tests (built only on Win32 here).
- **GTK4 message-list CSS not theme-aware** — every `tk` surface and pop-out window now follows the theme setting, but `apply_theme_ui_()` only rebuilds the `.sidebar` / `.sidebar-separator` CSS rules. `.sender-name` / `.timestamp` / `.room-header` / `.room-header-topic` and the `status_bar_` / `topic_tooltip_label_` `GtkLabel`s keep hardcoded light colours. Fix: rebuild all theme CSS rules from `t.palette` and give the status / tooltip labels palette-driven CSS classes.
- **Native context menus / dialogs unthemed on Win32 + Qt6** — Win32 `TrackPopupMenu` / `MessageBoxW` and Qt6 `QMenu` use the OS / default palette, so right-click menus and message boxes don't follow the in-app dark/light theme. (macOS follows via `NSApp.appearance`; GTK4 via `gtk-application-prefer-dark-theme`.) Fix: owner-draw the Win32 menus and apply the theme palette to the Qt `QMenu`s.

## Decisions still open

- **Timeline persistence** — opt in to sqlite-backed `Timeline::with_focus(...)`, or memory-only?
- **Room-list window** — `AllRooms` for desktop (recommended), or windowed?
- **Pack-entry encrypted badging** — show a lock glyph on encrypted packs in the picker?
