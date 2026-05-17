# ROADMAP.md

Completed work is in [CHANGES.md](CHANGES.md). What follows is only the pending and in-progress work.

## Step 5 — UI redesign (in progress)

Done: inline images, stickers, reply-to, message editing, voice messages, ComposeBar, read receipts (display + sending, overlay only — never expand rows), hover timestamps, day separators, typing indicators, inline bold/italic/code/strikethrough via `formatted_body`, URL previews + hyperlinks (Qt6, GTK4, Win32), Markdown-to-HTML for sent messages, last-message preview in sidebar (regular-weight room name, 1px inter-room separator, compact row sizing). Remaining:

- **Emoji reactions** — reaction bar below each message (emoji + count); tap to toggle; `send_reaction` / `redact_reaction` FFI; reactions in `TimelineEvent` as `Vec<(emoji, count, reacted_by_me)>`.
- **Message bubbles / cards** — visual polish pass on the message layout.
- **Threaded reply panel** — slide-in sidebar (deferred from reply-to landing).
- **Sidebar polish** — DM rooms show the other user's avatar.
- **ComposeBar gaps** — `/` command hints; `NativeTextArea` placeholder on macOS (GTK4 fixed; see Known gaps).

## Step 8 — MSC2545 phase A: remaining items

- **Inline emoticons in HTML message bodies** — render `<img data-mx-emoticon ...>` in `formatted_body` instead of alt text. Per-platform: Qt `QTextDocument::addResource`, GTK `GtkTextChildAnchor`, macOS `NSTextAttachment`, Win32 via RichEdit overlay (Step 8b).
- **Win32 shell wiring** — `StickerPicker` child `WS_POPUP` surface and the underlying RichEdit overlay (Step 8b). Right-click "Add to Saved Stickers" is done (`tk::win32::Surface::set_on_right_click` + `WM_RBUTTONUP`).
- **Async image cache for pickers** — promote `ensure_media_image` into a shared `tk::AsyncImageCache` (worker → decode → post-to-UI → cache + repaint) so `StickerPicker` / `EmojiPicker` custom tabs don't show grey placeholders for stickers not yet seen in a timeline. GTK4 worker-fetch now wired for both pickers; consolidation into a shared `AsyncImageCache` across all platforms still pending.

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

Linux (Qt6 + GTK4) done — see CHANGES.md. Remaining:

- Windows: deferred (WNS needs Store registration; UnifiedPush distributors on Windows are an option).
- macOS: deferred (APNs).

## Known gaps

- **`NativeTextArea` placeholder is a no-op on macOS** — `NSTextView` lacks a built-in placeholder. Fix: paint a `current_text().empty()`-gated label in the shared widget. (GTK4 is fixed: `dim-label` overlay label.)
- **`set_password` is a no-op on macOS** — toggling password mode on `NSTextField` requires swapping for `NSSecureTextField`; recovery-key field still shows plaintext.
- **`TestSurface` doesn't cover CoreGraphics** — QPainter, Cairo, and D2D are tested; macOS CGBitmapContext surface is still TODO.
- **Picker image cache consolidation** — GTK4 now has per-picker async fetch for both `StickerPicker` and `EmojiPicker`; a shared `tk::AsyncImageCache` to unify the four platform paths is still pending.
- **`tk_avatars_` / `tk_images_` not keyed by `(user_id, mxc)`** — cosmetic ghosting risk when two accounts share an mxc URL that resolves to different bytes.
- **URL preview + hyperlink rendering on macOS** — `get_url_preview` FFI and `MessageListView` preview card are wired on Qt6, GTK4, and Win32; macOS `MainWindowController` still needs `on_url_preview_ready_` and `on_link_clicked` wiring.
- **i18n not wired on macOS (`NSLocalizedString`) or Win32 (`LoadString`)**.
- **Notification image is the room avatar only** — all four shells now show the room avatar in notifications (Linux `image-data`/`bytes-icon`; Win32 `appLogoOverride` circle-crop; macOS `UNNotificationAttachment`). An image *message's* own picture is still never shown — `Notification` carries only `avatar_bytes`. Fix: add a message-image field to `Notification` and populate it from `timeline_item_to_ffi`.
- **GTK4 message-list CSS not theme-aware** — every `tk` surface and pop-out window now follows the theme setting, but `apply_theme_ui_()` only rebuilds the `.sidebar` / `.sidebar-separator` CSS rules. `.sender-name` / `.timestamp` / `.room-header` / `.room-header-topic` and the `status_bar_` / `topic_tooltip_label_` `GtkLabel`s keep hardcoded light colours. Fix: rebuild all theme CSS rules from `t.palette` and give the status / tooltip labels palette-driven CSS classes.
- **Native context menus / dialogs unthemed on Win32 + Qt6** — Win32 `TrackPopupMenu` / `MessageBoxW` and Qt6 `QMenu` use the OS / default palette, so right-click menus and message boxes don't follow the in-app dark/light theme. (macOS follows via `NSApp.appearance`; GTK4 via `gtk-application-prefer-dark-theme`.) Fix: owner-draw the Win32 menus and apply the theme palette to the Qt `QMenu`s.

## Decisions still open

- **Timeline persistence** — opt in to sqlite-backed `Timeline::with_focus(...)`, or memory-only?
- **Room-list window** — `AllRooms` for desktop (recommended), or windowed?
- **Pack-entry encrypted badging** — show a lock glyph on encrypted packs in the picker?
- **Thread panel layout** — slide-in panel (Telegram style) vs inline thread expansion (Discord style) vs separate window?
