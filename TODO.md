# TODO

## Step 5 — UI polish (in progress)
- [ ] Message bubbles / cards (visual polish pass on MessageListView rows)
- [ ] Threaded reply panel (slide-in sidebar, Telegram-style — layout TBD)
- [ ] Emoji reactions (reaction bar per row; `send_reaction` / `redact_reaction` FFI; `Vec<(emoji, count, reacted_by_me)>` in TimelineEvent)
- [ ] Room list: unread badge + last-message preview + last-activity sort
- [ ] Room list: DM rooms show the other user's avatar
- [ ] ComposeBar: `/` slash-command hint popup
- [ ] ComposeBar: send typing indicator to the room (`m.typing`)
- [ ] ComposeBar: placeholder label for GTK4 + macOS NativeTextArea (GtkTextView / NSTextView have no built-in placeholder)

## Step 6 — Device verification + key backup
- [ ] FFI: `needs_recovery`, `recover(key_or_passphrase)`, `backup_state`
- [ ] Callback: `on_backup_progress`
- [ ] RecoveryDialog per platform (skip / verify flow after login)

## Step 7 — Spaces navigation
- [ ] FFI: `space_children(space_id) -> Vec<String>`
- [ ] Room list drill-in UI (Back button + space name already exists on macOS; wire Qt6 / GTK4 / Win32)
- [ ] Space creation / management UI

## Step 8 — MSC2545 stickers/emoticons, remaining
- [ ] Inline emoticons in HTML bodies (`<img data-mx-emoticon>`) — Qt6, GTK4, macOS, Win32
- [ ] Win32: StickerPicker `WS_POPUP` surface + right-click "Add to Saved Stickers" (`TrackPopupMenu` on `WM_RBUTTONUP`)
- [ ] GTK4: sticker/emoji picker async worker fetch (stickers unseen in timeline show placeholder)
- [ ] `tk::AsyncImageCache` consolidation (shared host-side cache for picker + message-list images)
- [ ] Fix `save_sticker_to_user_pack` posting empty `info` (thread `info_json` through `StickerHit`)

## Step 8b — Win32 RichEdit inline media overlay
- [ ] `tk::InlineMediaSurface` abstraction + Win32 RichEdit 4.1 (`MSFTEDIT.DLL`) implementation
- [ ] `IRichEditOleCallback` + `OleCreatePictureIndirect` for images / stickers as OLE objects
- [ ] LRU pool of per-row RichEdit HWNDs (cap ~32, hide offscreen rows)

## Step 9 — MSC2545 send
- [ ] FFI: `send_emoticon_message(room_id, plain_body, html_body)`
- [ ] Composer: emit `<img data-mx-emoticon ...>` HTML body when a custom emoticon is picked
- [ ] `:shortcode:` autocomplete popup in compose input

## Step 10 — MSC2545 pack management
- [ ] List enabled packs UI in settings
- [ ] Toggle pack subscription via `im.ponies.emote_rooms` (room settings drill-in)
- [ ] Pack creation / removal flow
- [ ] Sticker delete / rename inside the user pack
- [ ] Manual sticker order / sort

## Step 11 — Notifications (macOS done; note below is for future hardening)
- [ ] macOS: update Step 11 status note in CLAUDE.md (macOS now done via UNUserNotificationCenter)

## Step 12 — Server pushers
- [ ] FFI: `register_pusher` / `remove_pusher`
- [ ] Linux: UnifiedPush via D-Bus `org.unifiedpush.Connector1`
- [ ] Windows: UnifiedPush distributor or WNS (needs Store registration)
- [ ] macOS: APNs pusher

## i18n
- [ ] macOS: wrap UI strings in `NSLocalizedString` + `.strings` extraction
- [ ] Win32: wrap UI strings in `LoadString` + `.rc` resource extraction

## Voice messages
- [ ] Win32: implement `tk::AudioPlayer` backend (currently returns `nullptr`, voice card is inert)
- [ ] All platforms: voice message recording + send (MSC3245)

## Known bugs / gaps
- [ ] Win32 MinGW build unverified (DirectWrite / `GetDpiForWindow` header availability)
- [ ] macOS: `set_password` no-op on `NSTextField` — recovery-key field shows secret in plaintext (fix: swap for `NSSecureTextField`)
- [ ] Win32: `NativeTextArea::natural_height()` undercounts wrapped lines (`EM_GETLINECOUNT × tmHeight` ignores soft-wrap)
- [ ] macOS: initial-sync progress not shown (`on_room_list_state` not wired — add status label or window-title update)
- [ ] `TestSurface` Catch2 backend tests don't cover CoreGraphics (macOS canvas untested)
- [ ] Open decision: timeline persistence (SQLite-backed `Timeline::with_focus` vs memory-only)
- [ ] Open decision: pack-entry encrypted badging (lock glyph on encrypted packs in picker)
