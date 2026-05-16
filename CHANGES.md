# Changelog

Newest first. Unreleased work is listed per day, one bullet per change.
Tagged releases summarize all changes since the previous tag.

## Unreleased

### 2026-05-16

- fix: typing row is always present at fixed height so the layout never shifts when typing starts/stops
- fix: no grouping flicker on new messages — is_cont skips suppressed read marker; suppress_read_marker_ collapses the row until SDK moves it
- fix: crash in newest_visible_real_event_id when typing row is visible — clamp visible_range() last index to messages_.size()-1
- fix(qt6): inline hyperlinks now clickable — change Qt::ExactHit → Qt::FuzzyHit in link_at; add on_link_hovered callback + pointing-hand cursor on Qt6 and GTK4
- feat(ui): room list redesign — last-message preview per row, regular-weight room name, 1px inter-room separators, halved row padding (kPadX 12→6, kPadY 8→4, kRowH 62→48)
- feat(sdk): populate last_message_body from latest cached room event via LatestEventValue
- feat(sdk): add latest_event_body helper (text/image/file/audio/video; local send variants) with 9 unit tests
- fix(win32): snapshot client_ + guard null hCal in openJumpToDateDialog
- fix(views): scroll_to_event_id guards empty id; document set_historical_mode repaint contract
- fix(tk): address toolkit code-review findings
- fix(sdk): remove event cache clear workaround from subscribe_room
- fix(sdk,client): address full-app code-review findings (SDK + client)
- fix(sdk): add topic_html to cfg(test) RoomInfo stub
- fix(room-view): match typing strip background to chat area
- fix(qt6): wire image/sticker/video viewer click callbacks
- fix(qt6): use QTextCursor to query charFormat in link_at
- fix(qt6): image viewer shows full resolution instead of downscaled thumbnail
- fix(qt6): guard QCalendarWidget against pre-epoch dates
- fix(linux-shells): address Qt/GTK shell code-review findings
- fix(linux-gtk): drop GTK3-only gtk_window_set_urgency_hint (GTK4 build)
- fix(join-room): address all code-review findings
- fix: invisible unread badge numbers; style search box like compose input
- fix: crash in draw_elided_line on empty attributed string
- fix(compose): vertically center text in the composer input
- fix(app): relayout before scroll_to_event_id so row_offsets_ are populated
- fix(app): clear stale focused-timeline state on live room selection
- feat(win32): MSC3030 jump-to-date — MonthCal picker + focused timeline callbacks
- feat(views): RoomHeader calendar button + RoomView near-bottom/return-to-live/jump-to-date callbacks
- feat(views): MessageListView scroll_to_event_id + historical pill mode
- feat(room-header): vector-drawn calendar icon
- feat(qt6): wire near-bottom/return-to-live + openJumpToDateDialog with QCalendarWidget
- feat(macos): wire near-bottom/return-to-live + openJumpToDateDialog with NSDatePicker sheet
- feat(login): homeserver discovery with .well-known + inline status
- feat(linux-gtk): GTK4 attention request via GNotification
- feat(gtk4): wire near-bottom/return-to-live + open_jump_to_date_dialog with GtkCalendar
- feat(app): ShellBase focused-timeline state (begin_focused_subscription_, request_forward_history_, return_to_live_)
- feat: clickable inline hyperlinks across all backends
- feat: MSC3266 room summary lookup + join-room dialog
- feat: attention requests when notifications arrive with window visible
- docs: record homeserver discovery, attention requests, hardening pass
- docs: implementation plan + design for vector-drawn room-header calendar icon

### 2026-05-15

- feat(sdk): MSC3030 FFI — timestamp_to_event, subscribe_room_at, paginate_forward
- feat(client): MSC3030 C++ client API — timestamp_to_event, subscribe_room_at, paginate_forward
- feat(tk): add on_near_bottom to ListView (symmetric to on_near_top)
- feat: MSC4230 animated image flag with GIF badge
- feat: MSC3765 rich room topics in the room header
- feat: MSC2010 spoiler rendering and reveal interaction
- feat: MSC2010-compatible message deletion via hover trash button
- feat: overlay scrollbar on GridView (EmojiPicker + StickerPicker)
- feat: insert_at_cursor on NativeTextArea; fix emoji cursor staying put
- refactor: consolidate chat area into shared RoomView widget across all shells
- perf: clip Qt paintEvent to dirty rect; cache dur_lo measure() in voice card
- perf: cache QFont by FontRole in QtFactory; cache avatar clip path by diameter
- perf: cache measure() result in RoomListView chevron/badge callsites
- perf: cache measure() result at hover-button callsites in MessageListView
- perf: optimize AppKit/CoreGraphics rendering
- Win32 D2D flip-model swap chain; dedupe NativeField set_rect; show unread rooms in collapsed sections
- Share Twemoji-first font fallback with Win32 TextRenderer for flag-emoji consistency
- Embed Twemoji Mozilla font on Windows to fix missing emoji flags
- Send fully_read marker alongside public+private read receipts
- Reserve receipt-cluster width during text wrap to prevent overlap
- Render emoji-only messages at 2x body size (BigEmoji font role)
- Rename device after OAuth login; fix Win32 typing-bar visibility
- Four UI fixes: search placeholder, reaction emoji centering, sticker aspect ratio
- fix: Win32 toast notifications not appearing
- fix: verification banner and crash on login
- fix: undeclared kRoomHeaderH in Win32 MainWindow
- fix(tk): guard on_near_bottom re-arm against resize; add nearBottom test
- fix: StickerPicker pack index when favorites tab is hidden
- fix: startup freeze — run UnifiedPush distributor scan off the UI thread
- fix: show 'Already in Saved Stickers' instead of suppressing context menu
- fix(sdk): MSC3030 code review — dedupe streaming tasks, fix from_ffi reached_end
- fix: save_sticker_to_user_pack info_json; Win32 right-click; notify_url_preview_ready
- fix: macOS compilation errors
- fix: Kind::Image rows not resizing when the image loads
- fix: double-text on login view homeserver field
- fix: build warnings and EXC_BAD_ACCESS crash on timeline init
- fix: build — spoiler access error and missing D3D11 test link
- fix: build after upstream RoomView refactor
- fix: avatar clip radius was diameter not diameter/2 in cached QPainterPath
- docs: update CHANGES/STATUS for RoomView refactor, scrollbar, CG opts, Win32 toast, perf
- Add list of MSCs to implement; HTML UI mockup with live style tweaker

### 2026-05-14 (after v0.1.1)

- feat: markdown-to-HTML formatting for sent messages
- feat: MSC4027 custom images in reactions
- feat: MSC2448 BlurHash placeholders for media
- feat: Win32 URL preview — on_url_preview_ready_ + set_preview_provider
- refactor: Qt6 sidebar user strip uses shared UserInfo widget
- Restore last room when switching accounts
- Match compose-bar font to message body text (FontRole::Body)
- Hide sub-spaces from the root room list
- Always show the room search field
- Relicense under GPL v3
- fix: typing indicator hides the bar when no one is typing (Qt6)
- fix: typing indicator hides the bar when no one is typing (GTK4/macOS/Win32)
- fix: RoomListView tests for always-visible search bar
- fix: RoomListView search bar — show only when content overflows
- fix: MSVC build (M_PI); update docs for new features
- fix: image viewer drag — override on_pointer_drag instead of on_pointer_move

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
