# Features

A reference of what Tesseract currently supports. Spec references (MSC / Matrix
version) are noted where relevant.

## Platforms & architecture

- Native frontends on **four platforms**: Qt6, GTK4, Win32, and macOS
- Shared Rust core built on [matrix-rust-sdk](https://github.com/matrix-org/matrix-rust-sdk); thin per-platform UI shells
- Multi-account support (multiple accounts active simultaneously)
- OS-native secure credential storage on every platform
- Single-instance behavior (launching again restores the running window)
- System tray with minimize-to-tray (default), unread dot, and mention-state color; clicking the tray icon jumps to the first unread room
- macOS dock badge showing the total notification count; clicking the dock icon raises the window and navigates to the first unread room
- Session restore (all open room tabs and active account restored on launch)
- Light / dark / system themes
- System font size inherited from the OS on all four backends (`QApplication::font`, `GtkSettings gtk-font-name`, `NONCLIENTMETRICS`, `NSFont.systemFontSize`); all per-role sizes scale with the user's accessibility font-size setting; Win32 body font raised 1 pt above the raw system size for better readability
- Automatic GitHub release update checker (runs at startup; opt-in via Settings → Privacy)
- In-flight request indicator in the status bar — an animated spinning ring (green / amber / red by threshold) with a tooltip showing the exact in-flight count

## Messaging

- Send, receive, edit, reply, react, and redact
- Markdown formatting — inline (bold, italic, strikethrough, code, links) and block-level (headings, lists, blockquotes, tables)
- Syntax-highlighted code blocks (bidirectional, theme-aware), rendered on a tinted background (single enclosing panel for fenced blocks, inline tint for `code`)
- Message rows show the event timestamp (HH:MM) under the avatar — always on the first message of a group, on hover for continuation rows; same-sender messages within 5 minutes group into continuation rows
- Reactions: Unicode and custom emoji, both send and display
- Custom emoji and stickers via image packs (MSC2545)
- Threads (matrix-rust-sdk thread support)
- Mentions: user mentions with `@` autocomplete, rendered as pills, click-to-profile; `m.mentions` populated (reliable notifications, including in encrypted rooms)
- Read receipts: public (`m.read`) and private (`m.read.private`)
- Fully-read markers (`m.fully_read`)
- Typing indicators
- Timeline day separators and new-message separator
- `m.notice` and `m.emote` (`/me`) handled distinctly from regular messages
- Decryption retry as keys arrive (no permanent "unable to decrypt" placeholders)
- Pinned messages — full support (pin/unpin) with power-level checks
- Slash commands (`/me`, `/shrug`, `/slap`, `/spoiler`, `/myroomnick`, `/myroomavatar`, `/join`, `/leave`, `/invite`, `/gif`; extensible)
- Media captions (MSC2530); emoji-only captions render at 2× body size; bare URLs in a caption are linkified the same as in a regular message body
- Inline Unicode emoji rendered at ~125% of body font size in message bodies, the composer (live as-you-type), and the room list's last-message preview
- Location messages render an embedded pannable/zoomable map; clicking (not panning) opens the location on openstreetmap.org
- An opt-in setting (default off) surfaces room join/leave/kick/ban/invite/knock events in the timeline, with consecutive same-action events collapsed into one expandable summary line

## Media

- Images, video, voice messages, audio, and files — send and receive
- GIF search and send via `/gif` (Klipy-powered; E2EE-transparent; animated preview strip in picker)
- Encrypted media supported
- Animated GIF / WebP / APNG in the message timeline
- Animated stickers, including stickers sent via bridges
- Zoomable / pannable image viewer
- Inline audio player; voice messages render waveforms (MSC3245); a voice message auto-advances to the next one from the same sender in the room when it finishes playing on its own
- Thumbnail-first loading in the timeline, with an optional automatic full-media fetch setting
- Media preview gating (MSC4278): Off / Private / On modes with BlurHash placeholders for suppressed media; click-to-reveal; user's own uploads are always shown
- Video thumbnails generated via native platform APIs (AVFoundation / Media Foundation / GStreamer)
- Image sending via clipboard paste and drag-and-drop
- File / image / video downloads
- Visible-first media loading: the media for rows currently on screen downloads ahead of the off-screen backlog and re-prioritizes as you scroll; a few stuck downloads can't freeze the queue
- URL previews (fetched via the homeserver)

## Rooms & navigation

- Room list with sections: Favorites, DMs, Rooms, Spaces (tag-aware: `m.favourite`); spaces show a collapsible "Not joined" sub-section for unjoined child rooms
- A phone icon appears on rooms with an active call
- A 🌉 Bridged badge appears in the room-info panel for rooms bridged to a third-party network (MSC2346)
- Sticky section headers — the current section's header pins to the top while scrolling (interactive: click to collapse/expand)
- Space navigation with drill-down and recursive subspace support; unjoined child rooms shown with a preview panel (name, avatar, topic, member count, Join button); selecting a joined space itself shows a summary panel (avatar, topic, joined/unjoined child counts)
- Multiple rooms open in tabs
- Pop-out room windows (ctrl/⌘+click a tab to open the room in its own native window)
- Quick switcher (ctrl/⌘+K command palette to jump between rooms, with a recently-visited strip)
- Back / forward room history navigation (Alt+Left / Alt+Right; ⌘[ / ⌘] on macOS)
- Automatic grouping of inactive rooms (configurable inactivity threshold)
- Jump-to-date via a calendar button in the room header (MSC3030; server capability checked); shared `DatePickerView` across all four platforms
- Full-text message search across all rooms, including encrypted (**Ctrl+Shift+F** / **⌘⇧F**) — see Security & privacy for details
- Unread indicators in the room list: a semibold room title plus a count badge for notifying rooms (accent-colored for mentions), or a small dot for rooms with unread messages that don't notify (e.g. "mentions only"); muted rooms are excluded
- Auto-scroll the room list to the most-recent unread room when new messages arrive — spaces count when any child room is unread; excludes low-priority/inactive rooms; optional (Appearance setting, default on)
- Last-message previews, including image and sticker previews
- Room search (filters by room display name)
- Direct messages (create / open; reuses existing DM if present)
- Room settings: edit the room's avatar, display name, and topic (per-field power-level gated); staged edits aren't sent until confirmed

## Notifications

- Native desktop notifications on all platforms
- Unified Push on Linux
- Notification content includes text, images, and stickers
- Clicking a notification opens the relevant room
- Per-room notification settings (mute / mentions / all) via server-side push rules
- Respects the user's server-side push rules

## Calls

- Native LiveKit-based MatrixRTC voice/video calls (MSC4143), behind `TESSERACT_ENABLE_CALLS`; interoperates with Element X and Element Call
- End-to-end encryption (HKDF key derivation matching Element Call's wire format); echo cancellation via each platform's native audio device manager
- Docked, expanded, floating (draggable, position persisted), and popout (dedicated OS window) call overlay modes
- Mute/video/hang-up controls, call duration timer, pinned-participant grid layout
- Screen sharing, with real per-source thumbnails in the picker (native platform capture: DXGI Desktop Duplication / PrintWindow on Windows, ScreenCaptureKit on macOS, xdg-desktop-portal + PipeWire on Linux)
- Mute, video-mute, and screen-share state persist across Docked/Floating/Popout overlay mode switches
- MSC4075 ring notifications for incoming calls
- Call button and incoming-call banner hidden when the server doesn't advertise LiveKit transport support, or for bridged rooms

## Security & privacy

- End-to-end encryption
- Guided encryption setup for new accounts (cross-signing wizard)
- Device verification via emoji (SAS)
- Key backup recovery
- Room key export / import (standard interoperable format)
- **Full-text message search** across all rooms, including E2EE (**Ctrl+Shift+F** / **⌘⇧F**): a local SQLite FTS5 index of decrypted message bodies in `search_index.db`. Opt-in (Settings → Privacy → Search, off by default); enabling lazily backfills history and indexes every subsequent message; disabling clears the index. Results show room · sender · snippet; clicking jumps to the message. Settings panel shows live stats (message count, room count, oldest indexed date, on-disk size).
- Privacy controls, including presence send/poll toggles
- Device & session management (list sessions with verified status, sign out remote sessions)
- Undecryptable-message states surfaced in the UI
- Cryptographic identity reset
- Clear-cache action (excludes the crypto/session store)

## Account & profile

- Login and logout (SDK-based logout removes the device server-side)
- QR-code login (MSC4108; gated on server capability advertisement)
- Profile editing: display name, avatar, and extended fields — pronouns, timezone, and biography (MSC4133)
- Multi-account

## Settings

- Account
- Sessions / devices
- Notifications (per-room)
- Appearance (light / dark / system theme; room-list inactive grouping; auto-scroll to unread rooms)
- Privacy (presence controls; search index toggle with live stats and on-disk size; update-checker opt-in)
- Media (automatic full-media fetch; microphone/speaker/camera device selection)
- About (version, with branded view)

## Composer

- Markdown input
- Emoji shortcode autocomplete popup
- User mention (`@`) autocomplete
- Slash commands (`/me`, `/shrug`, `/slap`, `/spoiler`, `/myroomnick`, `/myroomavatar`, `/join`, `/leave`, `/invite`, `/gif`, `/selfie`; popup autocomplete)
- Send on Enter, newline on Shift+Enter

---

## Not yet implemented

- **Room administration**: creating rooms, editing history visibility / join rules, inviting users, and moderation actions (kick / ban / power levels) — editing name/topic/avatar is implemented
- **Room directory browsing**
- **Global default notification level** (per-room settings work; global default planned)
- **Cross-signing setup for brand-new accounts** {confirm: existing accounts with cross-signing already initialized work fine}
- **Accessibility**: screen-reader support is incomplete
- **Localization**: English only (i18n architecture in place)
- **Background push on macOS / Windows** (Linux uses Unified Push; in-app notifications elsewhere)
- **Spaces management** beyond navigation (creating / editing space structure)
- **3PID management**, **account deactivation**, **identity server settings**

## Possible / planned polish

- Window position/size/maximized state restore
- Room mentions (`#room`) as pills; self-mention emphasis
- New-device warnings for users in your rooms
- Device rename
- Edit history viewer
- Additional slash commands
