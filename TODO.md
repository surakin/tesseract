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

## Step 8 — MSC2545 stickers/emoticons, remaining
- [ ] Inline emoticons in HTML bodies (`<img data-mx-emoticon>`) — Qt6, GTK4, macOS, Win32
- [ ] Inline emoticons in HTML bodies (`<img data-mx-emoticon>`) — Qt6, GTK4, macOS, Win32
- [x] Win32: right-click "Add to Saved Stickers" (`TrackPopupMenu` on `WM_RBUTTONUP` via `tk::win32::Surface::set_on_right_click`)
- [x] Fix `save_sticker_to_user_pack` posting empty `info` — `sticker_info_json` now threaded Rust→C++→`StickerHit`; all four callers updated
- [ ] GTK4: sticker/emoji picker async worker fetch (stickers unseen in timeline show placeholder)
- [ ] `tk::AsyncImageCache` consolidation (shared host-side cache for picker + message-list images)

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

## Step 12 — Server pushers
- [ ] FFI: `register_pusher` / `remove_pusher`
- [ ] Linux: UnifiedPush via D-Bus `org.unifiedpush.Connector1`
- [ ] Windows: UnifiedPush distributor or WNS (needs Store registration)
- [ ] macOS: APNs pusher

## i18n
- [ ] macOS: wrap UI strings in `NSLocalizedString` + `.strings` extraction
- [ ] Win32: wrap UI strings in `LoadString` + `.rc` resource extraction

## Voice messages

- [ ] All platforms: voice message recording + send (MSC3245)

## Known bugs / gaps

- [ ] macOS: `set_password` no-op on `NSTextField` — recovery-key field shows secret in plaintext (fix: swap for `NSSecureTextField`)
- [x] Win32: `NativeTextArea::natural_height()` counts wrapped lines (DrawText `DT_CALCRECT | DT_EDITCONTROL` at the `EM_GETRECT` width — no keystroke lag)
- [ ] macOS: initial-sync progress not shown (`on_room_list_state` not wired — add status label or window-title update)
- [ ] `TestSurface` Catch2 backend tests don't cover CoreGraphics (macOS canvas untested)
- [ ] Multi-account: `tk_avatars_` / `tk_images_` not keyed by `(user_id, mxc)` — cosmetic ghosting if two accounts share an mxc URL that resolves differently
- [ ] Multi-account: Qt6 sidebar user strip is still a native QLabel composite — swap for `tk::qt6::Surface + UserInfo` (cosmetic refactor; `UserInfo` is already used inside the account-picker popover)
- [ ] Open decision: timeline persistence (SQLite-backed `Timeline::with_focus` vs memory-only)
- [ ] Open decision: pack-entry encrypted badging (lock glyph on encrypted packs in picker)
