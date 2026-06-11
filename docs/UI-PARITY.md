# UI parity spec

Tesseract has four native UIs (Qt6, GTK4, Win32, AppKit). They render the
same Matrix surface but with different toolkits. This file is the
single place where the visual decisions live, so every renderer can be
checked against the same target.

Numeric sizes/paddings are not in this file. Fixed layout constants
(sizes, gaps, image caps) live in `client/include/tesseract/visual.h`
as `constexpr int`s. Font sizes and reaction-chip dimensions are
runtime-tunable and live in `tesseract::Settings`
(`client/include/tesseract/settings.h`) — referenced below as
`Settings::<field>`. Colours live in the shared `tk::Palette`
(`ui/shared/tk/theme.cpp`), mirrored in the table at the bottom of this
file. **Don't introduce per-platform constants for anything in those
three places.**

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
  font size `Settings::font_sender_name` (11 pt). Consecutive messages
  from the same sender within `Settings::message_group_interval_s`
  (300 s) collapse into continuation rows that suppress the repeated
  avatar and sender name. This grouping lives in shared code
  (`MessageListView`), so it is identical across all four platforms; set
  the interval to 0 to disable it.
- **Body** — flat text. **No bubble background, no rounded corners, no
  own-vs-other colour split.** Both light and dark schemes use the
  ambient text colour token.
- **Inline media** — `m.image` and `m.sticker` thumbnails capped at
  `kMaxInlineImageWidth` × `kMaxInlineImageHeight` (320 × 200) for
  images, `kStickerSize` (256 px square) for stickers.
- **Reaction chips** — `Settings::reaction_chip_height` (28 px) tall
  pill chips with `Settings::reaction_chip_gap` (6 px) between, in a row
  beneath the body.
- **Timestamp** — `HH:MM`, `Settings::font_timestamp` (9 pt), muted text
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
- **Name** — `Settings::font_sidebar_name` (12 pt), bold, single line,
  ellipsised.
- **Last-message preview** — `Settings::font_sidebar_preview` (10 pt),
  secondary text colour, single line, ellipsised.
- **Unread badge** — pill on the right showing the unread notification
  count, accent background, white text, `Settings::font_unread_badge`
  (10 pt), `kUnreadBadgeMinWidth` × `kUnreadBadgeHeight` (20 × 18 px).
  Hidden when count is zero.
- Row height: `kRoomRowHeight` (48 px).

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
6. **Light + dark are both shipped, on every platform.** All four UIs
   render from the same shared `tk::Palette` (`tk::Theme::light()` /
   `tk::Theme::dark()`) — including macOS, which draws via CoreGraphics
   from the palette hex values rather than `NSColor` semantic colours
   (only a couple of native AppKit status labels still use `NSColor`).
   The active theme is resolved in `ShellBase::apply_current_theme_()`
   from `Settings::ThemePreference`: `Light` / `Dark` force a palette,
   `System` (default) follows the OS. Each shell detects the OS scheme
   natively — `QStyleHints::colorScheme()` (Qt), the
   `gtk-application-prefer-dark-theme` setting (GTK),
   `win32::theme::current_mode()` (Win32), and
   `NSApp.effectiveAppearance` (macOS) — and feeds it into the same
   shared selection.

## Colour token table

Tokens are `tk::Palette` fields (`ui/shared/tk/theme.cpp`). Every
platform — macOS included — resolves colours from these values; there is
no per-platform substitution. The table lists the load-bearing tokens;
the full palette (reaction chips, presence dots, destructive states,
selection/code tints, hover/pressed steps) lives in `theme.cpp`.

| `Palette` field        | Light     | Dark      | Use                            |
|------------------------|-----------|-----------|--------------------------------|
| `bg`                   | `#FFFFFF` | `#1B1D21` | Chat area background           |
| `sidebar_bg`           | `#F0F2F5` | `#16181C` | Sidebar / room-list background |
| `chrome_bg`            | `#F8F9FA` | `#202327` | Headers, status, banners       |
| `border` / `separator` | `#D0D3D8` | `#33363B` | 1 px separators                |
| `text_primary`         | `#111111` | `#F0F0F2` | Body text, room name           |
| `text_secondary`       | `#8E8E93` | `#A0A0A8` | Sender name, preview           |
| `text_muted`           | `#A0A0A6` | `#808088` | Timestamps, hint text          |
| `accent`               | `#0084FF` | `#4DA3FF` | Buttons, focus ring, links     |
| `unread_bg`            | `#0084FF` | `#4DA3FF` | Unread pill background         |
