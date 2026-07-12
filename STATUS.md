# Tesseract — Implemented Features

Snapshot of every feature that has landed on `main`. Last updated **2026-07-12** (v0.8.14-unreleased). 1145 C++ + 369 Rust tests.

> **Emoji/sticker pickers and the shortcode popup now surface packs from
> any Space the current room belongs to (2026-07-12, v0.8.14-unreleased).**
> Extends the personal/current-room/subscribed-room pack scopes (see the
> "load MSC2545 image packs" entries below) with a fourth: every Space
> containing the current room, direct or nested-ancestor (a Space inside
> another Space). No Rust/FFI changes were needed — two existing pieces of
> plumbing compose directly. `ShellBase::parent_spaces_for_room_()` is a
> new BFS that inverts the already-cached `space_children_cache_`
> (space_id → joined children, kept warm for the Spaces sidebar) to answer
> "which spaces contain this room," with a visited-set guarding against a
> cyclical (misconfigured) hierarchy. `Client::set_active_room()` — already
> called for the current room on every switch/pop-out-open purely to keep
> that room's own pack fetched — is now also called for each ancestor
> space, reusing its existing LRU/fetch/notify mechanism with no new
> side effects. `views::is_pack_picker_visible`/`order_picker_packs`
> (`image_pack_order.h/.cpp`) gained a `parent_space_ids` parameter and a
> new bucket ordered personal → current room → parent spaces → subscribed;
> threaded through `EmojiPicker`/`StickerPicker`'s new
> `set_current_room_parent_spaces()` setter and `ShellBase::
> emoticons_for_room_()`, and wired into all four shells' main-window
> picker refresh, pop-out picker construction, and space-cache-ready
> callback. New unit tests cover space-sourced visibility, unrelated-room
> exclusion, and the 4-bucket ordering. Verified on Qt6/GTK4 (both build
> clean, 1145/1145 ctest); Win32/macOS mirror the same pattern but weren't
> build-verified in this environment.

<!-- -->

> **Native-field theming now traverses the widget tree instead of a
> hand-maintained per-shell field list (2026-07-12, v0.8.14-unreleased).**
> Follow-up to the per-field `set_text_color` fix below, which patched the
> symptom without changing the mechanism — every future native field would
> still need someone to remember to add it to the same manually-enumerated
> list. `tk::Widget` gained a virtual `on_theme_changed()` hook and a
> non-virtual `apply_theme()` that recurses into every child (including
> hidden ones, since a hidden field still needs correct colors queued for
> when it next shows — unlike `paint`/`paint_overlay`, which skip them).
> The 13 shared views that own a shell-supplied native field (`ComposeBar`,
> `RoomInfoPanel`, `RoomSearchBar`, `RoomListView`, `QuickSwitcher`,
> `MessageSearchView`, `ForwardRoomPicker`, `EncryptionSetupOverlay`,
> `QRGrantView`, `RoomSettingsView`, `ImagePackEditorView`, `SettingsView`,
> `JoinRoomView`) now push their own field's color from `on_theme_changed()`;
> every shell's `apply_theme_ui_()`/pop-out `apply_theme()` collapses from a
> per-field if-chain to one `surface->root()->apply_theme(t)` call per
> surface. Native fields moved from shell-owned `unique_ptr` to `shared_ptr`
> (`Host::make_text_field()`'s own signature is unchanged — `shared_ptr` has
> a converting assignment from `unique_ptr&&`) so each owning view can hold
> a `weak_ptr` instead of a pointer that could dangle. Auditing every call
> site surfaced further gaps the old per-field list missed entirely: Qt6's
> `SettingsWidget`/`JoinRoomDialog`/pop-out `RoomWindow` never pushed
> `set_text_color` at all, and macOS's join-room dialog surface was
> permanently stuck on `tk::Theme::light()`, never re-themed on a theme
> change. New unit test (`Widget::apply_theme visits every descendant,
> including hidden ones`). Verified on Qt6/GTK4 (both build clean, 1141/1141
> ctest, 376/376 Rust); Win32/macOS mirror the same pattern but weren't
> build-verified in this environment.

<!-- -->

> **Generic `tk::Host` tooltip system replaces 8 hand-rolled hover/tooltip
> implementations and 4 duplicate per-platform native tooltip codepaths
> (2026-07-12, v0.8.14-unreleased).** Every tooltip in the app used to be
> independently hand-rolled: a bool/enum hover-state field plus manual rect
> hit-testing in `on_pointer_move` (often requiring a `dispatch_pointer_move`
> override just to catch a rect that wasn't a real child widget), bubbling a
> `text, anchor` pair up through `RoomView`/`SettingsView` to the platform
> shell. Each of the four shells then rendered it differently — and two of
> them weren't even using a genuine native tooltip API: macOS faked it with
> a custom `NSPopover` and GTK4 with a custom `GtkPopover` styled with a
> `"tooltip"` CSS class; only Windows (`TOOLTIPS_CLASS`) and Qt6
> (`QToolTip`) used the real thing. None of the 8 sites had a show-delay —
> every tooltip appeared the instant the pointer crossed the hit-test rect.
> Replaced with `tk::Host::show_tooltip()`/`hide_tooltip()`/
> `update_tooltip_text()`, a dedicated tooltip slot on `Host` (separate from
> `register_popup()`'s popup slot, since tooltips don't need input capture
> and must be suppressed while a real popup is open) with a 500ms dwell
> delay via the existing `post_delayed` + generation-counter idiom, and a new
> `tk::Tooltip` render-only class drawn in the host's `paint_overlay` pass so
> it escapes every ancestor's clip (needed for e.g. a tooltip anchored on a
> message action-pill inside the scrolled `MessageListView`). Migrated all 8
> call sites (`RoomHeader`, `RoomInfoPanel`, `ComposeBar`, `MessageListView`
> action pills, `LocationMapPanner`, `AboutSection` cache rows —
> including its reference-counted adjacent-cell hover and async
> stats-arrival update-in-place case — `AdvancedSection`, and
> `TabbedGridPicker`'s per-cell shortcode tooltip) and deleted the old
> `on_show_tooltip`/`on_hide_tooltip` callback plumbing plus all 4 platform
> shells' native tooltip code. Fixed a pre-existing i18n gap found during
> the `ComposeBar` migration (4 tooltip strings not wrapped in `tk::tr()`,
> no `.po` entries). New `tests/cpp/test_tk_tooltip.cpp` (8 cases covering
> the dwell delay, generation-counter staleness guard, popup suppression,
> and pointer-down dismissal) plus a shared `tests/cpp/tk_test_host.h`
> fake `Host` factored out of `test_tk_host_pointer.cpp` for reuse. Verified
> on Qt6/GTK4 (both build clean, 1129/1129 ctest, 376/376 Rust); Win32/macOS
> mirror the same pattern but weren't build-verified in this environment.

<!-- -->

> **Native text fields no longer go stale on a theme change; forward-picker
> close wired on all four shells (2026-07-12, v0.8.14-unreleased).** Qt6's
> `QLineEdit`/`QTextEdit` carry an explicit `QPalette` that must be manually
> kept in sync with the app's `Theme`, unlike GTK4 (CSS-driven), macOS
> (`NSColor.labelColor`, a dynamic system color), or Win32 (a global
> `WM_CTLCOLOREDIT` handler) — none of which need an explicit sync.
> `apply_theme_ui_()` only refreshed 2 of the Qt shell's 13 native text
> fields on a theme change (`roomTextArea_`, `roomSearchField_`); everything
> else — including the Ctrl+K quick switcher's search field — kept whatever
> color it was constructed with, so switching to dark mode left it (and
> several others) stuck with unreadable black-on-dark text. Now refreshes
> all 13 (topic/room-settings/image-pack fields, quick switcher, message
> search, forward picker, find-in-room, and the encryption/QR-grant fields,
> three of which had no color sync at all). Auditing every native field also
> surfaced a second, cross-platform bug: `ForwardRoomPicker::on_close` was
> never wired on any of the four shells — `quick_switcher`/`message_search`
> both wire it; `forward_picker` (a dangling `if` with no body on Qt6)
> didn't — so Escape/outside-click on the forward-message picker never reset
> its native field. Fixed on all four shells. Verified on Qt6/GTK4 (both
> build clean, 1120/1120 ctest); Win32/macOS mirror the same pattern but
> weren't build-verified in this environment.

<!-- -->

> **Media lightbox pagination leak + gallery backpressure fixed
> (2026-07-12, v0.8.14-unreleased).** Follow-up to the 2026-07-10 gallery
> pagination fix below, covering bugs found while investigating why opening
> the room media viewer left background work running, delayed local echo,
> or could hang app shutdown. `VideoViewerOverlay` never swallowed wheel
> input while open (unlike `ImageViewerOverlay`), so scrolling over the
> fullscreen video lightbox fell through to the room timeline and silently
> drove backward pagination — contending with the send queue's durability
> write on matrix-sdk-sqlite's single write connection. The Rust
> `paginate_tasks` registry was the one task registry never force-aborted in
> `stop_sync()` (unlike `sync_tasks`/`media_tasks`/etc.), so an in-flight
> pagination task could block shutdown. Neither the media lightbox nor
> active voice/audio playback was torn down on room switch, leaving them
> running against a room the user had already left. `RoomMediaView` never
> opted into `autofill_only_when_empty`, so `ListView`'s "fill on open"
> autofill re-fired `on_near_top` on every relayout in media-sparse rooms,
> spamming pagination; fixed via `set_autofill_only_when_empty(true)` plus a
> proper viewport-fill loop (`RoomMediaView::estimated_capacity()` replacing
> a fixed magic-number retry target) and render-gap backpressure in
> `ShellBase` that defers further pagination rounds when the render queue
> falls more than 24 items behind, resuming once the renderer catches up —
> closing a race where pagination rounds outpaced the diff-streaming task
> that actually populates rows. Added a dedicated authoritative-count FFI
> (`paginate_media_view_back_async` / `on_media_view_paginate_result`) so
> the retry loop judges progress from the SDK's own Image/Video count
> instead of the slower, racier rendered-row count. 1120 C++ + 364 Rust
> tests.

<!-- -->

> **Image pack editor: multi-pack room/space editor + global settings tab,
> fully wired end to end (2026-07-11, v0.8.14-unreleased).**
> `ImagePackEditorView` edits every MSC2545 pack in a room (or space) at
> once, each as its own scrollable section (`ImagePackSectionList :
> ScrollableBase`) with its own name header (click to rename inline, same
> pattern as per-tile shortcode editing), a 3-way usage toggle
> (sticker/emoji/both), a header remove chip that deletes the whole pack,
> and an image-tile grid (hover remove chip, click-to-edit shortcode,
> position-aware drag-drop and paste). A fixed create-row above the list
> adds new empty packs; clicking a header selects that pack as "active" —
> the paste target and the drop fallback for a drop that lands outside
> every pack's grid. Position-based drop targeting threads a `tk::Point`
> through `tk::FileDropHandler` on all four platforms (Qt6
> `QDropEvent::position()`, GTK4 `GtkDropTarget` callback `x,y`, Win32
> `IDropTarget::Drop`'s screen-space `POINTL` via `ScreenToClient` +
> `phys_to_dip`, macOS `NSDraggingInfo.draggingLocation` via
> `convertPoint:fromView:nil`), with native-overlay wiring (pack-name
> field, shortcode field, paste-catcher) added to Win32/macOS, which never
> had it in the single-pack-era editor.
>
> Everything stays staged in memory until Accept, which now actually
> persists: `Client::save_room_pack` wholesale-replaces a pack's images
> (matching the editor's full-snapshot staging model), assigns a fresh
> collision-free `state_key` for new packs, and writes back to whichever of
> the stable (`m.room.image_pack`) / unstable (`im.ponies.room_emotes`)
> event types an existing pack already uses; `Client::remove_room_pack`
> empties a pack (Matrix has no true state-event delete) and discovery
> skips zero-image packs so a removed pack disappears everywhere instead of
> resurfacing empty. Editing is gated behind the room's actual power
> levels via `Client::can_set_room_image_packs` (mirrors the
> `can_set_room_name` family) — Create, remove, the usage toggle,
> rename/shortcode editing, and paste/drop are all disabled for
> insufficiently-privileged users; header-click-to-select-active stays
> enabled since it doesn't mutate anything. Mounted as a 5th "Emojis &
> Stickers" tab in `RoomSettingsView`, committing through the shared
> Accept/Cancel footer like every other tab (`RoomSettingsChanges::
> image_packs`, populated only when `ImagePackEditorView::has_changes()`),
> and extended to space roots the same way Media/encryption are hidden
> there instead — image packs are ordinary room state, so a space hosts
> its own pack the same as any room.
>
> A separate global "Emojis & Stickers" tab in the main `SettingsView`
> covers the account-wide surface: `UserPackEditor` (single-pack tile-grid
> editor for the personal `im.ponies.user_emotes` pack — add/remove/rename
> images, staged until Save, uploading brand-new pasted/dropped images via
> `upload_media`) and `KnownPacksList` (checkbox list of every known room
> pack, toggling `m.image_pack.rooms`/`im.ponies.emote_rooms` subscription
> immediately). `ImagePackTileGridBase` is a shared base for the tile-grid
> layout/paint/hit-test logic both editors need.
>
> Pack **discovery** fetches full per-room state (`RoomStateCache`,
> triggered on room switch rather than waiting for a sync tick, cached in
> `room_image_pack_cache`) since sliding sync never delivers custom
> `m.room.image_pack` state and packs can use non-empty `state_key`s that
> MSC2545 doesn't require to be empty — the earlier single-guessed-key
> lookup missed rooms with multiple named packs. What's **shown** in the
> emoji/sticker pickers and the inline `:shortcode:` popup is scoped to the
> personal pack, the currently-open room, and explicitly subscribed rooms
> (each pop-out window computes its own filtered list from its own room).
> Reads for the personal/enabled-rooms/per-room state events fetch both the
> stable and unstable MSC2545 event names and combine them
> (`merge_pack_contents`) rather than stopping at the first hit, so a
> partially-migrated account or room doesn't silently lose images.
>
> Qt6 is build-verified end to end (1096 C++ tests passing); GTK4 is
> user-build-verified per usual practice; Win32/macOS are verified by
> static reading/pattern-matching only (no toolchain in this environment).
> Also fixed along the way: animated stickers/emoji rendering blank in
> pack editor tiles (the static-image provider never checked
> `anim_cache_`, shared with the Windows composer's inline custom-emoji
> preview); animated stickers in the global tab only advancing on mouse
> move (its view lives on its own top-level surface no shell's per-tick
> repaint hook knew about); dropped images with no shortcode colliding on
> a multi-image drop; a stale shortcode field not reseeding on a second
> drop; clicking outside a shortcode field committing stale text instead
> of cancelling; the shortcode popup never checking `anim_cache_` for
> animated custom emoji; a latent `i18n-pseudo` bug in `gen_pseudo.py`
> (rejected by `msgfmt` on leading/trailing-newline mismatches); a Qt6
> text-rendering bug where an unset `max_width` sentinel mispositioned
> centered labels; and image-pack native fields resolving only against the
> room's `RoomSettingsView` instance, silently no-oping when a space's
> settings tab was open instead.

<!-- -->

> **Edited plain-text messages no longer render as a bare `*`
> (2026-07-11, v0.8.14-unreleased).** `ruma-events`' edit-fallback
> construction (`make_replacement_body()`) unconditionally stamps a
> synthetic empty-HTML `"* "` fallback onto an edit event's top-level
> `format`/`formatted_body`, even for edits with no real formatting — the
> real text stays correct and unprefixed in `m.new_content`.
> `resanitized_formatted_body` (`sdk/src/client/timeline_convert.rs`)
> re-read that raw top-level fallback from the edit event's own JSON
> instead of the resolved content, overriding the correct value with the
> bogus `"*"`. Now reads from `m.new_content` for `m.replace` events,
> matching Element/Cinny's handling of the same spec fallback.

<!-- -->

> **Windows: clipboard image paste restored in the BetterText composer
> (2026-07-11, v0.8.14-unreleased).** `BetterTextArea` handles Ctrl+V
> internally as a text-only paste and never let `WM_PASTE` reach the host
> subclass proc, so the existing `WM_PASTE`-based image-paste check was
> dead code for keyboard paste once the composer moved to BetterText.
> Intercepts at `WM_KEYDOWN` instead, mirroring the fix already applied to
> the old windowless RichEdit backend.

<!-- -->

> **Fixed a runaway pagination loop in the room media gallery
> (2026-07-10, v0.8.14-unreleased).** Closing the "Media (N)" gallery
> (`RoomMediaView`) didn't actually stop its backward-pagination retries: the
> grid's `tk::ListView` runs an arrange-time "autofill" that fires
> `on_near_top` whenever the loaded content doesn't fill the viewport, with no
> visibility gate, and `MainAppWidget::arrange()` kept re-arranging the
> gallery on every unrelated app-wide relayout even while hidden. Combined
> with `RoomMediaView::room_id_` never being cleared on close, and
> `ShellBase`'s `on_load_older_media` handler unconditionally re-arming
> `media_view_retries_left_` on every call, this produced a real, unbounded
> `paginate_back_async` loop for whatever room the gallery was last open on —
> visible as a `timeline/paginate` entry that never cleared from the inflight
> dot and cycled indefinitely. Since every pagination round's write shares
> matrix-sdk-sqlite's single write-connection lock with the send queue's
> durability write (the one that gates a local echo's ◷ appearing), this also
> explained a residual local-echo delay and, in the worst case, an app
> shutdown that wouldn't finish until an in-flight round happened to complete
> naturally. Fixed at the root: `on_load_older_media` now no-ops unless
> `room_id == media_view_room_id_`, plus defense-in-depth (`RoomMediaView`
> clears `room_id_` on close; `ListView`'s autofill and `MainAppWidget`'s
> gallery re-arrange both gate on `visible()`). Verified on Qt6; shared code
> so GTK4/Win32/macOS get the fix too.

<!-- -->

> **Pop-out window feature-parity audit (2026-07-10, v0.8.14-unreleased).**
> An audit of every `RoomView` callback found 13 places where a feature was
> wired for the main window's embedded room but never wired for pop-out room
> windows (or, in one case, the reverse): file-attachment save dialogs,
> jump-to-original-message, pin/unpin plus the pinned-messages banner,
> Up-arrow-to-edit-last-message, retry/abort of a failed send, inline
> autoplay video/GIF, the forward-message picker, the room media gallery,
> visible-row media/avatar lazy-fetch prioritization, open-DM-in-a-new-window,
> confirm dialogs, and macOS composer shortcode/slash/GIF popups. Closing
> these was largely mechanical once `PopoutRoomWidget` was refactored onto
> `tk::Stack` (its hand-rolled measure/arrange/paint were redundant with
> `Stack`'s defaults), since most gaps turned out to be single shared-code
> edits in `RoomWindowBase`/`ShellBase` that fix all four platforms at once.
> Also: a `ConfirmDialog` open in a pop-out now hides the native compose/
> search overlays underneath it instead of letting them paint through the
> modal backdrop, and incoming-call banners/`on_start_call` now resolve to
> whichever window — main or pop-out — currently has the room open, via
> `ShellBase::room_view_for_room_()`. Qt6/GTK4/macOS mirror the Windows
> pattern closely but were only compile-verified on Windows.

<!-- -->

> **MSC2545 image packs now combine stable + unstable event names
> (2026-07-10, v0.8.14-unreleased).** MSC2545 gives the per-room pack
> (`m.room.image_pack`) and the enabled-rooms pointer (`m.image_pack.rooms`)
> each a legacy `im.ponies.*` unstable equivalent. Pack loading previously
> probed the unstable name first and stopped at the first hit, so a room or
> account that had only written the stable name — or partially migrated,
> with images under one name the other lacked — silently lost those images.
> Reads now fetch both names and combine them (`merge_pack_contents`: union
> `images` by shortcode, unstable wins a collision; pack metadata filled from
> whichever side has it) at all four read sites — the rooms-pointer account
> data, the local state-store fast path, the HTTP fallback, and the implicit
> all-joined-rooms enumeration. Read-only change; no FFI/C++ changes and no
> change to how Tesseract writes its own account data.

<!-- -->

> **Linux OS dark/light mode detection fixed on Qt6 and GTK4 (2026-07-10,
> v0.8.14-unreleased).** Qt6's fallback theme-detection path (used whenever
> Qt's own `colorScheme()` is unavailable, e.g. Qt < 6.5) called the
> deprecated `Settings.Read` D-Bus method, which double-wraps its return
> value in an extra variant layer and made `QVariant::toInt()` silently read
> 0 instead of the real color-scheme value — Dark could never be detected
> through that path. Switched to `ReadOne`. GTK4 previously relied entirely
> on distro-specific bridges (`kde-gtk-config`) and a `GtkSettings` signal
> that never fires live on Wayland; it now queries the same XDG
> desktop-settings portal Qt6 uses, keeping the old `GtkSettings` read only
> as a last-resort fallback.

<!-- -->

> **Windows composer mention pills render as real inline chips
> (2026-07-10, v0.8.14-unreleased).** `BetterTextArea::insert_mention`
> previously inserted plain `"@Name "` text with a no-op
> `set_mention_colors()`, so Windows was the one platform where mentions
> didn't get a colored chip. Now renders the pill offscreen via a WIC-backed
> D2D render target and inserts it as a real BetterText image run — the same
> mechanism already used for custom-emoji pills.

<!-- -->

> **Pinned-events room-list fingerprint fix (2026-07-10, v0.8.14-unreleased).**
> Pin/unpin sends `m.room.pinned_events` correctly, but the sync loop only
> forwards a fresh room snapshot to C++ when `room_list_fingerprint()`
> changes, and `pinned_events` wasn't a tracked field — a pin/unpin that
> didn't also touch unread/name/avatar/recency left the pinned-messages
> banner and the Pin/Unpin action label stale until an unrelated change (or
> app restart) happened to flush it. Fixed by adding a joined-event-id field
> to the fingerprint key.

<!-- -->

> **Faster local echo under background load (2026-07-09, v0.8.14-unreleased).**
> A just-sent message's local echo could take seconds to appear in the timeline
> when the app was busy, even though the same message showed up promptly in the
> room list as the last-message preview. The root cause was **tokio async-worker
> starvation in the SDK**, not the C++ UI queue: the room-list update rides a
> lightweight, debounced `RoomInfoNotableUpdate` broadcast channel, but the
> timeline echo is produced by matrix-sdk / matrix-sdk-ui background tasks that
> spawn onto our shared runtime's async workers. Room switching ran the
> CPU-bound `Timeline::init_focus` build (it collects cached events into an
> `imbl::Vector`) *on* an async worker via `block_on(spawn(async {…build…}))`,
> occupying that worker for the whole build and starving the send-queue /
> diff-generation tasks that emit the echo. Fixed by running both `init_focus`
> builds (`subscribe_room`, `subscribe_room_at`) on tokio's **blocking pool**
> via `spawn_blocking` + a runtime `Handle`, leaving every async worker free for
> the latency-sensitive diff tasks; blocking-pool threads inherit the widened
> 8 MB stack the macOS deep-recursion guard needs. No FFI changes.
>
> An earlier attempt (`95b8ebd5`) misdiagnosed this as the echo waiting behind
> heavy media/relayout tasks on the shared C++ `post_to_ui_` queue — but that
> queue also carries the prompt room-list update, so it was never the
> bottleneck. Those changes still landed as general perf wins and remain in the
> tree: Qt/Win32/macOS avatar + Qt message-tile decode moved off the UI thread
> (GTK already did); media/room relayout finalizers coalesced onto the shared
> `schedule_relayout_`; map-tile arrivals trigger a plain repaint instead of a
> full O(timeline) `invalidate_data()` re-measure; and plain-text send routed
> through the mutation worker pool like every other composer mutation
> (reply/edit/reaction), with failures surfacing via the per-message ◷→⚠/retry
> indicator. Verified on **Qt6** and **GTK4**; Win32/macOS mirror the same
> pattern (pending on-platform build).

<!-- -->

> **Custom MSC2545 emoji inline, end to end (2026-07-06, v0.8.14-unreleased).**
> Picking a custom emoji from the picker or shortcode autocomplete inserts a
> real inline image pill in the composer (mirroring the @mention pill mechanism
> per platform — `QTextDocument::addResource`, `GtkTextChildAnchor`, an
> `NSTextAttachment` subclass), sends proper `<img data-mx-emoticon>` HTML, and
> renders that tag as an inline image in the read-only timeline. Inline images
> reserve layout via U+FFFC (OBJECT REPLACEMENT CHARACTER) bound to each
> backend's native inline-object mechanism (`IDWriteInlineObject` on Windows,
> `PangoAttrShape` on GTK4, `QTextObjectInterface` on Qt6; macOS keeps the
> placeholder-glyph approach), with `MessageListView::paint_span_images`
> painting the resolved bitmap once decoded. The SDK re-sanitizes the raw event
> JSON with `data-mx-emoticon` allow-listed (`sdk/src/html_sanitize.rs`), since
> matrix-sdk-ui's Timeline sanitizer strips it, and `image_packs.rs` now uses
> ruma's typed MSC2545 `PackInfo`/`MxcUri` (behind `unstable-msc2545`). Fixes
> an XSS: the attacker-controlled shortcode was interpolated into `alt`/`title`
> unescaped. As part of this work, the per-platform `tk::` backend
> implementations (canvas/host/audio/video/screen-capture) moved out of the
> cross-platform `tesseract_tk` library into each platform's own target
> (genuinely shared GStreamer code stays in `ui/shared/tk/`).

<!-- -->

> **BetterText: D2D/DirectWrite text backend on Windows (2026-07-09, v0.8.14-unreleased).**
> Vendored BetterText (`third_party/bettertext`), a from-scratch
> D2D/DirectWrite Win32 text control, as a new backend for
> `NativeTextField`/`NativeTextArea` (`BetterTextField`/`BetterTextArea` in
> `host_win32.cpp`). Adds change/submit notifications, content-height query,
> single-line mode, placeholder + password rendering, inline IME composition
> with candidate-window positioning, an opt-in scrollbar, per-axis padding, and
> real inline bitmap rendering — so custom emoji render inline in Windows
> compose fields (via `ShellBase::ensure_media_image_`) instead of the old
> plain-text fallback, with emoji glyphs routed through the bundled Noto Color
> Emoji font (an `IBetterTextFontProvider` adapter over the existing
> `build_emoji_fallback()` collection) to match the message list. Wired
> alongside the existing EDIT/RichEdit-backed classes, which are not yet removed
> pending full manual verification.

<!-- -->

> **Copy image to clipboard from the lightbox (2026-07-07, v0.8.14-unreleased).**
> The full-window image viewer gains a third top-right chrome button (a
> lucide "copy" icon, left of save) that copies the currently displayed
> image to the system clipboard. Added `tk::Host::set_clipboard_image`
> (sibling of `set_clipboard_text`) with native backends for Qt6
> (`QClipboard::setImage`), GTK4 (`gdk_texture_new_from_bytes` +
> `gdk_clipboard_set_texture`), Win32 (WIC decode → `CF_DIBV5`), and macOS
> (`NSPasteboard writeObjects`). The copy button is opt-in in
> `MediaOverlayBase` (`wants_copy_button_()`) so the video overlay is
> unaffected. Because copying needs no native file dialog (unlike save), the
> action is wired in shared code — `ShellBase::wire_main_app_viewers_` for
> the main window and `RoomWindowBase` for pop-outs — rather than
> per-shell. On success a self-dismissing "Copied to clipboard" `Toast`
> pill (owned by the overlay, auto-hidden via injected `post_delayed` with a
> liveness guard) confirms the copy over the lightbox — a native status-bar
> message would be hidden behind the full-window scrim. The Qt backend sets
> the clipboard via `QMimeData` carrying `x-kde-force-image-copy` so copied
> images land in KDE Klipper's history even with its "Ignore images" setting
> on (the same flag Spectacle uses), plus `application/x-kde-suggestedfilename`
> for nicer paste-target naming. 4 new Catch2 tests. Verified on **Qt6**.

<!-- -->

> **Room Permissions self-lockout warning (2026-07-06, v0.8.13).**
> The Permissions tab now warns and disables Accept if a staged change
> would lock the current user out of ever editing room permissions again
> (raising "Change permissions" above their own level, or lowering "New
> members" out from under an unprivileged account), mirroring the
> existing encryption "can't undo" warning. Only fires when the user can
> actually edit permissions at all. Determining the user's own effective
> power level uses ruma's `RoomPowerLevels::for_user` — room versions 12+
> give creators an "infinite" power level absent from the `users` map
> entirely, which a naive `users`/`users_default` lookup would misread as
> unprivileged. New `Client::room_own_power_level`.

<!-- -->

> **Idle-CPU and animation-repaint performance fixes (2026-07-04, v0.8.12-unreleased).**
> Found via `perf` while profiling why the app used measurable CPU at rest.
> Disabled matrix-sdk's default cross-process store-lock lease renewal
> (writes to the event-cache/crypto SQLite stores every 50ms for the whole
> session, meant to guard against a second OS process sharing the store —
> unnecessary since Tesseract already enforces single-instance-per-profile).
> Fixed a bug where an animated inline sticker/GIF forced a full repaint of
> the entire visible UI on every ~16ms animation tick instead of just the
> animated region (confirmed costing ~50% of CPU while otherwise idle with
> a sticker on screen): a `Canvas::clip_rect()` bug on Qt6/macOS/Win32, and
> a per-image overlay-widget mechanism built for GTK4, which has no
> partial-invalidation API for a single widget at all. Also fixed inline
> video re-establishing a hardware decode session (e.g. a CUDA context)
> every time a room with an already-seen video was revisited, instead of
> resuming the existing paused player.

<!-- -->

> **Voice message auto-advance (2026-07-01, v0.8.12-unreleased).**
> A voice message that finishes playing on its own now automatically
> starts the next voice message from the same sender in the room, if any.

<!-- -->

> **Scroll-position stability during pagination (2026-07-01, v0.8.12-unreleased).**
> Loading more history — backward while scrolled to the top, or forward
> while browsing old messages — no longer shifts what the user is actually
> looking at; only the scrollable range grows. The auto-scroll-to-bottom
> behavior is now correctly limited to a live message arriving while
> already pinned to the tail.

<!-- -->

> **Room join/leave timeline events (2026-07-02, v0.8.12-unreleased).**
> An opt-in setting (Settings → Appearance, default off) surfaces
> join/leave/kick/ban/invite/knock membership transitions in the message
> timeline. Consecutive same-action events collapse into one summary line
> with stacked avatars (e.g. "Alice, Bob and 3 others joined the room"),
> expandable into individual lines on click.

<!-- -->

> **Room settings — edit name/topic/avatar (2026-07-02, v0.8.12-unreleased).**
> A wrench icon in the room-info panel opens a full-panel view for staging
> edits to the room's avatar, display name, and topic, gated per-field by
> power level. Nothing is sent until Accept; Cancel discards everything.

<!-- -->

> **Screen-share picker thumbnails + Linux stability (2026-07-03, v0.8.12-unreleased).**
> The screen-share picker now shows real per-source thumbnails instead of
> placeholder tiles. Along the way, fixed a black-tile bug on Linux
> (xdg-desktop-portal + PipeWire), a UI freeze on stopping a stalled
> capture, unclosed portal sessions leaking past process lifetime, solid-
> black Windows capture of GPU-composited apps, and a macOS thumbnail
> deadlock.

<!-- -->

> **Location map click-through (2026-07-04, v0.8.12-unreleased).**
> Clicking (not panning) a location message's embedded map opens it on
> openstreetmap.org, centred on the pin.

<!-- -->

> **Media caption linkify (2026-07-01, v0.8.12-unreleased).**
> Captions on image/file/video messages now go through the same rich-text
> pipeline as regular message bodies instead of a plain-text-only paint path.
> Bare URLs in a caption render as clickable links and are hit-tested the same
> way a link in a normal text message is, with no Rust/FFI changes needed —
> the shared body-layout helper only ever read the body/formatted-body/event-id
> fields and never branched on message kind.

<!-- -->

> **Room-switch viewport auto-backfill (2026-07-01, v0.8.12-unreleased).**
> Switching rooms only ever fetched one fixed-size page of history up front;
> the only other backfill trigger required a manual scroll gesture and was
> skipped outright while the view was still pinned to the bottom (always true
> right after a switch). If that first page rendered fewer rows than the
> viewport could hold, the timeline sat under-filled until the user scrolled.
> The shared list view now proactively requests the next page whenever
> just-laid-out content doesn't fill the viewport, chaining through the
> existing prepend → relayout cycle until either the viewport fills or the
> room's real history is exhausted. Benefits both the room timeline and the
> thread panel, which share the same mechanism.

<!-- -->

> **MatrixRTC voice/video calls (2026-06-25, v0.8.12-unreleased).**
> Native LiveKit-based MatrixRTC calls (MSC4143), behind
> `TESSERACT_ENABLE_CALLS`, interoperating with Element X and Element Call.
> End-to-end encryption uses HKDF key derivation matching Element Call's wire
> format; echo cancellation runs through each platform's native audio device
> manager. The call overlay (`ParticipantTile`, `CallOverlayWidget`) has four
> modes — Docked, DockedExpanded, Floating (draggable, position persisted),
> and Popout (a dedicated OS window via `CallWindowBase`) — with mute/video/
> hang-up controls, a duration timer, and a pinned-tile 70/30 grid split.
> `IncomingCallBanner` surfaces MSC4075 ring notifications. The call button
> and incoming-call banner are hidden entirely when the server doesn't
> advertise LiveKit transport support, and for bridged rooms (MSC2346).

<!-- -->

> **`/selfie` slash command (2026-06-25, v0.8.12-unreleased).**
> Typing `/selfie` in the composer opens a full-surface camera overlay with a
> 3-second countdown and mirrored live preview; the captured still is
> JPEG-encoded via each platform's native API and inserted as a compose-bar
> attachment. Disabled while a call is active.

<!-- -->

> **Audio/video device selection (2026-06-25, v0.8.12-unreleased).**
> Settings → Media gained microphone, speaker, and camera dropdowns,
> enumerated at settings-open time and applied at the next session start.
> Backed by `QMediaDevices`/`GstDeviceMonitor` (Qt6/GTK4), WASAPI + Media
> Foundation (Win32), and `AVCaptureDeviceDiscoverySession` + CoreAudio
> (macOS). A new `tk::FormLayout` widget auto-sizes the label column to the
> widest label, fixing three previously-misaligned rows.

<!-- -->

> **Bridged-room detection (2026-06-27, v0.8.12-unreleased).**
> Rooms bridged via a third-party network (MSC2346 `uk.half-shot.bridge`
> state events) suppress the call button and threads panel and show a
> 🌉 Bridged badge in the room-info panel.

<!-- -->

> **Space root view (2026-06-28, v0.8.12-unreleased).**
> Selecting a joined space itself (rather than drilling into a child room)
> now shows `SpaceRootView` — a centred summary panel with avatar, name,
> topic, and joined/unjoined child counts — mirroring `RoomPreviewView`'s
> layout for unjoined rooms.

<!-- -->

> **Phone icon for active calls (2026-06-27, v0.8.12-unreleased).**
> The room list shows a phone icon on any room with an active call, driven
> by `m.call.member` state filtered to non-expired memberships.

<!-- -->

> **Room-switch media fetching overhaul (2026-06-30, v0.8.11).**
> Fixed media requests appearing to freeze in rooms that trigger many at once, and
> the in-flight indicator lingering indefinitely after leaving a media-heavy room.
> The download gate's hard ceiling now exempts long-stale slots the same way the
> soft per-lane limit already did, so a burst against a slow homeserver no longer
> blocks all further downloads in freeze-then-burst waves. The initial-room-open
> media prefetch scales to the message list's real viewport height instead of a
> fixed 50-event window. The dominant cause, found via a new debug-tooltip
> instrumentation pass: every room switch eagerly fetched an avatar for the
> *entire* membership list (for mention-pill resolution), uncancelled — removed in
> favor of an on-demand fetch when a mention pill actually renders, now wired
> consistently on all four shells (previously Qt6-only) and in the room-info
> panel's member list. Sender/read-receipt avatars and reaction images for
> timeline rows are now grouped with the rest of the room's media so they cancel
> on room switch too. 895 C++ + 300 Rust tests.

<!-- -->

> **macOS thread-reset stack-overflow fix (2026-06-30, v0.8.11).**
> Fixed a crash when a thread's timeline reset while the message list was
> mid-layout: macOS wired its repaint requester to a full synchronous
> `Surface::relayout()` instead of a deferred OS repaint like the other three
> platforms, so a deferred scroll-to-event firing during layout re-entered
> `relayout()` from inside itself and recursed until the stack guard was hit.
> Now routes through `host().request_repaint()`, matching Qt6/GTK4/Win32.

<!-- -->

> **Group unread rooms (2026-06-24, v0.8.9).**
> An optional "Group unread rooms" toggle in Appearance settings adds an **Unread**
> section above Favorites in the room list. When enabled, every room with a visible
> unread indicator — notification count, highlight, or unmuted quiet unread — is
> collected there regardless of type, including rooms nested inside spaces (which
> previously were invisible at the root list until the user drilled into the space).
> Rooms leave the section automatically when read; favorites and spaces are never
> moved. The fix and the new section are implemented as a testable
> `filter_root_rooms()` free function alongside `classify_room_section()`, with a
> shared `ShellBase::refresh_room_list_()` consolidating the four per-shell
> implementations. 7 new Catch2 tests for the filter edge cases. **892 C++ tests**.

<!-- -->

> **Colored sender display names (2026-06-24, v0.8.9).**
> Sender names in the message timeline are tinted using a hash of the user's Matrix
> user ID, mapped into an 8-hue palette tuned for contrast in both light and dark
> mode. The color is stable across display-name changes because it keys on the mxid.

<!-- -->

> **Space topic preview in room list (2026-06-24, v0.8.9).**
> Space entries in the Spaces section now show the space's **topic** as the one-line
> preview instead of a last-message snippet. Spaces are containers rather than chat
> rooms, so the topic is the more useful summary. Falls back to name-only (centred)
> when the topic is absent.

<!-- -->

> **Forward message (2026-06-19, v0.8.8).**
> A "Forward message" item appears in the ⋯ more menu for any non-redacted,
> non-pending event (including messages from other users). Opens
> `ForwardRoomPicker` — a modal overlay with a NativeTextField search bar and a
> two-section list: selected rooms pinned above a divider, filtered unselected
> rooms below. Avatars load lazily. The Rust FFI `forward_event` fetches the
> original event, strips `m.relates_to`, and re-sends the raw content to each
> target room; all msgtypes are preserved. Wired once in
> `ShellBase::wire_main_app_widget_()` across all four platform shells.
> **870 C++ tests**.

<!-- -->

> **macOS dock badge + dock-click unread navigation (2026-06-17, v0.8.6).**
> The macOS dock icon now shows the total notification count as a red badge
> (aggregated across all signed-in accounts via the existing
> `notify_tray_unread_` call sites). Clicking the dock icon raises the window
> — including when hidden via the tray toggle — and navigates to the
> highest-priority unread room, matching the system-tray click behaviour on
> the other platforms. Implemented in `AppDelegate` / `MainWindowController` /
> `ShellBase`. **870 C++ tests**.

<!-- -->

> **Win32 body font raised 1 pt above the OS default (2026-06-17, v0.8.6).**
> `lfMessageFont` returns Segoe UI 9 pt on a typical Windows installation, which
> reads noticeably small next to modern chat clients. A named
> `kBodyFontPtOffset = 1` is added to the detected system size in
> `canvas_d2d.cpp`; since all font roles derive additively from the body base,
> the whole Win32 UI scales uniformly from 9 → 10 pt.

<!-- -->

> **Async space-summary and server-info FFI (2026-06-17, v0.8.5).**
> `get_space_child_summary` and `get_server_info` converted from blocking Rust
> `block_on` (which pinned a C++ worker thread for the full HTTP round-trip) to
> async FFI: tokio tasks run HTTP on Rust's own thread pool and deliver results
> via `EventHandlerBridge` callback. The C++ worker pool is reduced from 4 → 2
> threads. Eliminates the scenario where four concurrent 30-second-timeout
> summary fetches could saturate the pool and make the client unresponsive.
> `RoomSummary::from_json` added to deserialise callback payloads. **870 C++ tests**.

<!-- -->

> **System font scaling across all four backends (2026-06-15/16, v0.8.5).**
> Tesseract now reads the OS body font size at startup — `QApplication::font()`
> (Qt6), `GtkSettings gtk-font-name` (GTK4), `SystemParametersInfo
> NONCLIENTMETRICS` (Win32), `NSFont.systemFontSize` (macOS) — and derives all
> per-role sizes as additive offsets from that base (`FontRole::Body` = base,
> `FontRole::Small` = base−4, `FontRole::Title` = base+2, etc.; `FontRole::BigEmoji`
> = 2×base; `FontRole::InlineEmoji` = `(base+1)×5/4`). The UI therefore scales
> naturally with the user's accessibility font-size setting without any manual
> slider in Settings.

<!-- -->

> **Inline emoji scaling (2026-06-16, v0.8.5).**
> Unicode emoji in message bodies now render at ~125% of the body font size
> (`FontRole::InlineEmoji`). Emoji-only captions beneath images, stickers, and
> other media render at 2× body size (`FontRole::BigEmoji`) instead of body text
> size, matching the display size of standalone emoji messages.

<!-- -->

> **Automatic update checker (2026-06-16, v0.8.5).**
> A background checker queries the GitHub releases API at startup (and
> periodically) and shows an in-app banner when a newer version is available.
> Controlled by a "Check for updates automatically" toggle in Settings →
> Privacy; disabled by default on builds that set `TESSERACT_GITHUB_REPO=""`.

<!-- -->

> **MSC4133 extended user profiles (2026-06-14, unreleased).**
> Three new per-user profile fields in the account settings panel and the
> user-profile info panel: **Pronouns** (`io.fsky.nyx.pronouns`), **Timezone**
> (`us.cloke.msc4175.tz`), and **Biography** (`gay.fomx.biography`). Each
> field uses a stable read key (`uk.tcpip.msc4133.<field>`) with an unstable
> write key; the implementation reads stable then falls back to unstable, and
> always writes stable so the data is readable by any compliant client. A
> `get_extended_profile` / `set_extended_profile` FFI pair wraps matrix-sdk's
> `get_raw_account_data_event` / `set_account_data`; a re-fetch after every
> write keeps the UI in sync. `AccountSection` and `UserProfilePanel` gain
> three `NativeTextField` rows. All four shells wired; **283 Rust + 861 C++ tests**.

<!-- -->

> **Unjoined space-children section + `RoomPreviewView` (2026-06-14, unreleased).**
> When navigating into a space, a collapsible **"Not joined"** section appears
> below the joined rooms listing every child room the user hasn't yet joined.
> Clicking an unjoined row opens `RoomPreviewView` — a right-side panel showing
> the room name, avatar, topic, member count, and a **Join** button — without
> changing the active room. Under the hood: `space_children_all()` (new Rust SDK
> function; `space_children()` refactored to delegate) returns both joined and
> unjoined direct children. MSC3266 summaries for unjoined rooms are fetched
> concurrently via `join_all` with `InFlightGuard` RAII and a generation counter
> that cancels in-flight requests when the space changes. `RoomListView` renders
> the new `kSecSpaceUnjoined` section (collapsible via
> `room_section_space_unjoined_collapsed`). `MainAppWidget` gains
> `show_room_preview` / `hide_room_preview` virtual slots; `RoomPreviewView` is
> a new `ui/shared/views/` widget. Unjoined-row avatars are lazy-loaded on first
> paint, not eagerly on space navigation (same `on_room_avatar_needed` path as
> joined rooms). Wired in all four shells (Qt6, GTK4, Win32, macOS).

<!-- -->

> **Block-level Markdown rendering (2026-06-13, unreleased).**
> Headings (`#` through `######`), unordered and ordered lists (including nested),
> blockquotes, and tables now render visually in `MessageListView` across all
> four canvas backends, complementing the existing inline styles (bold, italic,
> code, strikethrough, links) and code-block syntax highlighting. Headings use
> `FontRole::UiSemibold`; list items indent with correct bullet / ordinal;
> blockquotes get an accent left-border stripe; tables use a fixed-width
> columnar layout.

<!-- -->

> **Full-text message search, incl. encrypted rooms (2026-06-11/13).**
> A global search overlay (**Ctrl+Shift+F** / **⌘⇧F**) searches your message
> history — **including encrypted rooms**, which the Matrix server-side
> `/search` endpoint cannot do. Search runs against a local **SQLite FTS5**
> index of *decrypted* message bodies in the per-account `search_index.db`
> (`sdk/src/client/search.rs`, external-content FTS5 + triggers). Indexing is an
> **opt-in privacy setting** (Settings → Privacy → Search, off by default,
> since it stores decrypted text at rest); enabling lazily backfills history,
> disabling clears the index. Population is incremental (every message indexed
> on the live timeline-diff + pagination paths) plus a one-time background
> history backfill per joined room on first enable. The overlay reuses the
> QuickSwitcher pattern (`ui/shared/views/MessageSearchView.*`); results show
> room · sender · snippet and clicking jumps to the message (reusing the
> event-permalink scroll/focus path, so off-screen events resolve via a focused
> timeline). While the toggle is enabled, Settings shows a live summary: message
> and room count, oldest indexed date, indexing progress ("indexing…" → "up to
> date"), and on-disk size ("· ~1.2 MB") from a `dbstat` vtab query. New FFI:
> `Client::search_messages` / `set_search_indexing_enabled` /
> `search_index_stats` / `search_index_size_bytes`, `SearchHit`,
> `on_search_results` / `on_search_failed`. Shared code in `ShellBase` /
> `EventHandlerBase`; wired in all four shells (Qt6, GTK4, Win32, macOS).
> 283 Rust + 861 C++ tests. Verified on **Qt6**.

> **In-room find-in-conversation search bar (2026-06-13).**
> **Ctrl+F** (Win32 / Qt6 / GTK4) and **⌘F** (macOS) opens a `RoomSearchBar`
> anchored below the room header. Matching rows are highlighted in the timeline
> with a tinted accent overlay; ↑ / ↓ (wrapping) navigate between hits. A
> **Paginate** checkbox automatically back-paginates when no more matches remain
> in the current window, fetching older history in a loop until a match is found
> or the start of the timeline is reached. The bar closes on Esc; status-bar
> feedback confirms fetch progress. Shared code in `ui/shared/views/`
> (`RoomSearchBar`, `MessageListView::set_search_matches` /
> `clear_search_matches`); wired in all four shells (Qt6, GTK4, Win32, macOS).

> **Shared jump-to-date picker (2026-06-13, unreleased).** The four native date
> pickers (Win32 `MonthCal`, GTK4 `gtk_calendar`, Qt6 `QCalendarWidget`, macOS
> `NSDatePicker`) are replaced by a single `DatePickerView` widget in
> `ui/shared/views/`. The picker registers as a `tk::Host` popup, draws through
> the shared canvas, and handles pointer and wheel input through the normal
> dispatch layer. Scroll-wheel over the year token navigates years; elsewhere
> navigates months. `ShellBase::handle_date_jump_()` centralises the previously
> duplicated `timestamp_to_event` + `subscribe_room_at` logic across all shells.

> **Room-switch performance overhaul (2026-06-11, unreleased).** Switching
> rooms is now instant and flicker-free. The SDK no longer emits an empty
> `on_timeline_reset` before the populated one; instead the UI clears the
> previous room's rows the moment the user clicks
> (`MessageListView::begin_switch_loading`) and shows a clean loading view —
> a centered spinner only if the load outlasts ~500ms, so warm/fast switches
> show nothing transient and never the old room under the new header. The
> display gate reserves media height from intrinsic `media_w/media_h` instead
> of blocking on image/video decode (reveals on text-ready; timeout 400→150ms),
> the content-addressed body-layout cache is retained across switches, and the
> per-switch blocking `load_prefs_json()` is gone (cached `PrefsData` via
> `Prefs::room_layout`). `subscribe_room` **reuses** a still-live timeline
> (restart its streaming task) instead of rebuilding, with a **bounded
> warm-subscription LRU** (`ShellBase::prune_warm_subscriptions_`) capping the
> previously-unbounded live-timeline growth. The four shells' duplicated
> subscribe/paginate workers are consolidated into one shared
> `ShellBase::start_room_subscription_`: `subscribe_room` on the single mut
> thread (emits the reset on every switch), the blocking network paginate on
> the shared pool so it never blocks the next switch's reset — fixing a
> spurious loading spinner on rapid A↔B switching. Plus O(1) room lookup
> (`ShellBase::room_by_id_`) on the switch path. All shared code; wired in all
> four shells (Qt6, GTK4, Win32, macOS) and **verified on Qt6, GTK4, Win32 and
> macOS**. New Catch2 suites for the room-switch gate and warm-subscription
> LRU + room index, plus added prefs / layout-cache / loading-state cases —
> **813 C++ + 226 Rust tests**.

> **Multi-window: one window per account.**
> Ctrl+click an account in the picker opens it in its own window; clicking an
> account that already has a window raises that window instead of switching in
> place. Each account has a single SDK event bridge, now **re-pointable**
> (`EventHandlerBase::set_shell`, atomic): on spawn the account's bridge follows
> the new window (`ShellBase::hand_account_to_spawned_window_` — re-point + seed
> caches from the spawning window for instant paint + pin + register dedicated),
> so the existing `MainWindow` machinery drives the full room list, live timeline,
> media and sending with no new routing layer. A single app-wide tray icon is
> enforced by an `AccountManager` owner guard; on close a popped-out window hands
> its bridge back to the primary (`on_window_closing_`). The per-window destructor
> only tears down the **shared** accounts' sync when it's the primary (non-pinned)
> window, so closing a secondary no longer stops sync for every account. Shared
> code in `ShellBase` / `AccountManager`; wired in all four shells (Qt6, GTK4,
> Win32, macOS). 4 new Catch2 tests. Verified on **Qt6**.

> **Unread-room message prefetch.**
> Rooms that quietly accumulate unread messages (unread, **not** muted — no
> notification) now have their recent messages warmed into the SDK event cache
> ahead of time, so opening them renders instantly instead of fetching on click.
> The shell reconciles on every `on_rooms_updated` tick: it selects the
> quiet-unread rooms (excluding the open room), sorts most-recently-active first,
> caps at **20** (the cap *is* the LRU eviction), and fires a new
> `Client::start_unread_prefetch` FFI only when a fingerprint over the capped
> `(room_id, unread_count)` set changes — so new messages in an already-prefetched
> room re-warm it, but routine sync churn doesn't. The Rust side runs a **one-shot**
> silent backfill (build temp timeline → paginate → drop; events persist in the
> SQLite cache) on a task **independent** of the inactive-grouping backfill, with
> bounded concurrency; it skips rooms with a live timeline (open + pop-outs).
> Default-on (`prefetch_unread_rooms` setting). All shared code (`ShellBase` +
> `app/UnreadPrefetch.h`), so every shell benefits. 4 new Rust + 6 new C++ tests.

> **Pre-launch hardening + decomposition (2026-06-09, unreleased).** A full-tree
> code review (`docs/CODE_REVIEW_2026-06-09.md`) drove a large correctness /
> safety / dedup pass and the start of a god-object decomposition: shells routed
> through shared `ShellBase` handlers, a multi-window/logout use-after-free and an
> FFI aliasing-UB closed, ~1,250 LOC of cross-shell duplication hoisted into
> `ShellBase`, shared toolkit/view bases extracted, and the `MessageListView` /
> `ShellBase` god-objects partially split into collaborator classes. No
> user-facing feature change; 734 C++ / 208 Rust tests. See [CHANGES.md](CHANGES.md)
> and `docs/TODO-phase5-remaining.md`.

> **Unified Lucide icon set.**
> The composer (emoji / sticker / mic / stop), message hover-action bar
> (react / reply / thread / edit / more) and its overflow menu (delete / pin),
> the image & video viewers (close / download / play), the voice / audio /
> inline-video play glyphs, the room-list join (+) button, and the room-header
> jump-to-date & threads buttons all render from monochrome
> [Lucide](https://lucide.dev) SVGs instead of Unicode glyphs or hand-drawn
> shapes. Icons are embedded at build time (`ui/icons/lucide/` → generated
> `icons.h`), rasterized by nanosvg, and **tinted** to each context's theme
> colour via a new `tk::rasterize_svg(..., Color tint)` overload. A reusable
> `tk::IconCache` holds each rasterized icon and re-rasterizes only when the
> DPI scale **or** tint changes, so icons stay crisp on HiDPI and recolor
> correctly on light/dark theme switches. `PopupMenu::Item` gained an optional
> `svg_icon`. All shared code (`ui/shared/`), so every shell benefits.

<!-- -->

> **`matrix.to` and `matrix:` URI navigation (MSC2312).**
> Clicking a `matrix.to` or `matrix:` link in a message body navigates
> within the app instead of opening the browser. A link to a joined room
> navigates directly (resolved via the new `RoomInfo::canonical_alias`
> field); an unknown room opens the join dialog pre-filled with the alias
> or room ID. User links open the profile panel; event links scroll to
> the target event. All four platforms register as the OS handler for the
> `matrix:` URI scheme: Linux via `tesseract.desktop`
> (`MimeType=x-scheme-handler/matrix` + `%U`; Qt6 forwards via
> `QLocalServer`, GTK4 via `G_APPLICATION_HANDLES_OPEN`); Windows via
> `HKCU\Software\Classes\matrix` written at launch (no admin rights),
> with `WM_COPYDATA` single-instance forwarding; macOS via
> `CFBundleURLTypes` in `Info.plist` and `application:openURL:options:`
> in `AppDelegate`. `sdk/src/matrix_uri.rs` (19 Rust unit tests) parses
> both URI formats via ruma validators. `ShellBase::open_matrix_link()`
> owns the shared navigation and defers links received before login via
> `pending_matrix_link_`.

<!-- -->

> **Sticky, collapsible section headers in the room list.**
> Section headers (Favorites, DMs, Rooms, Spaces, Inactive) stick to the top
> of the room list while scrolling their section and are pushed up by the next
> section's header. The pinned header is fully interactive — click to
> collapse/expand the section, hover highlight works — via
> `RoomListView::dispatch_pointer_down/move` overrides that give the overlay
> priority over the inner `ListView` rows. Room titles render **semibold**
> (`FontRole::SidebarName`) when the room has unread messages; section-header
> titles use `text_primary` on a dedicated higher-contrast background
> (`section_header_bg` / `section_header_hover` palette tokens).

<!-- -->

> **Auto-scroll the room list to unread rooms.**
> When a room receives new messages, the room list scrolls the most-recent
> unread room into view so it is never hidden below the fold; with several
> unread rooms the newest wins, and a **space** counts when any of its child
> rooms is unread (its aggregated child notification count + newest child
> activity, computed in `ShellBase::apply_space_child_counts_`). Candidates
> exclude low-priority and Inactive rooms. The scroll is minimal — already-
> visible rooms aren't disturbed — and only genuinely newer activity re-triggers
> it (a per-view `last_unread_scroll_ts_` gate), so unrelated updates never yank
> the user's position. Built on a new deferred
> `tk::ListView::scroll_to_index_deferred` that applies once row heights are
> re-measured. The behavior is optional via a new **"Scroll to rooms with new
> messages"** Appearance checkbox (`Settings::autoscroll_unread_rooms`, default
> on). All shared code, so every shell (Qt6, GTK4, Win32, macOS) benefits.
> 9 new Catch2 tests. Verified on **Qt6**.

<!-- -->

> **Faster room switching & message bursts.**
> The message list no longer rebuilds and re-shapes every row when messages
> arrive or a room is opened. Body text layouts are shaped once and cached —
> reused across measure, paint, and repaints, keyed on width / theme /
> spoiler-revealed / content and LRU-bounded — so room switches and repaints
> stop re-shaping text (the cache reuses the existing event-id-keyed
> `link_layout_cache_`). `tk::ListView` gained targeted height invalidation
> (`invalidate_row` / `insert_row` / `erase_row` + an adapter
> `height_dependency_span`), so a single inserted/updated/removed message
> re-measures only a bounded day-block neighbourhood and rewalks offsets from
> there instead of the whole room; the keyless room-list path
> (`invalidate_data`) is unchanged, with a size-mismatch fallback to a full
> rebuild. Per-message relayout is coalesced in shared `ShellBase`
> (`schedule_relayout_`) so a sync burst triggers one layout pass, not one per
> message, with native-overlay timing unchanged. All three land in shared code,
> so every shell (Qt6, GTK4, Win32, macOS) benefits. 10 new Catch2 tests.
> Verified on **Qt6**.

<!-- -->

> **Pop-out room windows.**
> Ctrl/⌘+click a room tab to pop the room out into its own native window (the
> tab closes). Built on the shared `RoomWindowBase`, so all four shells (Qt6,
> GTK4, Win32, macOS) get the same behaviour with only thin per-platform glue
> (window/menu, pickers, native text-area overlay). A pop-out is a full room
> view: timeline with forward/back pagination, compose with reply/edit/media
> send and image-paste, emoji/sticker/reaction pickers, @mention autocomplete
> with avatars, animated inline media and pickers (driven by the shared 60 Hz
> tick via `repaint_anim_frame`), and a room info panel with members, topic
> edit, room tags, notification mode, leave, and ignore. Any path that targets a
> room already open in a pop-out (room-list click, ctrl/⌘+click, notification or
> tray navigation) raises the existing window via
> `ShellBase::focus_secondary_window_` instead of re-opening the room.
> Verified on **Win32**; the other shells mirror the same shared logic.

<!-- -->

> **GIF picker (`/gif`).**
> Type `/gif <query>` in the composer to search and send GIFs (Klipy SDK).
> Results appear in an animated horizontal strip above the compose bar (↑/↓/Tab
> to navigate, Enter to send, Esc to dismiss). Chosen GIFs are sent as
> autoplaying `m.video` carrying the `fi.mau.gif` vendor hint; E2EE rooms
> encrypt the MP4 and poster thumbnail via `EncryptedFile`. The Klipy
> `customer_id` is a SHA-256 hash of the local MXID so no raw identity leaves
> the device. The preview strip animates: a static JPEG thumbnail appears
> immediately while an animated WebP/GIF (or a first-frame extracted from MP4)
> is decoded off-thread via the native image backend; once ready it replaces the
> static thumbnail in place. Strip sources are persisted to `MediaDiskCache` so
> re-search skips re-downloading. Send format priority: MP4 → WebP → GIF, so
> bridges receive a `video/mp4` when WebP re-upload is not supported. Room-list
> last-message preview shows "sent a GIF" for `fi.mau.gif` vendor-hint events
> from bridges. Shared `GifEngine` / `GifPopup` / `GifController`; wired on all
> four shells (Qt6, GTK4, Win32, macOS). Unit-tested.

<!-- -->

> **Room navigation history (Alt+Left / Alt+Right; Cmd+[ / Cmd+] on macOS).**
> Back and forward navigation through the session's room visit history, like
> browser navigation. `ShellBase` maintains a `room_nav_history_` vector (capped
> at 100 entries) with a cursor; every organic room visit appends an entry and a
> new forward visit truncates any forward branch. `navigate_history_back()` /
> `navigate_history_forward()` are called under a `room_nav_in_progress_` guard
> so the history is not re-entered mid-navigation. Shortcuts are application-
> scoped on all four shells — they fire even while the compose box holds focus —
> with macOS using Cmd+[ / Cmd+] to avoid conflicting with the Option+arrow
> word-navigation shortcut. 10 new Catch2 tests.

<!-- -->

> **Quick switcher (Ctrl/⌘+K).**
> A centered command-palette overlay for jumping between rooms: a native search
> field with a horizontal "Recent" strip of recently-visited rooms and a full,
> name-filtered room list below. Up/Down navigate, Enter/click jump,
> Escape/click-outside close. Implemented once in `views/QuickSwitcher` and
> mounted as the topmost overlay in `MainAppWidget`; visit-order recency is an
> MRU list on `ShellBase` recorded at the `after_active_room_changed_`
> navigation chokepoint. `NativeTextField` gained a `set_on_popup_nav` hook so
> the arrow keys drive the list while the field holds focus, and each shell
> wires the accelerator so it fires even over a focused native edit control
> (Win32 accelerator table, Qt `QShortcut`, GTK `GtkShortcutController`, macOS
> `NSEvent` monitor).
>
> Typing a leading `@` flips the switcher into **user mode** to start a DM with
> anyone — including a previously-unseen mxid. It shows a live-filtered roster of
> known users (DM partners + members of joined rooms, built lazily on a worker
> thread in `ShellBase::build_known_users_roster_` and cached per account), and
> as a full `@user:server` is typed it debounce-resolves the profile
> (`Client::resolve_user_profile` → matrix-sdk `fetch_user_profile_of`) to
> confirm the user exists before offering the row. Activating a user row routes
> through the existing `ShellBase::handle_open_dm_` (`get_or_create_dm`) and
> navigates to the DM. The `@`-detection and rendering live entirely in the
> shared layer, so no platform shell changed.

<!-- -->

> **Encryption-setup overlay.**
> `EncryptionSetupOverlay`, a shared widget wired on all four shells, guides
> new-account users through enabling cross-signing. Fresh path: `Intro` →
> `ChooseMethod` (recovery key or passphrase) → `ShowKey` / `Progress` →
> `Done`. Recovery path: `Intro` → `EnterKey` → `Progress` → `Done`.
> Callbacks (`on_enable_recovery`, `on_recover`, `on_request_sas`, `on_close`)
> drive SDK calls; `advance_progress` / `report_error` advance the progress
> step after async operations resolve. `ShellBase` gates the overlay with
> `encryption_setup_shown_` / `encryption_setup_dismissed_` to prevent
> double-raise and post-dismiss reappearance.

<!-- -->

> **In-flight request indicator (animated spinning ring).**
> An animated ring in the status bar shows the number of currently in-flight
> Matrix API requests: green for 0–1, amber for 2–10, red for more than 10. The
> ring is a set of small dots orbiting a 16 px circle at a constant angular
> velocity (replaces the earlier static dot). A tooltip shows the exact count.
> Wired on Qt6, GTK4, Win32, and macOS. The macOS shell receives its status bar
> in the same pass (it previously had none), giving all four platforms parity for
> the sync-state label and inflight ring. **71 SDK operations** across room_list,
> account, send, verification, recovery, image_packs, pins, tags, and timeline
> are covered by `InFlightGuard` RAII, so the indicator accurately tracks every
> non-sync HTTP call. Shared draw logic in `tk::draw_inflight_indicator`
> (`tk/inflight_dot.h`); the count is re-read from the authoritative Rust atomic
> (`in_flight_count()`) on each change so it stays correct regardless of
> cross-thread notification ordering.

<!-- -->

> **Non-blocking media downloads.**
> Media fetches (room/user avatars, inline image/video/sticker thumbnails,
> full-size images, sticker/emoji picker images, map tiles, URL previews, and
> voice/audio playback) run as async `tokio` tasks rather than blocking calls
> that each pin a C++ worker thread. The Rust SDK exposes `fetch_media_async` /
> `fetch_url_async` / `get_url_preview_async`, which `rt.spawn` the download
> under a per-lane `Semaphore` — a wide interactive lane for small avatars and
> thumbnails, a narrow bulk lane for slow full-size/preview/tile/voice transfers
> — and deliver the bytes through an `on_media_ready` / `on_url_preview_ready`
> callback, so a slow or dead-server download can never starve the visible media
> the user is waiting on. `ShellBase` correlates each request via a UI-thread
> pending registry; a shared `fetch_media_pipeline_` does disk-read → async
> fetch → disk-store → deliver. Switching rooms calls `cancel_media_group` to
> abort the previous room's still-pending timeline downloads (grouped by room id)
> so the new room's media gets the slots. The voice play/scrub path no longer
> blocks the UI thread on an uncached clip on any of the four shells — bytes are
> warmed asynchronously and the view repaints when they land.

<!-- -->

> **Pinned events.**
> A `PinnedBanner` widget above the message list cycles through
> `m.room.pinned_events` with left/right chevrons and a counter; clicking
> the body text jumps to the pinned message (in-window scroll when loaded,
> `subscribe_room_at` otherwise). A Pin / Unpin entry appears in the hover
> action pill, gated by the current user's power level (`can_pin_in_room`
> reads `m.room.power_levels`). Architecture: `sdk/src/client/pins.rs`
> exposes `pin_event` / `unpin_event` (state-event read-modify-write) and
> `can_pin_in_room`; `RoomInfo` carries a resolved `Vec<PinnedEvent>` fetched
> via `Room::load_or_fetch_event` (cache → SQLite → `/event/{id}`). Pin
> changes flow through the existing `on_rooms_updated` path — no new FFI
> callback. `MessageListView` gains `set_can_pin` + `set_pinned_event_ids`;
> all four shells wire `on_pin_requested` / `on_unpin_requested` callbacks.
> 11 new tests.

<!-- -->

> **Room tags (favourite / low priority).**
> The room info panel shows two `tk::SwitchButton`s — Favourite and Low
> priority — as stacked rows between the topic and the member list.
> `SwitchButton` is a new shared widget (settings-style label + sliding on/off
> switch; accent track when on); a sibling `tk::ToggleButton` (accent-filled
> pill toggle) also lands for general reuse. The two tags are mutually
> exclusive both in the UI (toggling one clears the other) and server-side,
> backed by matrix-sdk's `Room::set_is_favourite` / `set_is_low_priority`
> via new fire-and-forget FFI `set_room_favourite` / `set_room_low_priority`.
> Tag state is carried on `RoomInfo` (existing `is_favorite` plus a new
> `is_low_priority`, read from a single `tags()` fetch), so the switches
> re-sync to authoritative server state through `refresh_info` on every room
> update. Wired through `RoomView` → `ShellBase` and the popout room windows.
> 4 new widget tests.

<!-- -->

> **Hover action pill.**
> The per-message hover affordances are consolidated into a single rounded
> pill of square cells (reply / thread / react / edit) anchored to the
> top-right of every row, with one shared outline and 1 px dividers. The
> hovered cell gets a subtle pressed fill. The `+react` chip moves onto the
> pill for rows with no existing reactions and stays at the end of the
> reactions strip when reactions are present; redacted and unable-to-decrypt
> rows suppress it. Destructive and moderator actions (Delete message, Pin /
> Unpin) are tucked behind a `⋯` overflow button that opens a `PopupMenu`
> overlay — a new shared `tesseract_tk` widget — so the pill stays tidy while
> the popup is open and the action context is preserved.

<!-- -->

> **Win32 windowless RichEdit compose bar.**
> The compose bar on Win32 is now driven by a windowless `ITextServices2`
> control (loaded from `MSFTEDIT.DLL` via `GetProcAddress`) rendered
> directly into the surface's `ID2D1HwndRenderTarget` via `TxDrawD2D`.
> The implementation covers `ITextHost2` (~51 pure virtuals); colour emoji
> render correctly via DirectWrite COLR/CPAL tables (`TO_DEFAULTCOLOREMOJI`
> + `TO_DISPLAYFONTCOLOR` typography options). All prior `NativeTextArea`
> behaviour is preserved: `replace_range`, @-mention pills, clipboard image
> paste, slash-popup navigation, Up-to-edit-last, IME. A companion change
> implements `IProvideFontInfo` to route emoji codepoints to Noto Color Emoji
> in all message rows on Win32.

<!-- -->

> **Win32 full HiDPI fix.**
> With `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`, Win32 APIs return
> physical pixels while D2D draws in DIPs. A systematic coordinate-space
> fix in `host_win32.cpp` converts mouse coords to DIPs before widget
> dispatch, scales logical DIP bounds → physical px in `set_rect()`,
> returns logical px from `natural_height()`, and converts all popup /
> picker `SetWindowPos` calls through DIP-to-physical helpers. Status-bar
> height is derived from the logical font height × DPI scale. Emoji and
> sticker pickers, and the Win32RichEditArea, now also honour dark mode.

<!-- -->

> **Tab session restore.**
> The full set of open room tabs is now persisted across restarts. On every
> room navigation the `im.gnomos.tesseract` Matrix account-data event is
> updated with an `open_rooms` array (all tab room IDs in visual order) and
> `last_room` (the active tab). On startup each shell reads these fields from
> `PrefsData` into `AccountSession::open_rooms` / `last_room`; on account
> switch `pending_restore_rooms_` (a vector replacing the previous single
> `pending_restore_room_` string) is populated from that snapshot. A new
> `ShellBase::try_restore_tab_session_()` helper pre-populates `tabs_` from
> the saved list and fires `on_tab_state_changed_ui_()` exactly once, so the
> full tab bar is reconstructed in a single pass with no inter-tab flickering.
> Backward-compatible: old prefs with only `last_room` auto-populate
> `open_rooms = {last_room}` on parse. The prefs serializer was migrated from
> hand-rolled JSON to `nlohmann/json` (the same library `settings.cpp` uses).
> 4 new unit tests; wired in all four shells (Qt6, GTK4, Win32, macOS).

<!-- -->

> **Matrix threads UI.**
> A new "threads" button in `RoomHeader` (immediately left of the calendar
> button) toggles a right-side panel that takes 40% of the room area, with the
> main `MessageListView` shrinking to 60%. The panel has three states:
> `Closed`, `List` (a `ThreadListView` showing every thread in the current
> room, sourced from `Client::list_room_threads`), and `Open` (a `ThreadView`
> showing one thread's reply timeline + its own `ComposeBar` that sends via
> `Client::send_thread_message`). Clicking a thread-preview chip rendered
> under any thread-root message in the main list jumps straight to `Open`;
> the thread header's close button returns to whatever previous state opened
> the panel. While a thread is open the main `MessageListView` dims and the
> thread's root row is outlined + auto-scrolled into view. In-thread replies
> are hidden from the main timeline (filtered at the `ShellBase` insert/update
> path and again as defence in depth inside `MessageListView`). `ShellBase`
> owns a pure `compute_thread_transition_` state machine + an applier that
> maps transitions to `subscribe_thread` / `subscribe_room_threads` calls;
> `EventHandlerBase` marshals the SDK's four `on_thread_*` callbacks +
> `on_threads_updated` to `handle_thread_*_ui_` virtuals gated on
> `(current_room_id_, current_thread_root_)`. Room switch closes the panel
> via a `RoomSwitch` transition fired before every `current_room_id_`
> assignment. Wired in all four shells (Qt6, GTK4, Win32, macOS). The thread
> list is ordered newest-at-bottom to match the message timeline: it opens
> scrolled to the newest, paginates older threads on scroll-up with anchored
> scroll (`preserve_top_through` + `on_near_top`), and re-backfills the full
> list on every open.

<!-- -->

> **Privacy settings tab — presence toggle and room key export/import.**
> A new "Privacy" tab in Settings contains two groups. The "Presence" group has a
> checkbox to enable or disable both outgoing presence publishing (`PresenceTracker`)
> and the Rust-side 60 s receive-polling loop (suspended while the window is
> hidden); the setting persists across restarts
> via a new `send_presence` field in `app_settings.json`. The "Encryption" group
> provides "Export room keys…" and "Import room keys…" buttons that walk the user
> through a passphrase prompt → file-picker → async SDK call; the high-level
> `matrix-sdk` `Encryption::export_room_keys` / `import_room_keys` APIs handle
> the password-encrypted Megolm key file format. All four shells provide native
> dialogs: `QInputDialog`/`QFileDialog` (Qt6), `GtkFileChooserNative`/
> `gtk_dialog_new_with_buttons` (GTK4), in-memory `DLGTEMPLATE`/`OPENFILENAMEW`
> (Win32), `NSAlert+NSSecureTextField`/`NSSavePanel` (macOS).

<!-- -->

> **Storage size display and cache-clear in About settings.**
> The About settings tab now shows a "Storage" section at the bottom-left (capped
> to its natural width rather than filling the page) with "Local cache" and
> "SDK store" size rows computed asynchronously when settings opens. A destructive
> "Clear all caches" button (with confirm dialog) wipes the media disk cache,
> waveform SQLite store, and the matrix-sdk event store, then refreshes the
> displayed sizes; credentials and active sessions are unaffected. Wired on all
> four shells (Qt6, GTK4, Win32, macOS).

<!-- -->

> **@mentions.**
> Typing `@` in the composer opens an autocomplete popup (mirroring the emoji
> shortcode popup) that filters the room's members as you type, with an `@room`
> entry pinned on top, keyboard nav (up/down/Tab/Esc) and click-to-accept.
> Selecting a candidate inserts an inline **pill**: a native rounded chip on
> Qt6 (`QTextImageFormat`), GTK4 (`GtkTextChildAnchor` label) and macOS
> (`NSTextAttachment`); Win32 inserts plain `@Name` text tracked in a registry
> (a styled chip there needs an EDIT→RichEdit migration — deferred). On send the
> draft becomes a plain `body` (display names), an HTML `formatted_body` with
> `matrix.to` mention links, and the intentional-mentions `m.mentions` field —
> auto-derived in the Rust SDK by scanning the outgoing HTML for `matrix.to`
> user anchors (`@room` sets `mentions.room` and is rewritten to plain text).
> Received mentions render as pills: `matrix.to` user links (and a literal
> `@room`) get a themed rounded background via a new `TextSpan` background
> field, drawn in `MessageListView` for all four canvas backends; clicking a
> pill opens the user's profile panel (resolved from room members) rather than a
> browser. Shared logic lives in `MentionEngine`, `MentionPopup`,
> `MentionController`, and `client/build_mention_message`; wired into the main
> window + pop-out on all four shells. Verified on **Qt6**; GTK4 builds clean;
> **macOS + Win32 are written but unverified** (no toolchain in the dev env).
> 144 Rust tests + 468 C++ tests.

<!-- -->

> **Account registration (OIDC `prompt=create`).**
> The login screen offers a capability-gated "New here? Create an account" link
> below Sign in. It reuses the existing OAuth browser-loopback flow with the OIDC
> `prompt=create` parameter so the homeserver's provider shows its signup page;
> the same `/callback` listener completes it and the user ends up logged in. The
> link appears only when the resolved homeserver advertises registration support
> (`prompt_values_supported` contains `create`), detected via a
> generation-guarded probe fired when discovery resolves. Driven entirely in the
> shared `LoginView` (no per-shell wiring); legacy `/register` / non-OAuth
> homeservers are out of scope.

<!-- -->

> **Group inactive rooms.**
> Appearance settings gain a "Room list" group with a "Group inactive rooms"
> toggle and an inactivity-period selector (1 week–6 months, default 1 month;
> a local `app_settings.json` preference). When enabled, the room list shows a
> fifth, default-collapsed "Inactive" section holding DMs and Rooms with no
> activity past the threshold; favorites and spaces are never grouped, the
> section is hidden when empty, and a room reclassifies out of it automatically
> when new activity arrives. Classification is a pure, unit-tested
> `classify_room_section()` helper in the shared `RoomListView`; the toggle is
> wired through all four shells (Qt6 / GTK4 / Win32 / macOS).

<!-- -->

> **Outgoing Matrix presence.**
> The app now publishes its own presence (`PUT /presence/{userId}/status`),
> not just receives it. A platform-agnostic `PresenceTracker` runs an
> idle-decay FSM: Online while the user is engaging with the app, Unavailable
> after 5 minutes of no input + no window focus, Offline on logout. Each shell
> feeds the tracker via the new `tk::Host::set_on_user_activity` hook
> (pointer / wheel events) plus a focus tap (`changeEvent` on Qt6,
> `notify::is-active` on GTK4, `WM_ACTIVATE` on Win32,
> `windowDidBecomeKey/Resign` on macOS) and a 30 s periodic tick. The
> homeserver PUT is fired from a detached worker so input handling never
> blocks. 114 Rust tests + 411 C++ tests.

<!-- -->

> **Code-block syntax highlighting and tinted backgrounds.**
> Fenced code blocks in messages now render with syntax colors. A new Rust
> `highlight_code(code, lang, dark)` FFI (the `syntect` crate, pure-Rust
> `fancy-regex` engine) tokenizes the block and returns per-run RGB from a
> light/dark theme; results are memoized in a bounded LRU in the C++ client
> wrapper since the renderer rebuilds spans on every paint. `markdown_to_html`
> now emits `<pre><code class="language-X">` (sanitized to an injection-safe
> charset) and the shared `html_to_spans` parses that class, decodes the
> source, and emits per-token colored `tk::TextSpan`s (new `has_color` / `color`
> fields). Unknown or absent languages fall back to plain monospace. Per-span
> foreground color is honored in all four canvas backends (Qt6 `QPainter`,
> GTK4 Pango, macOS CoreText, Win32 Direct2D `SetDrawingEffect`).
> Fenced ` ``` ` blocks also render on a tinted panel (a new `code_bg` palette
> token — light `0xD9DCE3`, dark `0x2E3138`) drawn as a single rounded rect
> enclosing the whole block; inline `` `code` `` gets a tight per-run tint,
> distinguished by the new `code_block` flag on `tk::TextSpan`.

For build instructions, architectural overview, and the open-roadmap items, see [CLAUDE.md](CLAUDE.md). For tracked open issues / known gaps, see the "Known gaps" section at the bottom of CLAUDE.md.

## Test coverage

| Suite | Count |
| ----- | ----- |
| Rust unit tests (`cargo test -p tesseract-sdk-ffi`) | 364 |
| C++ Catch2 tests via ctest (Qt6 preset) | 1120 |

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
- **Secure token storage** — per-platform `SecretStore` backend: Windows Credential Manager (`CredWriteW`/`CredReadW`), macOS Keychain (`SecItemAdd`/`SecItemCopyMatching`), Linux `libsecret` (probed at build time; plaintext stub fallback when absent). `SessionStore` migrates transparently from the legacy plaintext `session.json` on first load, writing a `{"v":2}` sentinel on success so subsequent starts bypass the migration path.
- **Session restore on startup** — `SessionStore` persists the full `PersistedSession` JSON on every token refresh and reloads it at launch. All open room tabs and the active account are also restored: the `im.gnomos.tesseract` account-data event carries an `open_rooms` array so the full tab workspace survives a restart.
- **XDG data/config split** — account data (per-account `accounts/<uid>/` tree with `session.json` + the matrix-sdk SQLite store, plus the `accounts.json` index) lives under `data_dir()`: `~/.local/share/tesseract/` on Linux, `%APPDATA%/Tesseract/` on Windows, `~/Library/Application Support/Tesseract/` on macOS. Only `app_settings.json` stays in `config_dir()` (`~/.config/tesseract/` on Linux); `data_dir()` equals `config_dir()` on Windows/macOS. `migrate_legacy_layout()` runs on startup and handles both the pre-multi-account single-account layout and a multi-account `accounts/` tree left under `config_dir()` by older builds (Linux), moving each into `data_dir()` crash-safely.
- **`logout`** — wipes Rust session, C++ wrapper state, and the SQLite store; surfaces back through the FFI.
- **Soft logout** — `SessionChange::UnknownToken` threaded through `on_error` with a `soft_logout` flag so the UI can retry restore without clearing the store.
- **Recovery key / device verification (Step 6)** — `needs_recovery`, `recover(key_or_passphrase)`, `backup_state` FFI; `on_backup_progress` callback; per-platform `RecoveryBanner` (in-toolkit; not a modal dialog).
- **Server capabilities on login** — `tesseract::ServerInfo` struct captures homeserver URL, Matrix spec versions, MSC3030 (Jump-to-Date) support flag, capability bits (`can_change_password`, `can_set_displayname`, `can_set_avatar`), and default room version; fetched concurrently via `/_matrix/client/versions` (no-auth) + `/_matrix/client/v3/capabilities` (Bearer) after `RoomListState::Running`; stored in `ShellBase::server_info_` for feature-gating across all four shells; Settings "Server" tab shows the homeserver URL.
- **Shutdown stability** — background workers are drained before the tokio runtime tears down, preventing use-after-free when a worker posts back to the UI thread after the EventHandler is destroyed; a separate guard prevents a double-callback segfault when `stop_sync` is called re-entrantly.
- **Identity strip in sidebar** — circular avatar + display name + right-click "Log Out" on every platform.
- **Single-instance enforcement** — a per-user OS lock prevents two app instances from running concurrently (`QLockFile` on Qt6, `GApplication` uniqueness on GTK4, a named mutex on Win32, `NSRunningApplication` check on macOS); the second launch exits with a notice.
- **Duplicate account guard** — after OAuth completes the shell checks existing `accounts_` for a matching `user_id` before committing to disk; re-adding the same account discards the temp store and returns to the last active account without side effects.
- **Startup restore error dialog** — when `restore_session()` fails at launch (network outage, transient server error), the login view displays a modal `AlertDialog` overlay ("Connection Error") with Retry and Sign In buttons instead of silently showing a blank login form. The session files are left untouched so Retry can re-attempt restore once connectivity returns; `SessionStore::clear_account()` is called only by `handle_auth_error()` on a confirmed `sync_auth_error` response. All four shells wired.

## Sync & rooms

- **Sliding sync via matrix-sdk-ui** — `SyncService` + `RoomListService` replace the legacy `sync_once` loop.
- **Initial-sync progress in the status bar** — `RoomListService::state` exposed via a new `on_room_list_state` FFI callback; each shell paints "Syncing rooms…" (debounced 300 ms) / "Reconnecting…" / "Downloading encryption keys (N)…" until both sliding-sync and key-backfill settle, then clears to "Connected". Wired on Qt6, GTK4, and Win32; macOS deferred (no status-bar surface).
- **Per-room `Timeline` handles** — `HashMap<OwnedRoomId, TimelineHandle>` keyed by room; subscribed lazily.
- **Timeline FFI** — `subscribe_room`, `unsubscribe_room`, `paginate_back`, `paginate_back_with_status` (reports `reached_start`); position-aligned `on_timeline_reset` / `on_message_inserted` / `on_message_updated` / `on_message_removed` callbacks mirror matrix-sdk-ui's `VectorDiff` semantics.
- **Back-pagination on scroll-to-top** — UI fires `paginate_back` when the user reaches the top; in-place insertion preserves the visual scroll position. Scroll preservation is **row-anchored** (`ListView::ScrollAnchor` + `ListAdapter::row_key`): the row under the cursor (or the top-of-viewport row) is pinned to its screen position across prepends *and* async row-height growth (images, URL previews, voice waveforms decoding in/above the viewport), with the hover highlight re-resolved to the same message after the relayout. Keyless lists (room/thread) fall back to the legacy total-height delta.
- **Background backfill** — `start_background_backfill` walks every joined room not currently subscribed and warms the persistent event cache with bounded concurrency.
- **Async room actions** — text sends, reactions, pagination, room join/leave/invite-accept/decline, and file uploads converted from blocking C++ worker-thread calls to fire-and-forget `rt.spawn()` tokio tasks delivering results via `IEventHandler` callbacks (`paginate_back_async`, `accept_invite_async`, `send_image_async`, etc.). Blocking wrappers removed.
- **Kind-aware last-message preview** — each room row's preview uses `formatted_body`'s first plain line for text/notice/emote, shows "\<sender\> sent an image/video/file/voice message" for media kinds, and draws an inline ~28 px thumbnail for sticker last-messages (`RoomListView` `sticker_provider_` backed by the shells' shared image cache; wired on all four platforms).
- **Unread highlighting** — a room with unread messages is bolded and badged by severity: a **mention** shows an accent count pill, a **notifying** room a neutral count pill, and a room with **unread messages that don't notify** (e.g. set to "mentions only") a **bold name + small neutral dot** — so quiet-but-unread rooms are no longer invisible. Muted rooms are excluded (silenced on purpose). The decision is one pure `unread_style_for(notification, highlight, unread, muted)` helper (`views/roomlist_unread.h`) consumed by both the row and the collapsed section-header rendering; `RoomInfo` carries `unread_count` (`Room::num_unread_messages()`) and `muted` (`cached_user_defined_notification_mode`), and the room-list update-dedup fingerprint includes the quiet-unread state so the dot appears and clears live.
- **Tombstoned (upgraded) rooms hidden** from the room list.
- **Runtime offline banner** — when sync loses connectivity (`sync_offline` / `sync_error`), a 32 px amber "No internet connection — reconnecting…" strip appears at the top of the chat panel; it auto-hides when `RoomListState` returns to `Running`. `ShellBase::offline_` tracks the flag; `EventHandlerBase` wires both transitions; `MainAppWidget::set_offline(bool)` drives the banner. All four shells benefit with no per-shell changes.
- **Graceful shutdown** — `Drop` on `ClientFfi` calls `stop_sync()`.
- **Non-blocking FFI lock (room-switch freeze fix)** — the C++ `Client` no longer serialises every FFI call behind one coarse `std::mutex` held across blocking `block_on`s. The read + dispatch bridge methods are now `&ClientFfi` (interior-mutable Rust state: `thread_lists` / `thread_timelines` moved behind `parking_lot::RwLock`), guarded by a `std::shared_mutex` taken in shared mode; only ~15 genuine writers (`start_sync`, `restore_session`, `logout`, …) take the exclusive lock. The UI thread's cheap room-switch reads (`list_room_threads`, `subscribe_room_threads`) now run concurrently with a worker mid-`subscribe_room` timeline build instead of freezing behind it.
- **Low-power CPU optimisations** — the sync worker no longer fans out into matrix-sdk SQLite queries on every notable update. The room-info watcher coalesces `RoomInfoNotableUpdate` bursts in a 150 ms window and folds their reasons, skipping the image-pack/prefs rebuild when only read-receipt / latest-event / recency bits are set. `sync_room_subscriptions` is diff-aware — a re-selection of the already-open room or a thread toggle that lands in an already-subscribed room is a no-op. The presence polling loop reads a cached DM-counterpart set (refreshed from `RoomInfo.dm_counterpart_user_id` after every room-list rebuild) instead of walking every joined room with a `dm_other_user` lookup per tick, the tick interval is raised from 30 s to 60 s, and the loop is suspended entirely while the window is hidden/minimized/unfocused (re-enabled with an immediate one-shot kick on focus regain via `Client::poll_presence_now`). On low-end laptops these collapse a previously dominant `chunk_large_query_over` hotspot.

## Spaces (Step 7)

- `is_space: bool` on `RoomInfo`; spaces shown at the bottom of the room list with `#` prefix on Qt6 / GTK4 (top-row dedicated bar on macOS).
- `space_children(space_id)` FFI returning joined direct children; `space_children_all(space_id)` returning all direct children (joined + unjoined).
- **Stack-based drill-in navigation** — selecting a space replaces the room list with its children; `←` back button + space name label at the top of the sidebar; recursive sub-spaces; auto-pop to "All rooms" when the stack is empty.
- **Space children hidden from the root room list** — they appear only when navigating into the parent.
- **Unjoined space children** — a collapsible "Not joined" section below the joined-rooms list shows every child room the user hasn't joined. Clicking opens `RoomPreviewView` (name, avatar, topic, member count, Join button) without leaving the current room. Summaries fetched concurrently via MSC3266 with generation-based cancellation.

## Shared UI toolkit (`tesseract_tk`)

- **`tk::Canvas`** — abstract 2D backend with four concrete impls (`canvas_d2d`, `canvas_qpainter`, `canvas_cairo`, `canvas_cg`). Color / Rect / Point / Image / TextLayout primitives; rounded-rect, stroke, push/pop clip; circle-cropped image draw; initials disc helper.
- **`tk::Widget`** — measure / arrange / paint + pointer / wheel dispatch with `dispatch_pointer_down` + `world_to_local` capture semantics.
- **`tk::Host`** — per-platform integration surface (repaint scheduling, post-to-UI, native edit overlays). `request_repaint`, `post_to_ui`, `make_text_field`, `make_text_area`, `make_audio_player`, `make_audio_capture`, `encode_for_send`.
- **Native text overlays** — `NativeTextField` (`QLineEdit` / `GtkEntry` / Win32 EDIT / `NSTextField`) and `NativeTextArea` (`QTextEdit` / `GtkTextView` / multi-line EDIT / `NSTextView`) for IME-friendly input. `set_placeholder` is implemented on all four platforms (GTK4 uses a `dim-label` `GtkLabel` overlay child since `GtkTextView` has no native placeholder API).
- **Shared views** — `LoginView`, `RoomListView`, `MessageListView`, `EmojiPicker`, `StickerPicker`, `RecoveryBanner`, `ComposeBar` mounted identically on every platform.
- **`AlertDialog`** — modal overlay widget (not backdrop-dismissible) with a title, body, and up to two configurable action buttons (`open(Options, primary_cb, secondary_cb)` / `close()` / `is_open()`). Used by `LoginView` to surface startup restore errors; available for other blocking error prompts.
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
- **Block-level Markdown rendering** — headings (`#` through `######`), unordered and ordered lists (including nested), blockquotes, and tables render visually in `MessageListView` across all four canvas backends. Headings use `FontRole::UiSemibold`; list items indent with correct bullet / ordinal; blockquotes get an accent left-border stripe; tables use fixed-width columns. Complements the existing inline styles and code-block syntax highlighting.

## Media

- **`fetch_media_bytes(mxc)`** / **`fetch_source_bytes(source_json)`** — synchronous wrappers around matrix-sdk's media cache; the latter handles plain mxc + encrypted `EncryptedFile` transparently.
- **Avatars** — sender avatars (24 px per row) + room avatars (36 px); circular crop via `draw_circle_image`; initials-disc fallback when bytes aren't yet cached. Rooms without a custom avatar fall back to the *other participant's* avatar in 1:1 chats (`RoomInfo::dm_avatar_url`, populated in Rust by inspecting `m.direct` first and then filtering joined members by `service_members` per MSC4171); render sites read via the inline `effective_avatar_url()` accessor and `ShellBase::ensure_room_avatar_` routes the DM-fallback fetch through `fetch_media_bytes` so the cache key naturally dedupes with the user's avatar elsewhere.
- **Lazy room-list avatars** — room-list avatars are requested only when a row is first painted (`RoomListView::on_room_avatar_needed` fires from `paint_row` on a cache miss, wired to `ensure_room_avatar_` in `ShellBase::wire_main_app_widget_`), so rooms in collapsed or off-screen sections fetch nothing until scrolled into view. The former per-shell "fetch every room" loops are gone.
- **Visible-first download priority** — the per-lane FIFO `tokio::Semaphore` is replaced by a `PriorityGate` over a pure `MediaQueue` (priority desc, then FIFO seq). The timeline still eagerly enqueues every row's media at `Normal`, but a `MessageListView::on_visible_range_changed` callback (frame-coalesced, de-duped; re-exposed via `RoomView`, bound once in `wire_main_app_widget_`) calls `prioritize_media(group, ids)` so the media for the rows currently on screen jumps ahead of the off-screen backlog — and re-prioritizes as the user scrolls. Covers all four shells + the thread panel.
- **Stuck-download reclamation** — matrix-sdk media is a single opaque await with no progress hook, so a stalled fetch would otherwise hold its lane slot until the 30/120 s timeout and freeze the queue. A slot held past an 8 s stall deadline stops counting against the lane limit (the gate grants the next, highest-priority waiter while the stuck download keeps draining in the background), and a hard ceiling (2× the lane) bounds total concurrent connections. Healthy downloads still behave exactly like the old semaphore.
- **Bounded fetches** — every media download runs under a per-request timeout (30 s thumbnails/avatars, 120 s full files), so a stalled or endlessly-retrying request can't hang a read-pool worker thread or pin the in-flight indicator.
- **HTTP/2 multiplexing** — the reqwest media client uses HTTP/2 prior knowledge so parallel MXC downloads share connections; `MEDIA_BULK_PERMITS` is 10 concurrent fetches to take advantage of the extra bandwidth.
- **Failed-fetch backoff** — a fetch that returns empty (network error / 5xx / timeout) is recorded in a per-key exponential-backoff cache (30 s → 30 min); the `ensure_*` avatar/media paths skip a key still in cooldown, so an unreachable avatar (e.g. a forgotten DM on a dead homeserver) stops being re-requested on every sync tick. The backoff state is persisted to `app_cache.db` across sessions so it survives a restart. Cleared on success and on cache-wipe.
- **Inline images** — thumbnail to max 320 × 200, MSC2530 caption rule applied, rounded-rect chrome. Bytes are decoded off the UI thread on all four shells (`QImageReader` on Qt6, WIC on Win32, `CGImageSource` on macOS, `GdkPixbuf` on GTK4) and posted back via `post_to_ui_` so large images never stall paint or input.
- **Media-preview gating (MSC4278)** — a global `media_previews` setting (`Off` / `Private` / `On`, default `On`) backed by the `m.media_preview_config` account-data event controls whether inline image/sticker/video thumbnails auto-load. Suppressed media renders a BlurHash (MSC2448) placeholder behind a click-to-load pill and is not fetched until revealed; `Private` mode suppresses only in public rooms (resolved against each room's cached `join_rule`, with the per-room `m.media_preview_config` override applied on top). The decision is a single pure function (`app/media_preview_policy.h::media_allowed`) consulted at both the receive-time fetch gate and the paint-time placeholder predicate, so a revealed/allowed item is fetched exactly when it is shown. **The user's own media is exempt from public-room suppression in `Private` mode** (you already have it locally and it is never a privacy/safety concern to you), but `Off` still suppresses everything including your own uploads. Wired once in `ShellBase`, so all four shells share it.
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

- **`sdk/src/image_packs.rs` aggregator** — user pack (`im.ponies.user_emotes` / `m.image_pack`), enabled-rooms list (`im.ponies.emote_rooms` / `m.image_pack.rooms`), per-room state events (`im.ponies.room_emotes` / `m.room.image_pack`). Reads combine the stable + unstable event names (`merge_pack_contents`) at every read site instead of stopping at the first hit. 16 unit tests.
- **Spec-correct usage semantics** — missing/empty `usage` → both sticker + emoticon allowed; per-image `usage` overrides pack-level.
- **Per-room discovery** — a shared full-state fetch (`RoomStateCache`, triggered on room switch) rather than a single guessed `state_key`, since packs can use non-empty state keys and sliding sync doesn't deliver custom `m.room.image_pack` state; cached in a lazily-built `room_image_pack_cache`.
- **Picker/popup scoping** — emoji/sticker picker tabs and the inline `:shortcode:` popup filter to the personal pack, the currently-open room, and explicitly subscribed rooms; each pop-out window computes its own filtered list.
- **FFI surface (reads)** — `list_image_packs`, `list_known_room_packs`, `list_pack_images`, `list_favorite_stickers`, `user_pack_has_sticker`.
- **FFI surface (writes)** — `send_sticker`, `save_sticker_to_user_pack`, `toggle_favorite_sticker`, `remove_user_pack_image`, `rename_user_pack_image`, `set_pack_room_subscribed` (dual-writes stable + unstable event types, forces a synchronous rebuild so `is_subscribed` is correct before returning), `save_room_pack` (wholesale-replaces a room/space pack's images, matching the editor's full-snapshot staging model), `remove_room_pack` (empties a pack — Matrix has no true state-event delete; discovery skips zero-image packs), `can_set_room_image_packs` (power-level gate, mirrors `can_set_room_name`).
- **`IEventHandler::on_image_packs_updated`** — fires whenever the cache is rebuilt; pickers refresh in place.
- **UI** — `ImagePackEditorView` (multi-pack room/space editor, `RoomSettingsView`'s "Emojis & Stickers" tab) and `ImagePacksSection` (`UserPackEditor` + `KnownPacksList`, global `SettingsView` tab) share tile-grid logic via `ImagePackTileGridBase`.

## Compose bar

- Shared `tesseract::views::ComposeBar` on every platform via `tk::*::Surface`.
- Multi-line expanding input via `tk::NativeTextArea` (auto-grows 56 → 160 px, clamped).
- Send-on-Enter, Shift+Enter inserts a newline.
- Emoji + sticker + send buttons painted by the toolkit.
- Send button gates on trimmed non-empty content.
- Clipboard image paste; file drag-drop; pending-image / pending-file preview chip with clear button.
- Reply-mode banner (`kReplyBandH = 44 px`) with sender + body snippet and "×" cancel; edit-mode banner (`kEditBandH = 44 px`) with "×" cancel; both modes mutually exclusive.
- **Slash commands** — `SlashCommandEngine`/`SlashCommandPopup` autocomplete (typing `/` opens the popup); `dispatch_compose_send` routes recognised commands: `/me` + `/slap` → `m.emote`, `/shrug` appends `¯\_(ツ)_/¯`, `/spoiler [(reason)] <text>` → `m.text` with a `data-mx-spoiler` span (MSC2010; content rendered through inline markdown). Unknown `/foo` is sent verbatim.

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
- **Native text field color sync (Qt6)** — `QLineEdit`/`QTextEdit` hold an explicit `QPalette` with no automatic dark-mode following, unlike GTK4/macOS/Win32. `apply_theme_ui_()` re-applies `set_text_color(palette.text_primary)` to every native field on the Qt6 shell (room search, quick switcher, message search, forward picker, find-in-room, topic/room-settings/image-pack fields, encryption/QR-grant fields) on every theme change, not just construction.

## System tray

- **All four platforms** — system-tray icon with **Show App** / **Quit** popup menu. Closing the main window hides it (the SDK keeps running, sync stays warm); Quit on the tray menu does the real exit.
- Cross-platform `tesseract::ITrayIcon` abstraction; per-platform impls created after login (mirrors `INotifier`).
- **Qt6** — `QSystemTrayIcon`; `is_available()` from `QSystemTrayIcon::isSystemTrayAvailable`. Falls back to plain quit when no system tray is present.
- **GTK4** — pure `org.kde.StatusNotifierItem` + `com.canonical.dbusmenu` implementation over GDBus (`GtkSniTrayIcon`; icon rendered with gdk-pixbuf + cairo). Replaces the former `libayatana-appindicator3` tray, which pulled libgtk-3 into the GTK4 process and aborted `gtk_init()` with "GTK 2/3 symbols detected" — there is no longer any appindicator (GTK3) dependency.
- **Win32** — `Shell_NotifyIcon` against a hidden helper HWND; `TrackPopupMenuEx` for the right-click menu; `WM_CLOSE` intercepted in `MainWindow`'s wnd_proc.
- **macOS** — `NSStatusItem` with a template menu-bar icon; `windowShouldClose:` hides the window; Quit calls `[NSApp terminate:nil]`.
- **Unread overlay** — when any signed-in account has rooms with notifications, the tray icon gets a small coloured dot in the bottom-right (accent blue for unread, destructive red for highlights / mentions). Aggregation lives in `ShellBase::compute_tray_unread` over `per_account_rooms_`; the `ITrayIcon::set_unread` hook is implemented per shell (QPainter overlay on Qt6, pre-rendered Cairo PNGs swapped via `app_indicator_set_icon_full` on GTK4, GDI+ ARGB compositing into `CreateIconIndirect` on Win32, `NSImage lockFocus`+`NSBezierPath` on macOS).

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
- **Per-room notification settings** — a Notifications section in `RoomInfoPanel` with a four-option dropdown (Default / All messages / Mentions / Off) mapped to Matrix per-room push rules (`RuleKind::Override` + `EventMatch` for "off", `RuleKind::Room` for "all"/"mentions", no rule for "default"); backed by a new shared `tk::ComboBox` widget and wired through both the main window and pop-out room windows; Rust `client.rs` reads/writes `m.push_rules`.
- All platforms suppress the notification when the window is focused and the target room is already open.

## Build & packaging

- **Corrosion** fetched at configure time (no global Rust toolchain install requirement beyond `rustup`).
- **`WHOLE_ARCHIVE` link** for the 3-way circular dependency between `tesseract_sdk_bridge_cxx`, `tesseract_client`, and `tesseract_sdk_ffi-static`.
- **Cross-platform CMake presets** — `windows-debug`, `windows-release`, `linux-debug`, `linux-release` (builds GTK4 + Qt6), `macos-appkit-{arm64,x86_64}-{debug,release}`.
- **CPack installer packaging** — NSIS on Windows, DMG on macOS (see [PACKAGING.md](PACKAGING.md)).
- **Bundled SQLite** via matrix-sdk's `bundled-sqlite` feature; no system OpenSSL dep (TLS uses rustls).

---

## Maintenance note

Update this file after every major feature lands — append a new bullet (or extend an existing one) in the right category, refresh the test counts in the table at the top, and bump the "Last updated" date.
