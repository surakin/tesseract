# UI parity spec

Tesseract has four native UIs (Qt6, GTK4, Win32, AppKit). They render the
same Matrix surface but with different toolkits. This file is the
single place where the visual decisions live, so every renderer can be
checked against the same target.

Numeric sizes/paddings are not in this file — they live in
`client/include/tesseract/visual.h` as `constexpr int`s. Every UI
includes that header. **Don't introduce per-platform constants for
anything listed there.**

## Window anatomy

```
┌─────────────────────────────────────────────────────────────────────┐
│ [back] Space name                  Room title                       │
├─────────────────┬───────────────────────────────────────────────────┤
│                 │                                                   │
│  Room list      │   Message list                                    │
│  (260 px wide)  │   (expands)                                       │
│                 │                                                   │
│                 │                                                   │
│                 │                                                   │
├─────────────────┤                                                   │
│  User strip     │                                                   │
│  (48 px tall)   │                                                   │
├─────────────────┴───────────────────────────────────────────────────┤
│                          Compose bar (40–120 px, expanding)         │
└─────────────────────────────────────────────────────────────────────┘
```

Sidebar width: `kSidebarWidth` (260 px), fixed.
Compose bar grows with content between `kComposeMinHeight` and
`kComposeMaxHeight`.

## Message row anatomy

```
┌──────────────────────────────────────────────────────────────────┐
│ ⬤   Alice                                                        │
│ 32  hello! how are you doing today?                              │
│     ↳ another line if the body wraps                             │
│     [👍 2]  [🎉 1]                                14:23           │
└──────────────────────────────────────────────────────────────────┘
```

- **Avatar** — `kMsgAvatarSize` (32 px) circle on the left, **always**,
  for every sender including the current user. Initials fallback when
  the sender has no avatar URL. `kMsgAvatarGap` (8 px) between avatar
  and the body column.
- **Sender name** — `kMsgSenderNameHeight` (16 px) row above the body,
  font size `kFontSenderName` (12 pt). Same-sender consecutive messages
  may collapse the name (per-platform decision; flagged here for future
  consistency work).
- **Body** — flat text. **No bubble background, no rounded corners, no
  own-vs-other colour split.** Both light and dark schemes use the
  ambient text colour token.
- **Inline media** — `m.image` and `m.sticker` thumbnails capped at
  `kMaxInlineImageWidth` × `kMaxInlineImageHeight` (320 × 200) for
  images, `kStickerSize` (256 px square) for stickers.
- **Reaction chips** — `kReactionChipHeight` (22 px) tall pill chips
  with `kReactionChipGap` (4 px) between, in a row beneath the body.
- **Timestamp** — `HH:MM`, `kFontTimestamp` (10 pt), secondary text
  colour, right-aligned in a `kMsgTimestampHeight` (14 px) footer
  beneath the row.

## Room row anatomy

```
┌──────────────────────────────────────────────────────────────────┐
│ ⬤   Room name                                              ( 3 ) │
│ 36   Last message preview, ellipsised on overflow…               │
└──────────────────────────────────────────────────────────────────┘
```

- **Avatar** — `kRoomAvatarSize` (36 px) circle, initials fallback when
  the room has no avatar URL.
- **Name** — `kFontSidebarName` (13 pt), bold, single line, ellipsised.
- **Last-message preview** — `kFontSidebarPreview` (11 pt), secondary
  text colour, single line, ellipsised.
- **Unread badge** — pill on the right showing the unread notification
  count, accent background, white text, `kFontUnreadBadge` (11 pt),
  `kUnreadBadgeMinWidth` × `kUnreadBadgeHeight` (20 × 18 px). Hidden
  when count is zero.
- Row height: `kRoomRowHeight` (62 px).

## Decision list

Every UI must obey these. When you change something here, change every
UI that doesn't already match.

1. **No message bubbles.** Body text renders flat over the chat
   background. There is no rounded fill, no own-vs-other colour split,
   no right-alignment branch for own messages.
2. **Avatar on the left for every message**, including own messages.
   Render the initials disc when the sender has no avatar URL.
3. **Timestamp visible on every message** in a small right-aligned
   footer (HH:MM, secondary colour, 10 pt).
4. **Sidebar width is 260 px**, fixed across all platforms.
5. **Spaces are not prefixed with `#`** in the sidebar; they are
   distinguished by drill-in behaviour, not a glyph.
6. **System dark mode**: macOS picks up dark mode automatically by
   using `NSColor` semantic colours (e.g. `labelColor`,
   `secondaryLabelColor`, `windowBackgroundColor`) wherever the table
   below lists a colour token. Qt, GTK, and Win32 use the listed hex
   literals as-is; dark-mode adaptation is a future-work item tracked
   separately.

## Colour token table

| Token             | Hex      | Use                                  | macOS substitute                |
|-------------------|----------|--------------------------------------|---------------------------------|
| `kColorBg`        | `#FFFFFF`| Chat area background                 | `[NSColor controlBackgroundColor]` |
| `kColorSidebarBg` | `#F0F2F5`| Sidebar / room list background       | `[NSColor windowBackgroundColor]`  |
| `kColorBorder`    | `#D0D3D8`| 1 px separators                      | `[NSColor separatorColor]`         |
| `kColorTextPrimary`   | `#111111`| Body text, room name             | `[NSColor labelColor]`             |
| `kColorTextSecondary` | `#8E8E93`| Sender name, timestamps, preview | `[NSColor secondaryLabelColor]`    |
| `kColorAccent`    | `#0084FF`| Buttons, focus ring, links           | `[NSColor controlAccentColor]`     |
| `kColorUnreadBadge` | `#0084FF`| Unread pill background             | `[NSColor controlAccentColor]`     |
