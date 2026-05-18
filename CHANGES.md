# Changelog

Newest first. Unreleased work is listed per day, one bullet per change.
Tagged releases summarize all changes since the previous tag.

## Unreleased

### 2026-05-18

- fix(ui/win32): image/video viewer now closes on Esc immediately — `SetFocus` to the top-level window on viewer open so its `WM_KEYDOWN` handler receives Esc (previously Esc did nothing until an app deactivate/reactivate restored top-level focus)
- style: standardise formatting across the whole codebase — reconciled STYLE.md's Rust section to idiomatic same-line braces and narrowed Universal Rule §2 (trivial Rust `match` arms stay unbraced); added a repo-root `.clang-format` (Cpp + ObjC sections) encoding the C++/Objective-C rules; reformatted `sdk/` via `cargo fmt` and all C++/ObjC (`client/`, `ui/shared/`, the four shells, `tests/`) via `clang-format` — behaviour-preserving (323/323 ctest, 92/92 cargo)
- feat(pickers): unified async image cache — EmojiPicker/StickerPicker now share the message-list `tk_images_`/`anim_cache_` on all four shells (Qt6 dropped its private per-picker caches), images route through `media_disk_cache_` so custom emoticons/stickers survive an app restart, and decode runs off the UI thread (Qt6 QImageReader / GTK4 GdkPixbuf+cairo / macOS CGImageSource / Win32 WIC) so first paint of a large pack no longer stalls
- feat(ui): room-list last-message preview now uses `formatted_body`'s first plain line for text/notice/emote; renders "<user> sent an image/video/file/voice message" for those media kinds; and draws a small inline ~28 px thumbnail for sticker last-messages (SDK exposes `last_message_kind` + `last_message_sticker_url`; `RoomListView` gains a `sticker_provider_` backed by the shells' shared image cache, wired in all four shells)

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
