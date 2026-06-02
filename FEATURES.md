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
- Session restore (all open room tabs and active account restored on launch)
- Light / dark / system themes
- In-flight request indicator in the status bar (green / amber / red dot with tooltip showing the count)

## Messaging

- Send, receive, edit, reply, react, and redact
- Markdown formatting (send and receive)
- Syntax-highlighted code blocks (bidirectional, theme-aware)
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
- Slash commands (`/me`, `/shrug`, `/slap`, `/spoiler`; extensible)
- Media captions (MSC2530)

## Media

- Images, video, voice messages, audio, and files — send and receive
- Encrypted media supported
- Animated stickers, including stickers sent via bridges
- Zoomable / pannable image viewer
- Inline audio player; voice messages render waveforms (MSC3245)
- Thumbnail-first loading in the timeline, with an optional automatic full-media fetch setting
- Video thumbnails generated via native platform APIs (AVFoundation / Media Foundation / GStreamer)
- Image sending via clipboard paste and drag-and-drop
- File / image / video downloads
- URL previews (fetched via the homeserver)

## Rooms & navigation

- Room list with sections: Favorites, DMs, Rooms, Spaces (tag-aware: `m.favourite`)
- Space navigation with drill-down and recursive subspace support
- Multiple rooms open in tabs
- Automatic grouping of inactive rooms (configurable inactivity threshold)
- Jump-to-date via a calendar button in the room header (MSC3030; server capability checked)
- Unread indicators in the room list
- Last-message previews, including image and sticker previews
- Room search (filters by room display name)
- Direct messages (create / open; reuses existing DM if present)

## Notifications

- Native desktop notifications on all platforms
- Unified Push on Linux
- Notification content includes text, images, and stickers
- Clicking a notification opens the relevant room
- Per-room notification settings (mute / mentions / all) via server-side push rules
- Respects the user's server-side push rules

## Security & privacy

- End-to-end encryption
- Guided encryption setup for new accounts (cross-signing wizard)
- Device verification via emoji (SAS)
- Key backup recovery
- Room key export / import (standard interoperable format)
- Privacy controls, including presence send/poll toggles
- Device & session management (list sessions with verified status, sign out remote sessions)
- Undecryptable-message states surfaced in the UI
- Clear-cache action (excludes the crypto/session store)

## Account & profile

- Login and logout (SDK-based logout removes the device server-side)
- Profile editing (display name, avatar)
- Multi-account

## Settings

- Account
- Sessions / devices
- Notifications (per-room)
- Appearance (light / dark / system theme)
- Privacy (presence controls)
- Media (automatic full-media fetch)
- About (version, with branded view)

## Composer

- Markdown input
- Emoji shortcode autocomplete popup
- User mention (`@`) autocomplete
- Slash commands (`/me`, `/shrug`, `/slap`, `/spoiler`; popup autocomplete)
- Send on Enter, newline on Shift+Enter

---

## Not yet implemented

- **Room administration**: creating rooms, editing room settings (name, topic, avatar, history visibility, join rules), inviting users, and moderation actions (kick / ban / power levels)
- **Room directory browsing**
- **Global default notification level** (per-room settings work; global default planned)
- **Cross-signing setup for brand-new accounts** {confirm: existing accounts with cross-signing already initialized work fine}
- **Message search across rooms**
- **Accessibility**: screen-reader support is incomplete
- **Localization**: English only (i18n architecture in place)
- **Background push on macOS / Windows** (Linux uses Unified Push; in-app notifications elsewhere)
- **Spaces management** beyond navigation (creating / editing space structure)
- **QR-code login**, **3PID management**, **account deactivation**, **identity server settings**

## Possible / planned polish

- Window position/size/maximized state restore
- Keyboard shortcuts (e.g. quick room switcher, dismiss with Esc)
- Room mentions (`#room`) as pills; self-mention emphasis
- New-device warnings for users in your rooms
- Device rename
- Edit history viewer
- GIF picker
- Additional slash commands
