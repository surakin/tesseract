# Changelog

Newest first. Unreleased work is listed per day, one bullet per change.
Tagged releases summarize all changes since the previous tag.

## Unreleased

## v0.1.4 — 2026-05-21

Changes since v0.1.3:

- fix(shutdown): route `LoginView` homeserver-discovery thread and `UpConnector` endpoint-scan through `ShellBase::run_async_` so they are drained before destruction. Two detached `std::thread`s were still live at teardown — `hs_changed_` (300 ms debounce for `discover_homeserver`) and the UP scan thread — causing malloc corruption in `~MessageListView` after `accounts_.clear()` had already freed the client.
- fix(qt6): reset `QMediaPlayer` source between voice/audio clips — `setSourceDevice` short-circuits on an unchanged pointer and replayed the first clip's FFmpeg stream for every subsequent play. Fixed by clearing the source and resetting the `QBuffer`/`QByteArray` before each `play()` call.
- feat(notifications): wire the Notifications toggle to the SDK pusher — `IUpConnector::set_enabled` added so Linux UnifiedPush connectors can remove or re-register their homeserver pusher when the toggle changes; routed through `SettingsController` instead of inline per-shell persistence so all four platforms share the same path. Local OS-notification suppression via `Settings::notifications_enabled` is unchanged.
- feat(settings): "Hide message content in notifications" privacy toggle — when enabled, outgoing notifications title becomes "Tesseract", body becomes "New message", and avatar + image bytes are cleared regardless of the image-preview setting; useful when screensharing. Gate centralised in `ShellBase::apply_notification_redaction_()`. Persisted in `tesseract::Settings`.
- feat(ui): click avatar in `UserProfilePanel` or `RoomInfoPanel` to open full-resolution image in lightbox — fetches original via `fetch_source_bytes`; lightbox image provider falls back to `tk_avatars_` thumbnail for instant display while bytes are in flight; applied to GTK4 / Win32 / macOS shells in the same pass.
- feat(settings): bottom-pinned "About" tab with brand splash — `SideTabView::add_bottom_tab()` anchors a tab group to the sidebar's lower edge, separated from the regular tabs by a thin divider; the About page embeds `BrandView` (app icon, name, version).
- fix(settings): route `SettingsController` worker threads and GTK4 `on_logout` callback through `ShellBase` drain to prevent use-after-free at shutdown; fix `run_async` call-site in GTK4 shell.
- feat(header): show padlock icon next to room name for encrypted rooms — 10×12 px vector padlock drawn to the left of the room name in `RoomHeader` using `text_secondary` colour; name label width reduced so ellipsis truncation still works on long names; condensed mode unaffected.
- feat(ui): hide reaction chip counter when only one person has reacted — chip shows emoji only; count shown from two reactors onward.
- feat(settings): Log Out button at the bottom of the Account settings page — destructive-variant button opens the shared `ConfirmDialog` overlay before firing `on_logout`; `SettingsView` owns its own `ConfirmDialog` instance since it runs on a separate surface.
- fix(timeline): surface undecryptable messages as a single muted line instead of dropping them. The Rust converter in [sdk/src/client.rs](sdk/src/client.rs) now matches `MsgLikeKind::UnableToDecrypt(EncryptedMessage)`, maps the contained `UtdCause` to a single-line padlock message via the new `utd_message_for_cause` helper, and emits a `TimelineEvent { msg_type: "m.utd", body: <reason> }`. The C++ side adds `EventType::Utd` + `UtdEvent` + a `"m.utd"` dispatch arm in `ffi_convert.h`, and `MessageListView` renders the row muted via the existing `paint_wrapped_text` path the redacted tombstone uses. Delete and reaction-add affordances are suppressed on UTD rows; reply still works. Covers all 9 `UtdCause` variants — `SentBeforeWeJoined`, `VerificationViolation`, `UnsignedDevice`, `UnknownDevice`, `HistoricalMessageAndBackupIsDisabled`, `WithheldForUnverifiedOrInsecureDevice`, `WithheldBySender`, `HistoricalMessageAndDeviceIsUnverified`, and the catch-all `Unknown`. New `matrix-sdk-base` direct dep so the converter can name `UtdCause` (matrix-sdk doesn't re-export the crypto types). 1 new Rust test + 1 new C++ test. 118 Rust tests + 412 C++ tests
- fix(sdk): stop polling presence for users that return 403 Forbidden — bridge puppet accounts (e.g. `@_twitterpuppet_*`) often have presence privacy enabled and return `403 Forbidden { kind: Forbidden }` on every `GET /presence/{userId}/status` poll. The polling task now records forbidden user IDs in an `Arc<Mutex<HashSet<OwnedUserId>>>` local to the task and skips them on subsequent ticks. Detected via `HttpError::client_api_error_kind() == Some(ErrorKind::Forbidden)` (matches the existing UIA error-kind pattern used by `delete_devices`); other transient errors (404, 5xx, network) stay retriable. New `is_presence_forbidden` helper + 3 unit tests. 117 Rust tests + 411 C++ tests
- feat(presence): publish outgoing Matrix presence — pairs with the receive-side dots shipped in `d652c95`. New `PresenceTracker` (shared) runs a deadline-driven FSM: Online while engaging with the app, Unavailable after 5 min of no input + no window focus, Offline on logout. Each shell wires (1) `tk::Host::set_on_user_activity` for pointer/wheel events, (2) a focus tap via `changeEvent` (Qt6) / `notify::is-active` (GTK4) / `WM_ACTIVATE` (Win32) / `windowDidBecomeKey,Resign` (macOS), (3) a 30 s `notify_presence_tick_` timer, and (4) `notify_presence_logout_` before `client_->logout()`. Transitions publish via `Client::set_presence(PresenceState)` → new `set_presence` FFI → `ruma::api::client::presence::set_presence::v3::Request` on a detached worker thread (fire-and-forget). 8 PresenceTracker unit tests + 2 Rust set_presence byte-mapping tests. 114 Rust tests + 411 C++ tests
- feat(settings): device list in Settings — new "Sessions" tab lists every device on the account with display name, device id, last-seen IP/time, cross-signing verification badge, and a "This device" marker for the current session. Sign out walks the Matrix UIAA fallback-URL flow: a 401 challenge from `delete_devices` is detected via `as_uiaa_response()`, the row offers "Open in browser" pointing at the spec'd `/_matrix/client/v3/auth/<stage>/fallback/web?session=…` URL (built locally via `build_uia_fallback_url` + `urlencoding_encode_segment`, 4 unit tests), and "I've confirmed" retries with `AuthData::FallbackAcknowledgement{session}`. Verification state is cross-referenced from `client.encryption().get_device(...)`. Rename plumbing (`set_device_display_name` FFI + `Client::set_device_display_name` + `SettingsController::rename_device`) is in place but the per-row inline-edit UI is deferred to a follow-up. New: `DeviceFfi`/`DeleteDeviceBegin` FFI structs, `Client::Device`/`DeviceVerification`/`DeleteDeviceBegin` types, `SettingsController::{load_devices,delete_device,confirm_device_deletion,rename_device}` + per-device in-flight mutex, `DevicesSection` view with DeviceRow chips/action buttons/UIA state machine, `tk::Widget::clear_children` helper. 112 Rust tests + 401 C++ tests
- feat(ui): DM-counterpart avatar fallback — rooms with no avatar of their own now show the other participant's avatar in the room list, room header, room-info panel, and tab strip. Computed in `build_room_infos` (Rust) and piped through a new `dm_avatar_url` field on `RoomInfo` + a `effective_avatar_url()` accessor on the C++ struct; `ensure_room_avatar_` routes the DM fallback through `fetch_media_bytes` (user mxc) instead of the room-avatar endpoint. Functional members (`io.element.functional_members` / MSC4171) are excluded so bridged 1:1s show the puppet's avatar, not the bot's — the same filter also fixes `get_or_create_dm` recognising existing bridged DMs. Counterpart selection prefers `Room::direct_targets()` (m.direct, cheap and in-memory) and falls back to filtering joined members only when `active_members_count - service_members_count == 2`, so large group rooms incur zero extra cost; 380→382 C++ tests, 107/108 Rust tests (the lone failure is a pre-existing image-packs ordering test unrelated to this change)
- feat(client): server capabilities on login — `tesseract::ServerInfo` struct (homeserver URL, spec versions, MSC3030/Jump-to-Date support, `can_change_password`/`can_set_displayname`/`can_set_avatar`, default room version); fetched concurrently via `/_matrix/client/versions` + `/_matrix/client/v3/capabilities` after `RoomListState::Running`; stored in `ShellBase::server_info_` for feature-gating; Settings "Server" tab shows the homeserver URL; 363/363 C++ tests, 108/108 Rust tests
- feat(auth): per-platform secure token storage — `SecretStore` backend using Windows Credential Manager, macOS Keychain, and Linux `libsecret` (plaintext stub fallback when absent); `SessionStore` migrates transparently from legacy plaintext `session.json` on first load (sentinel `{"v":2}` written on success); 357/357 C++ tests

## v0.1.3 — 2026-05-19

Changes since v0.1.2:

- feat(download): save file attachments, images, and videos to disk — clicking a file card in the timeline opens a native save dialog and fetches the file from the Matrix media server; a ⬇ button added to `ImageViewerOverlay` and `VideoViewerOverlay` does the same for displayed images and videos; all four platforms (Qt6 `QFileDialog`, GTK4 `GtkFileChooserNative`, Win32 `GetSaveFileNameW`, macOS `NSSavePanel`); bytes fetched off the UI thread via `fetch_media_bytes` / `fetch_source_bytes` and written with `std::ofstream`; 328/328 ctest, 96/96 cargo
- feat(voice): MSC3245 voice message send — mic button in `ComposeBar` starts/stops recording; OGG/Opus encoding in Rust (`audiopus` + `ogg` crates) at 48 kHz mono 32 kbps with MSC1767 waveform sampling (up to 256 samples, normalised [0, 1000]); live waveform strip animates during recording; cancel button; per-platform `tk::AudioCapture` backend (Qt6 `QAudioSource`, GTK4 GStreamer `pulsesrc`, Win32 WASAPI, macOS `AVAudioEngine`); `send_voice` FFI + `Client::send_voice` C++ API; mic button hidden automatically when no capture device is available; voice send wired in all four main shells via new `ShellBase::wire_voice_capture_()` helper; pop-out secondary windows hide the mic button (recording is a singleton interaction owned by the main window); 3 new Rust unit tests + 5 new C++ tests (328/328 ctest, 95/95 cargo)
- fix(tests): update two `ComposeBar` attachment tests to match current floating-preview design — image previews now float above the bar without changing `natural_height()`; only file chips grow it
- fix(ui/win32): image/video viewer now closes on Esc immediately — `SetFocus` to the top-level window on viewer open so its `WM_KEYDOWN` handler receives Esc (previously Esc did nothing until an app deactivate/reactivate restored top-level focus)
- style: standardise formatting across the whole codebase — reconciled STYLE.md's Rust section to idiomatic same-line braces and narrowed Universal Rule §2 (trivial Rust `match` arms stay unbraced); added a repo-root `.clang-format` (Cpp + ObjC sections) encoding the C++/Objective-C rules; reformatted `sdk/` via `cargo fmt` and all C++/ObjC (`client/`, `ui/shared/`, the four shells, `tests/`) via `clang-format` — behaviour-preserving (323/323 ctest, 92/92 cargo)
- feat(pickers): unified async image cache — EmojiPicker/StickerPicker now share the message-list `tk_images_`/`anim_cache_` on all four shells (Qt6 dropped its private per-picker caches), images route through `media_disk_cache_` so custom emoticons/stickers survive an app restart, and decode runs off the UI thread (Qt6 QImageReader / GTK4 GdkPixbuf+cairo / macOS CGImageSource / Win32 WIC) so first paint of a large pack no longer stalls
- feat(ui): room-list last-message preview now uses `formatted_body`'s first plain line for text/notice/emote; renders "`<user>` sent an image/video/file/voice message" for those media kinds; and draws a small inline ~28 px thumbnail for sticker last-messages (SDK exposes `last_message_kind` + `last_message_sticker_url`; `RoomListView` gains a `sticker_provider_` backed by the shells' shared image cache, wired in all four shells)
- refactor(shell): de-duplicated the four native shells into ui/shared/ — ShellBase gains `build_rows_` (+ `prep_row_media_` hook), concrete account-prefs (active-account gate) and image-packs handlers, the `cached_emoticons_` member, and secondary-window dispatch helpers; new shared `tesseract::text::trim` and `ScreenLockState`; `RoomWindowBase::wire_room_view_` absorbs the per-shell secondary-window RoomView wiring behind `surface_repaint_`/`compose_text_area_`/`preview_lookup_` hooks. Behaviour-preserving (323/323 ctest, 92/92 cargo); the UnifiedPush `UpConnectorCore` hoist was deliberately deferred — Qt6/GTK4 endpoint-normalisation are not behaviour-equivalent.

## v0.1.2 — 2026-05-18

Changes since v0.1.1:

- feat(messaging): `m.location` / MSC3488 receive — location messages render as interactive 240 px inline maps; OSM tiles fetched from `tile.openstreetmap.org` with disk cache; pan by drag, zoom by scroll wheel; attribution overlay; red-circle pin at event coordinates; all four platforms (Qt6, GTK4, Win32, macOS)
- feat(messaging): optimistic send via SDK local echo — `send_message` switches to `timeline.send()` so sent messages appear immediately with a ◷ (sending) indicator; transitions to ✓ for 2 s once the server confirms delivery; recoverable failure shows ⚠ + "Retry" (re-enables the SDK send queue); unrecoverable failure shows ⚠ + ✕ (aborts via `timeline.redact`); `retry_send` / `abort_send` FFI + C++ client API added
- feat(compose): emoji shortcode expansion — typing `:abc` shows a suggestion popup above the cursor with matching Unicode emoji and MSC2545 custom emoticons; Up/Down navigate rows, Enter/click accepts, Escape dismisses; completing a full `:shortcode:` token auto-expands inline; all four platforms
- feat(app): secondary room windows — `open_room_in_new_window(room_id)` opens any room in its own native window; raise-existing policy prevents duplicates; `RoomWindowBase` shared C++ base handles lifecycle, SDK subscription ref-counting, and event dispatch; all four shells dispatch events to open secondary windows
- feat: light/dark/system theme preference — all four shells detect OS appearance and honour a persisted `ThemePreference`; Win32 picks up `WM_SETTINGCHANGE`, macOS `effectiveAppearance`, Qt6 `QPalette::ColorScheme`, GTK4 `GtkSettings::gtk-application-prefer-dark-theme`
- feat(notify): image & sticker notification previews with a lock-screen privacy gate — notifications now embed the message's image or sticker picture (2 MiB cap, decrypts E2EE, fail-safe to avatar); cross-platform `tesseract::IScreenLock` suppresses previews when the screen is locked; stickers now notify at all (`m.sticker` push handler added); second settings checkbox added and persisted on all four shells
- feat(pickers): emoji + sticker picker shortcode tooltips — `GridView` hover tracking fixed (cells now highlight on mouse-over); `EmojiPicker` and `StickerPicker` draw an inline `:shortcode:` tooltip centred above the hovered cell, clipped to picker bounds
- feat(ui): room list redesign — last-message preview per row, regular-weight room name, 1 px inter-room separators, halved row padding
- feat(matrix): `m.notice` renders with muted colour; `m.emote` renders as "* SenderName body" with italic spans — both support `formatted_body`, spoilers, URL cards, links, and reactions
- feat: MSC3030 jump-to-date — all four shells wire MonthCal/QCalendarWidget/GtkCalendar/NSDatePicker with focused timeline; RoomHeader calendar button
- feat: MSC3266 room summary lookup + join-room dialog
- feat: MSC3765 rich room topics in the room header
- feat: MSC4230 animated image flag with GIF badge
- feat: MSC2010 spoiler rendering and reveal interaction; message deletion via hover trash button
- feat: homeserver discovery with `.well-known` + inline status on the login screen
- feat: attention requests when notifications arrive with window visible
- feat(image-viewer): oversized images open zoomed to fit rather than 1:1
- feat(ui): BrandView shows the application icon (embedded PNG from `tesseract.svg`)
- feat(app): single-instance enforcement on all four shells
- feat(sdk): `latest_event_body` helper with 9 unit tests; `last_message_body` populated via `LatestEventValue`
- fix(linux): Wayland foreground activation via XDG portal — Qt6 and GTK4 notifiers switch to `org.freedesktop.portal.Notification` on Wayland; the portal's `ActionInvoked` signal (xdg-desktop-portal 1.16+) carries a compositor-validated `xdg_activation_v1` token; Qt6 sets `_q_waylandActivationToken` on `QWindow`; GTK4 calls `gtk_window_set_startup_id()` — window is now reliably raised when a notification action is invoked
- fix(qt6): dark theme detection on GNOME — Qt6 shell queries `org.freedesktop.portal.Settings` at startup and subscribes to `SettingChanged`; falls back to portal value when `QStyleHints::colorScheme()` returns `Unknown` (common on GNOME without QGnomePlatform / Qt < 6.6)
- fix(compose): markdown→HTML on send centralised in `Client::send_message/send_reply/send_edit` for all shells + secondary windows — Win32, macOS, and pop-out windows now convert markdown; duplicated per-shell calls removed; added `test_markdown.cpp` test coverage
- fix(msc2545): prefer merged stable image-pack event types (`m.room.image_pack`, `m.image_pack.rooms`) with unstable fallback; personal pack is `im.ponies.user_emotes` only (no fabricated stable type); `set_account_data_both` dual-write helper added for future pack management
- fix(auth): reject duplicate account sign-in on all four shells
- fix(notify/qt6): fresh popup per notification (stale replaces-id was collapsing all toasts); `image-path` hint correctly populated; stale `id_to_room_` entries cleaned up on `NotificationClosed`
- fix(win32): swallow stale `WM_CHAR('\r')` after Enter submits the compose bar to prevent phantom newline in next reply/edit session
- fix(win32): `NativeTextArea::natural_height()` measures soft-wrapped lines via `DrawTextW(DT_CALCRECT | DT_WORDBREAK …)` — compose box now auto-grows without a keystroke lag
- fix(viewer): image/video overlay backdrop black-on-first-move fixed on all four platforms via transparent Surface/Host propagation
- fix(gtk4): `NativeTextArea` placeholder text via `dim-label` `GtkLabel` overlay
- fix(gtk4): async image fetch for `EmojiPicker` custom emoticon tabs
- fix(map): `on_tile_needed` wired in all four primary shell main-window files (was only wired for secondary pop-out windows); zoom rate capped at one step per wheel notch; wheel events intercepted only inside the painted map rect; location description shown as tooltip on hover
- fix(settings): settings surface receives the current theme immediately on creation
- fix(ui): emoji glyphs centred within their picker grid cells
- fix(message-list): bare URLs in plain-text bodies are now clickable
- fix(reply): pass `event_id` (not `in_reply_to_id`) to reply detail resolver
- fix(sdk): build room timeline on a worker thread to avoid stack-overflow crash on macOS
- fix(win32): compile cleanly under `/std:c++20` on SDK 19041
- feat: overlay scrollbar on GridView (EmojiPicker + StickerPicker)
- feat: MSC4027 custom images in reactions; MSC2448 BlurHash placeholders for media
- feat: clickable inline hyperlinks across all backends
- feat: markdown-to-HTML formatting for sent messages
- Relicense under GPL v3

## v0.1.1 — 2026-05-14

Changes since v0.1:

- URL previews + inline hyperlink rendering (OpenGraph card; row-height invalidation on arrival)
- UnifiedPush server pusher for Linux / Step 12 (Qt6 + GTK4): D-Bus connector, endpoint rewrite to `/_matrix/push/v1/notify`, Register/Unregister signature + deadlock fixes, stop/logout split
- Typing indicators: send `m.typing` and display incoming
- Read receipts: public `m.read` send; mark rooms read on open (`m.read` + `m.read.private`)
- Inline markdown rendered from `formatted_body`; day separators + virtual timeline items
- Emoticon image loading, picker tab scrolling, compose-bar height fix
- Sticker fixes: saved-pack visibility, aspect ratio, right-click viewer, dedupe-by-URL on save
- Qt6: transparent native text overlays
- Fix use-after-free crash when selecting a room; fix two pre-existing test failures
- Three UI fixes: compose icons inside input, search clear button, play triangle

## v0.1 — 2026-05-14

First tagged release. All work up to v0.1, by area:

### Core / SDK

- Initial C++/Rust scaffold; renamed to Tesseract; OAuth/MAS loopback login; CLAUDE.md + Catch2 test framework
- matrix-sdk 0.16.1 → 0.17.0; refresh-token handling; tokio Drop context for client swap
- Step 2 — sliding sync (SyncService + RoomListService) replacing `sync_once`; per-room Timeline map; subscribe/unsubscribe/paginate FFI
- SQLite-backed timeline persistence via EventCache
- Session: SessionStore restore-or-login; full PersistedSession on token refresh; OAuth flush on `stop_sync`; UnknownToken / soft_logout handling; auto-recover from sync `State::Error`; infinite-reconnect-loop fixes
- Multi-account support (infrastructure + all four shells); per-account notifier; restore last room on switch
- Prefs stored as Matrix account-data (`im.gnomos.tesseract`)
- `ShellBase` + `EventHandlerBase` refactor extracted from all four shells
- Background backfill (all rooms, limited to visible); cancellable media fetches; shutdown-stability hardening (worker drain, double-callback fix, `panic_in_cleanup` fixes)
- i18n for Qt6 + GTK4; device rename to hostname after OAuth (`device_display_name`)

### UI toolkit (`tesseract_tk`)

- Shared `tk::Canvas` / `tk::Widget` / `tk::Host` toolkit across all four platforms (Direct2D / QPainter / Cairo / CoreGraphics)
- Native text overlays; ListView scrollbar drag; `tesseract::Settings`; CoreGraphics + D2D test surfaces
- macOS port: AppKit → Mac Catalyst → native AppKit
- DirectWrite colour-emoji text on Windows; Twemoji fallback

### Features

- Message rows: sender identity + avatars, grouping, day separators, redactions, editing, reply-to + scroll-to-original, reactions + MSC4027 custom-image reactions, read receipts + hover timestamps, typing indicators
- Media: inline images/stickers, MSC2545 image packs (sticker/emoji pickers, encrypted, animated GIF/WebP/APNG), MSC3245 voice messages, `m.video` receive + playback, `m.file` drag-and-drop, clipboard image paste + MSC2530 captions
- Text: Markdown→HTML on send; inline markdown render from `formatted_body`; URL previews + inline hyperlinks
- Navigation: spaces drill-in; room search + 500 ms debounce + activity sort; collapsible room-list sections; favorites
- Encryption: device verification (SAS) + key-backup recovery (Step 6); recovery + verification banners
- Notifications: Linux (Qt6/GTK4 D-Bus), macOS (UNUserNotificationCenter), Win32 (WinRT toast); system tray + minimize-to-tray on all four; UnifiedPush server pusher (Linux)

### Build & packaging

- App icons (Win32 `.ico` / macOS `.icns` / GTK4 / Qt6) generated from a shared SVG
- CPack installers — NSIS (Windows) + DMG (macOS); Debian + Arch packaging helpers
- MinGW cross-compile support; `WHOLE_ARCHIVE` link for the 3-way FFI cycle; bundled SQLite (rustls, no system OpenSSL)
