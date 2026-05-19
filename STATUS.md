# Tesseract — Implemented Features

Snapshot of every feature that has landed on `master`. Last updated **2026-05-19**.

> **Wayland foreground activation.**
> Both the Qt6 and GTK4 notification backends now use the XDG Desktop Portal
> (`org.freedesktop.portal.Notification`) on any Wayland session, not only inside
> Flatpak. The portal's `ActionInvoked` signal (xdg-desktop-portal 1.16+) carries an
> `xdg_activation_v1` token granted by the compositor when the user clicks the
> notification; Qt6 passes it via `QWindow::setProperty("_q_waylandActivationToken")`
> and GTK4 via `gtk_window_set_startup_id()`, so GNOME Shell now reliably brings the
> window to the foreground. 323/323 C++ tests pass.

For build instructions, architectural overview, and the open-roadmap items, see [CLAUDE.md](CLAUDE.md). For tracked open issues / known gaps, see the "Known gaps" section at the bottom of CLAUDE.md.

## Test coverage

| Suite | Count |
| ----- | ----- |
| Rust unit tests (`cargo test -p tesseract-sdk-ffi`) | 95 |
| C++ Catch2 tests via ctest (Qt6 preset) | 328 |

## Platforms

| Shell | UI toolkit | Canvas backend | Status |
| ----- | ---------- | -------------- | ------ |
| Linux | Qt6 Widgets | QPainter | primary dev target — verified end-to-end |
| Linux | GTK4 | Cairo + Pango | verified end-to-end |
| macOS | AppKit (`NSWindowController`, `NSView`) | CoreGraphics + CoreText | verified on macOS 15; opus playback requires macOS 14+ |
| Windows | Win32 + COM | Direct2D + DirectWrite + WIC | MSVC verified; MinGW cross-compile verified; audio via IMFMediaEngine |

---

## Authentication & session

- **OAuth 2.0 (RFC 8252) loopback flow** — two-phase `begin_oauth` / `await_oauth` API, ephemeral loopback HTTP server, mDNS-safe redirect URI.
- **Session restore on startup** — `SessionStore` persists the full `PersistedSession` JSON on every token refresh (`%APPDATA%/Tesseract/` on Windows, `~/.config/tesseract/` on Linux, `~/Library/Application Support/Tesseract/` on macOS) and reloads it at launch.
- **`logout`** — wipes Rust session, C++ wrapper state, and the SQLite store; surfaces back through the FFI.
- **Soft logout** — `SessionChange::UnknownToken` threaded through `on_error` with a `soft_logout` flag so the UI can retry restore without clearing the store.
- **Recovery key / device verification (Step 6)** — `needs_recovery`, `recover(key_or_passphrase)`, `backup_state` FFI; `on_backup_progress` callback; per-platform `RecoveryBanner` (in-toolkit; not a modal dialog).
- **Shutdown stability** — background workers are drained before the tokio runtime tears down, preventing use-after-free when a worker posts back to the UI thread after the EventHandler is destroyed; a separate guard prevents a double-callback segfault when `stop_sync` is called re-entrantly.
- **Identity strip in sidebar** — circular avatar + display name + right-click "Log Out" on every platform.
- **Single-instance enforcement** — a per-user OS lock prevents two app instances from running concurrently (`QLockFile` on Qt6, `GApplication` uniqueness on GTK4, a named mutex on Win32, `NSRunningApplication` check on macOS); the second launch exits with a notice.
- **Duplicate account guard** — after OAuth completes the shell checks existing `accounts_` for a matching `user_id` before committing to disk; re-adding the same account discards the temp store and returns to the last active account without side effects.

## Sync & rooms

- **Sliding sync via matrix-sdk-ui** — `SyncService` + `RoomListService` replace the legacy `sync_once` loop.
- **Initial-sync progress in the status bar** — `RoomListService::state` exposed via a new `on_room_list_state` FFI callback; each shell paints "Syncing rooms…" (debounced 300 ms) / "Reconnecting…" / "Downloading encryption keys (N)…" until both sliding-sync and key-backfill settle, then clears to "Connected". Wired on Qt6, GTK4, and Win32; macOS deferred (no status-bar surface).
- **Per-room `Timeline` handles** — `HashMap<OwnedRoomId, TimelineHandle>` keyed by room; subscribed lazily.
- **Timeline FFI** — `subscribe_room`, `unsubscribe_room`, `paginate_back`, `paginate_back_with_status` (reports `reached_start`); position-aligned `on_timeline_reset` / `on_message_inserted` / `on_message_updated` / `on_message_removed` callbacks mirror matrix-sdk-ui's `VectorDiff` semantics.
- **Back-pagination on scroll-to-top** — UI fires `paginate_back` when the user reaches the top; in-place insertion preserves the visual scroll position.
- **Background backfill** — `start_background_backfill` walks every joined room not currently subscribed and warms the persistent event cache with bounded concurrency.
- **Kind-aware last-message preview** — each room row's preview uses `formatted_body`'s first plain line for text/notice/emote, shows "<user> sent an image/video/file/voice message" for media kinds, and draws an inline ~28 px thumbnail for sticker last-messages (`RoomListView` `sticker_provider_` backed by the shells' shared image cache; wired on all four platforms).
- **Tombstoned (upgraded) rooms hidden** from the room list.
- **Graceful shutdown** — `Drop` on `ClientFfi` calls `stop_sync()`.

## Spaces (Step 7)

- `is_space: bool` on `RoomInfo`; spaces shown at the bottom of the room list with `#` prefix on Qt6 / GTK4 (top-row dedicated bar on macOS).
- `space_children(space_id)` FFI returning joined direct children.
- **Stack-based drill-in navigation** — selecting a space replaces the room list with its children; `←` back button + space name label at the top of the sidebar; recursive sub-spaces; auto-pop to "All rooms" when the stack is empty.
- **Space children hidden from the root room list** — they appear only when navigating into the parent.

## Shared UI toolkit (`tesseract_tk`)

- **`tk::Canvas`** — abstract 2D backend with four concrete impls (`canvas_d2d`, `canvas_qpainter`, `canvas_cairo`, `canvas_cg`). Color / Rect / Point / Image / TextLayout primitives; rounded-rect, stroke, push/pop clip; circle-cropped image draw; initials disc helper.
- **`tk::Widget`** — measure / arrange / paint + pointer / wheel dispatch with `dispatch_pointer_down` + `world_to_local` capture semantics.
- **`tk::Host`** — per-platform integration surface (repaint scheduling, post-to-UI, native edit overlays). `request_repaint`, `post_to_ui`, `make_text_field`, `make_text_area`, `make_audio_player`, `make_audio_capture`, `encode_for_send`.
- **Native text overlays** — `NativeTextField` (`QLineEdit` / `GtkEntry` / Win32 EDIT / `NSTextField`) and `NativeTextArea` (`QTextEdit` / `GtkTextView` / multi-line EDIT / `NSTextView`) for IME-friendly input. `set_placeholder` is implemented on all four platforms (GTK4 uses a `dim-label` `GtkLabel` overlay child since `GtkTextView` has no native placeholder API).
- **Shared views** — `LoginView`, `RoomListView`, `MessageListView`, `EmojiPicker`, `StickerPicker`, `RecoveryBanner`, `ComposeBar` mounted identically on every platform.
- **Drag-and-drop ingest** — `FileDropHandler` on every Surface; image-data MIME types route to the compose bar's image preview, generic files route to the file chip.

## Messaging

- **Send text / image / file / sticker** — `send_message`, `send_image`, `send_file`, `send_sticker` FFI; matrix-sdk handles E2EE transparently. Text sends use `timeline.send()` local echo so the message appears immediately with a ◷ indicator; transitions to ✓ on delivery, ⚠ + Retry on recoverable failure, ⚠ + ✕ on unrecoverable failure. `retry_send` (re-enables SDK send queue) and `abort_send` (`timeline.redact` for local echoes) exposed through FFI and C++ client API.
- **MSC2530 captions** — `image_filename` distinct from `body` round-tripped; UI shows the body beneath the image only when the sender supplied an explicit `filename`.
- **Redactions** — `redact_event(room_id, event_id, reason)`; `MsgLikeKind::Redacted` surfaces as `msg_type: "m.redacted"` tombstone placeholder in the timeline.
- **Reactions** — `send_reaction` (toggle) FFI; aggregated reaction chips under each message with sender-name tooltips and a hover-only "+" add button.
- **Replies (`m.in_reply_to`)** — `in_reply_to_id` / `in_reply_to_sender_name` / `in_reply_to_body` extracted in `timeline_item_to_ffi`; quote block rendered above the message body in `MessageListView`; hover "↩ Reply" button fires `on_reply_requested`; `ComposeBar` grows a reply-preview banner (`kReplyBandH = 44 px`) above the text input with a "×" cancel; `send_reply` FFI sends an `m.text` with `Relation::Reply`; reply relation threaded through image/file sends via `AttachmentConfig::reply`; click on a quote block scrolls to the original message in-list or fires `on_scroll_to_original` when not loaded; all 4 shells wired.
- **Message editing** — `send_edit` FFI wraps `room.make_edit_event()` + `send_queue().send()`; `is_edited` field in `TimelineEvent` set from `msg_content.is_edited()`; `(edited)` badge appended after the body in `MessageListView`; hover "✏" button on own text messages fires `on_edit_requested`; `ComposeBar` grows an edit-mode banner (`kEditBandH = 44 px`) above the text input with a "×" cancel and `on_send_edit` callback; edit mode and reply mode are mutually exclusive (`set_editing` clears reply state); all 4 shells wired.
- **Location messages (`m.location` / MSC3488) receive** — location events render as interactive 240 px inline maps; OSM tiles fetched from `tile.openstreetmap.org` and composited with a disk cache; pan by drag, zoom by scroll wheel (one notch = one zoom level); attribution overlay; red-circle pin at event coordinates; location description shown as a hover tooltip. `on_tile_needed` wired in all four primary shell `MainWindow` files. Send is not yet implemented.
- **Read receipts** — `EventTimelineItem::read_receipts()` aggregated via a `collect_read_receipts` helper; `MessageListView` paints up to 5 mini-avatar discs (16 px) with a `+N` overflow pill at the row's bottom-right.
- **Hover-only `HH:MM` timestamp** — paints under the sender avatar when the row is hovered; no always-visible time column.
- **MSC2545 sticker decryption** — encrypted-sticker support via direct `ruma = { features = ["compat-encrypted-stickers"] }`; sticker timeline events emit JSON-encoded `MediaSource` for the encrypted variant.

## Media

- **`fetch_media_bytes(mxc)`** / **`fetch_source_bytes(source_json)`** — synchronous wrappers around matrix-sdk's media cache; the latter handles plain mxc + encrypted `EncryptedFile` transparently.
- **Avatars** — sender avatars (24 px per row) + room avatars (36 px); circular crop via `draw_circle_image`; initials-disc fallback when bytes aren't yet cached.
- **Inline images** — thumbnail to max 320 × 200, MSC2530 caption rule applied, rounded-rect chrome.
- **File cards** — fixed 56-px-tall rounded card with filename (ellipsis-trimmed) + human-readable size.
- **Inline stickers** — borderless 256 × 256 thumbnail; right-click context menu offers "Add to Saved Stickers" (Qt6 / GTK4 / macOS).
- **Animated images** — GIF / APNG / animated WebP frame-by-frame decoding on Qt6 (`QImageReader`), GTK4 (`GdkPixbufAnimationIter`), Win32 (`IWICBitmapDecoder` + per-frame metadata), macOS (`CGImageSource`). 60 Hz frame tick repaints when any frame advances; delays clamped ≥ 20 ms.
- **Homeserver upload limit** — `media_upload_limit()` cached per session.
- **Clipboard image paste + drag-drop** in the compose bar; image data re-encoded to JPEG ≤ 1600 × 1200 when sent via `encode_for_send(compress=true)`.

## Voice messages (MSC3245)

- **Receive path** — `MessageType::Audio` arm in `timeline_item_to_ffi` (gated on `unstable-msc3245-v1-compat`); voice events surface as `msg_type = "m.voice"` carrying `audio_source_json`, `audio_duration_ms`, `audio_waveform` (MSC1767, 0..=1024), `audio_mime`. Plain `m.audio` (no voice marker) folds through the file-card path.
- **C++ `VoiceEvent`** + `EventType::Voice`.
- **Voice card UI** — 280 × 48 rounded card with play/pause circle, waveform strip (flat placeholder bars when waveform is omitted), mm:ss remaining-time label.
- **Scrubbable waveform** — click or drag anywhere on the bars to seek; clicking on a non-active row starts playback at the chosen position.
- **Speed pill** — `1×` / `1.5×` / `2×` on the active row; cycles the global playback rate.
- **Background prefetch** — each shell kicks off a worker thread when a Voice row is first seen, warming the SDK media cache so the first play tap is instant.
- **Per-platform `tk::AudioPlayer` backend** — Qt6 `QMediaPlayer` + `QAudioOutput`; GTK4 GStreamer pipeline (`giostreamsrc` ! `decodebin` ! `audioconvert` ! `autoaudiosink`); macOS `AVAudioPlayer`; Win32 `tk::audio_win32.cpp` using `IMFMediaEngine` — in-memory `IStream` avoids disk spillage; 60 ms timer-pool tick drives progress; callbacks marshalled back to the UI thread via `post_to_ui`.
- **Send path** — mic button in `ComposeBar` starts/stops recording; cancelled via a dedicated cancel button. OGG/Opus encoding in Rust (`audiopus` + `ogg` crates) at 48 kHz mono; MSC1767 waveform sampled every ~100 ms of PCM (up to 256 samples, normalised [0, 1000]). Live waveform strip in the compose bar animates during recording. Per-platform `tk::AudioCapture` backend: Qt6 `QAudioSource`, GTK4 GStreamer `pulsesrc` pipeline, Win32 WASAPI (`IAudioCaptureClient`), macOS `AVAudioEngine` (async permission request to avoid main-thread deadlock). `send_voice` FFI + `Client::send_voice` C++ API. Mic button hidden automatically when no capture device is available (factory returns `nullptr`). Voice recording wired in all four main shells via `ShellBase::wire_voice_capture_()`; pop-out secondary windows hide the mic button — recording is a singleton interaction owned by the main window. Room ID is captured at the moment recording starts so room switches during a long recording send to the correct room.

## Pickers

- **Emoji picker** — Unicode-category tabs + per-pack custom tabs; search; virtualised grid via `tk::GridView`. Hovering a cell shows an inline `:shortcode:` tooltip (centred above the cell, flipped below near the top edge).
- **Sticker picker** — Favorites tab + per-pack tabs; search; virtualised grid. Floating panel on every platform (Qt6 `QFrame`, GTK4 `GtkPopover`, macOS `NSPanel`, Win32 `WS_POPUP` HWND). Same `:shortcode:` hover tooltip as emoji picker.
- **GridView hover tracking** — `GridView::on_pointer_move` / `on_pointer_leave` update `hovered_index_` and expose `hovered_index()` + `rect_at()` accessors; cell highlight on hover now works correctly (was silently broken).
- **Recent emoji (MSC4356)** — `m.recent_emoji` + `io.github.johennes.msc4356.recent_emoji` account-data, dual-written on every bump; reads stable → unstable → legacy `io.element.recent_emoji` so existing Element users keep their picker rank. 100-entry cap, move-to-front-and-increment semantics, count-desc top-N for the Frequents tab.
- **Add to Saved Stickers** — right-click on an inline sticker offers `save_sticker_to_user_pack` (all four platforms: Qt6 / GTK4 / macOS / Win32 via `WM_RBUTTONUP` + `TrackPopupMenu`). All platforms now pass the real `ImageInfo` JSON instead of `"{}"`, so width/height/mimetype/size are preserved in the saved pack entry.
- **Toggle favourite** — `toggle_favorite_sticker` flips the `im.tesseract.favorite` flag on user-pack entries.
- **Async sticker image fetch** — Win32 + Qt6 + macOS + GTK4 all run a worker thread + decode + post-to-UI + cache + repaint per pending sticker. GTK4 also wires the same async path for `EmojiPicker` custom emoticon tabs (`ensure_emoji_image_async`, deduped via `emoji_fetches_in_flight_`).
- **Unified async picker image cache** — `EmojiPicker` and `StickerPicker` now share the message list's `tk_images_` / `anim_cache_` on all four shells (Qt6 dropped its private per-picker caches), so picker artwork and inline-message artwork are decoded once and reused. Images route through `media_disk_cache_`, so custom emoticons and stickers survive an app restart, and decode runs off the UI thread (Qt6 `QImageReader`, GTK4 `GdkPixbuf` + cairo, macOS `CGImageSource`, Win32 WIC) so the first paint of a large pack no longer stalls the UI.

## MSC2545 image packs

- **`sdk/src/image_packs.rs` aggregator** — user pack (`im.ponies.user_emotes` / `m.image_pack`), enabled-rooms list (`im.ponies.emote_rooms` / `m.image_pack.rooms`), per-room state events (`im.ponies.room_emotes` / `m.room.image_pack`). 16 unit tests.
- **Spec-correct usage semantics** — missing/empty `usage` → both sticker + emoticon allowed; per-image `usage` overrides pack-level.
- **FFI surface** — `list_image_packs`, `list_pack_images`, `list_favorite_stickers`, `send_sticker`, `save_sticker_to_user_pack`, `user_pack_has_sticker`, `toggle_favorite_sticker`.
- **`IEventHandler::on_image_packs_updated`** — fires whenever the cache is rebuilt; pickers refresh in place.

## Compose bar

- Shared `tesseract::views::ComposeBar` on every platform via `tk::*::Surface`.
- Multi-line expanding input via `tk::NativeTextArea` (auto-grows 56 → 160 px, clamped).
- Send-on-Enter, Shift+Enter inserts a newline.
- Emoji + sticker + send buttons painted by the toolkit.
- Send button gates on trimmed non-empty content.
- Clipboard image paste; file drag-drop; pending-image / pending-file preview chip with clear button.
- Reply-mode banner (`kReplyBandH = 44 px`) with sender + body snippet and "×" cancel; edit-mode banner (`kEditBandH = 44 px`) with "×" cancel; both modes mutually exclusive.

## Internationalisation

- **Qt6** — all shell strings wrapped with `QObject::tr()`; `QTranslator` loads `share/translations/tesseract_<locale>.qm` at startup. `i18n_extract_qt` CMake target (guarded by `find_program(lupdate)`) runs `lupdate src/ -ts i18n/qt/tesseract_LANG.ts` to produce a translation template.
- **GTK4** — all shell strings wrapped with `_(s)` = `gettext(s)`; `bindtextdomain("tesseract", share/locale)` + `textdomain` called in `main()`. `i18n_extract_gtk` CMake target runs `xgettext` to produce `i18n/gtk/tesseract.pot`.
- Shared views (`ui/shared/views/`) stay in English — translated via each platform's mechanism when strings are passed in by the host.
- macOS (`NSLocalizedString`) and Win32 (`LoadString`) not yet wired.

## Theme

- **`ThemePreference`** — persisted user preference (`Light` / `Dark` / `System`); `set_theme()` added to every platform `Surface`; `apply_current_theme_()` in `ShellBase` applies the selected palette.
- **OS appearance detection** — each shell overrides `os_color_scheme_()` to return `ThemeMode::Dark` or `ThemeMode::Light`:
  - **Win32** — `WM_SETTINGCHANGE` with `"ImmersiveColorSet"` parameter.
  - **macOS** — `effectiveAppearance` checked on theme-change notification.
  - **GTK4** — `GtkSettings::gtk-application-prefer-dark-theme` property.
  - **Qt6** — `QStyleHints::colorSchemeChanged` signal; falls back to the XDG Desktop Portal (`org.freedesktop.portal.Settings`, namespace `org.freedesktop.appearance`, key `color-scheme`) when `QStyleHints::colorScheme()` returns `Unknown` (GNOME without QGnomePlatform or Qt < 6.6). The portal value is read at startup and kept current via the `SettingChanged` D-Bus signal.
- **Live updates** — all four shells re-apply the theme whenever the OS switches, provided `ThemePreference::System` is active. User-pinned Light or Dark is never overridden by OS changes.

## System tray

- **All four platforms** — system-tray icon with **Show App** / **Quit** popup menu. Closing the main window hides it (the SDK keeps running, sync stays warm); Quit on the tray menu does the real exit.
- Cross-platform `tesseract::ITrayIcon` abstraction; per-platform impls created after login (mirrors `INotifier`).
- **Qt6** — `QSystemTrayIcon`; `is_available()` from `QSystemTrayIcon::isSystemTrayAvailable`. Falls back to plain quit when no system tray is present.
- **GTK4** — `libayatana-appindicator3` (probed via `org.kde.StatusNotifierWatcher`). Built in a separate `tesseract_gtk_tray` static lib so GTK3 headers stay isolated from the GTK4 shell. Requires `libayatana-appindicator3-dev`.
- **Win32** — `Shell_NotifyIcon` against a hidden helper HWND; `TrackPopupMenuEx` for the right-click menu; `WM_CLOSE` intercepted in `MainWindow`'s wnd_proc.
- **macOS** — `NSStatusItem` with a template menu-bar icon; `windowShouldClose:` hides the window; Quit calls `[NSApp terminate:nil]`.

## Notifications (foreground toasts)

- Cross-platform `tesseract::INotifier` / `Notification` abstraction; per-platform impls created after login.
- Push-rule evaluation via `evaluate_push_rules` in `sdk/src/client.rs`; fires on `VectorDiff::PushBack` (live events only); `is_mention` from `Action::is_highlight()`.
- **Win32** — WinRT `Windows.UI.Notifications.ToastNotificationManager`; `ToastGeneric` XML with sender, optional room name (omitted for DMs), 120-char body preview; `WM_TESSERACT_NOTIFY_CLICK` navigates to the room. AUMID registered in `HKCU\Software\Classes\AppUserModelId\` at startup (required for non-packaged apps); `notify()` wrapped in `try`/`catch(winrt::hresult_error)` for robustness.
- **Qt6** — `QDBusInterface` against `org.freedesktop.Notifications`; replace-per-room; Flatpak portal path supported; click navigates + raises window.
- **GTK4** — `GDBusConnection` (session bus); same replace-per-room and Flatpak portal patterns as Qt6.
- **macOS** — `UNUserNotificationCenter`; `UNUserNotificationCenterDelegate` on `MainWindowController`; in-foreground suppression when the source room is active; click navigates to the room.
- **Image & sticker previews** — `m.image` / `m.sticker` notifications embed the message picture (SDK fetch, 2 MiB cap, E2EE-transparent; a dedicated `m.sticker` push handler — stickers are a distinct event type). Win32 large inline `<image>` + circular avatar `appLogoOverride`; macOS `UNNotificationAttachment`; Linux single image slot. Gated by the `notification_image_previews` setting.
- **Lock-screen privacy gate** — cross-platform `tesseract::IScreenLock` (Win32 WTS, macOS `com.apple.screenIsLocked`, Linux logind `LockedHint`); `ShellBase::notification_image_allowed_()` strips the picture whenever the screen is locked (avatars are not gated).
- **Wayland foreground activation** — Qt6 and GTK4 notifiers use `org.freedesktop.portal.Notification` whenever `WAYLAND_DISPLAY` is set; the portal's `ActionInvoked` signal carries an `xdg_activation_v1` token that is passed to the compositor before calling `activateWindow()` / `gtk_window_present()`, enabling reliable window focus on GNOME Shell and other strict Wayland compositors.
- All platforms suppress the notification when the window is focused and the target room is already open.

## Build & packaging

- **Corrosion** fetched at configure time (no global Rust toolchain install requirement beyond `rustup`).
- **`WHOLE_ARCHIVE` link** for the 3-way circular dependency between `tesseract_sdk_bridge_cxx`, `tesseract_client`, and `tesseract_sdk_ffi-static`.
- **Cross-platform CMake presets** — `windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-qt6-debug`, `linux-qt6-release`, `macos-appkit-{arm64,x86_64}-{debug,release}`.
- **CPack installer packaging** — NSIS on Windows, DMG on macOS (see [PACKAGING.md](PACKAGING.md)).
- **Bundled SQLite** via matrix-sdk's `bundled-sqlite` feature; no system OpenSSL dep (TLS uses rustls).

---

## Maintenance note

Update this file after every major feature lands — append a new bullet (or extend an existing one) in the right category, refresh the test counts in the table at the top, and bump the "Last updated" date.
