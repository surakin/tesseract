# Changelog

Newest first. Unreleased work is listed per day, one bullet per change.
Tagged releases summarize all changes since the previous tag.

## v0.8.15 — 2026-07-14

### Summary

- fix(win32): fix a use-after-free in native text field/area destruction — `Host::areas_by_id_` never erased a control's entry on destruction, so `set_theme()` could iterate a dangling pointer
- fix(cache): bound or prune six unbounded caches found in a memory-usage audit (Rust `sdk_media_fetched`, `media_backoff`/`room_summary_backoff` SQLite tables, five `ShellBase` maps, per-platform animated-image frame decode, Windows `emoji_bitmap_cache_`)
- fix(tk): harden `clear_children()`/`remove_child()` against reentrant/self-destroying widget callbacks — `weak_ptr` tracking on `Host`'s tracked widget references plus deferred subtree destruction via a new `RootWidget` class
- fix(windows): update vendored BetterText, fixing the password field's masking dot showing the wrong glyph
- feat(login): add legacy username/password login (`m.login.password`) for self-hosted homeservers without an OIDC/MAS provider, behind a new `TESSERACT_ENABLE_LEGACY_LOGIN` build flag (default `ON`)
- feat(ui): dispatch file drop and drag-hover through the widget tree instead of a flat per-Surface callback, so each drop target (compose bar, room/pack editors) claims its own drop and paints its own localized hover highlight; fixes native text fields on Qt6/macOS/GTK4 swallowing file drags before the Surface ever saw them
- fix(login): hide the homeserver field's drawn border once the password form is showing
- feat(compose): show the full slash-command list on no match instead of hiding the popup, and add scrolling so `/gif`/`/selfie` are reachable past the old 8-row cap
- Update vendored BetterText from upstream (adds per-range text styling used by the emoji-sizing change below, plus document-model fixes)
- feat(emoji): render bigger emoji (matching message-row sizing) in the composer, live as-you-type, and in the room list's last-message preview
- fix(room-header): bound topic-link clicks to the topic's own rect, so a click on the room name or avatar can no longer resolve to the topic's hyperlink
- feat(tk): let multiple `FormLayout`s share one label-column width via a new `FormLayoutGroup`, fixing misaligned combo boxes across the Room Settings → Permissions tab's four groups
- fix(macos): stop the composer's inline-emoji resize from corrupting glyph layout — a just-typed emoji could end up with a zero advance width until a later edit forced a relayout
- fix(macos): keep room-list preview text anchored to a fixed baseline when its emoji renders bigger, instead of the whole line drifting down

### Details

#### 2026-07-14

- fix(win32): `Host::areas_by_id_` registered every `BetterTextField`/
  `BetterTextArea` on creation but never removed the entry on destruction —
  a repeatedly opened/closed transient control (search bar, quick
  switcher, ...) left a dangling pointer behind, and `Host::set_theme()`
  iterates the whole map on every theme change, so the first light/dark
  toggle after any such control closed was a use-after-free.
  `Win32TextAreaBase` now takes an `on_destroyed` callback wired by
  `make_text_field()`/`make_text_area()` right after registration, with the
  erased id captured by value at that point rather than re-derived via the
  virtual `ctrl_id()` from inside the destructor (calling a virtual after
  the most-derived part of the object has already unwound is undefined
  behavior). Also reordered `Host`'s private members so the id-registry
  maps outlive `root_`'s own teardown during `~Host()` — with the old
  declaration order, `areas_by_id_` would have been destroyed before
  `root_`'s subtree teardown fired the new erase callbacks, introducing a
  second use-after-destruction bug. `fields_by_id_` and the
  `Win32NativeTextField`/`Win32NativeTextArea`/`Win32RichEditArea` classes
  it was meant for are dead code (never instantiated anywhere) and were
  left untouched. Windows-only; unverified in this environment (no MSVC
  toolchain, and the MinGW cross-compile preset fails earlier, in
  `third_party/bettertext`'s DirectWrite headers, unrelated to this
  change) — pending an on-platform build/test.
- fix(cache): a memory-usage audit surveyed every in-memory/on-disk cache's
  eviction policy and found six independent unbounded-growth cases, each
  fixed with the smallest mechanism proportionate to its actual risk: the
  Rust `sdk_media_fetched` "already fetched" hint (replaced the unbounded
  `HashSet<String>` with `MediaFetchedCache`, a FIFO-capped 4096-entry
  membership set — purely a soft-latency hint, so an evicted key just costs
  one slower gated re-fetch, never wrong behavior); the `media_backoff`/
  `room_summary_backoff` SQLite tables (gained the same 30-day TTL sweep
  `room_summary_cache` already had, via a new
  `prune_stale_backoff_and_cache_rows()` — neither table previously had any
  cap or expiry for a URL/room that's simply never retried again); five
  `ShellBase` maps (`url_previews_`, `url_preview_data_`,
  `url_preview_in_flight_`, `blurhash_attempted_`, `tile_fetch_failed_`)
  wired into all three existing per-account teardown checkpoints
  (`switch_active_account_impl_`, `logout_active_account_impl_`,
  `clear_all_caches_`) that every structurally similar map already uses —
  these five were simply never added; `last_sent_receipt_`, now erased
  alongside `pagination_` when a room ages out of the warm-subscription LRU
  in `prune_warm_subscriptions_()`; `reply_details_requested_`, capped
  (2000 entries, full clear on overflow, mirroring `voice_bytes_cache_`'s
  existing cap) since it's event_id-keyed and can't be room-pruned the same
  way; and animated-image frame decoding on Windows (D2D), macOS
  (CoreGraphics), and Qt6, which had no frame-count ceiling — capped at 200
  frames each, matching GTK's existing `canvas_cairo.cpp` decoder. Also
  capped Windows' `emoji_bitmap_cache_` (256 entries, full clear on
  overflow) — previously only cleared on D3D device-loss `rebind()`. 9 new
  unit tests (5 Rust `MediaFetchedCache`, 4 Rust backoff-sweep). Verified
  via the linux-debug preset (Qt6 + GTK4): full C++ suite (1171 tests) and
  Rust suite (383 tests) pass. The Windows (`canvas_d2d.cpp`) and macOS
  (`canvas_cg.cpp`) changes mirror the compiled/verified Qt6/GTK4 pattern
  exactly but could not be compiled on this machine.
- fix(tk): building on 5bf6137's notify-before-free fix, hardened
  `clear_children()`/`remove_child()` against two more subtle lifetime
  hazards. `Host`'s tracked widget references (`pressed_widget_`,
  `hovered_btn_`, `hovered_widget_`, `drag_hovered_widget_`, `popup_`,
  `pending_popup_`) are now `weak_ptr` instead of raw pointers, via a new
  `Widget::self_alive_` + `tk::track()` helper — a stale reference is safe
  to detect via `.lock()` no matter when the underlying widget is actually
  destroyed, making the old manual `on_subtree_removing()` nulling
  unnecessary (deleted). Actual destruction of a removed subtree is now
  deferred to the next event-loop turn via a new `RootWidget` class (the
  literal top of every surface's tree, inserted by `Host::set_root()`,
  which knows its owning `Host` and calls `Host::queue_for_deletion()`
  directly) — this lets a widget's own callback (e.g. a `CheckButton::
  on_change` that rebuilds its own parent) safely run to completion
  without freeing its own `std::function` mid-invocation. macOS's
  `subtree_removed_during_dispatch_` workaround, which existed for exactly
  this reentrancy hazard, is now dead code and was removed. Verified on
  Qt6/GTK4 (full test suite, plus 2 new regression tests covering deferred
  destruction and self-removal from within a callback); Win32/macOS mirror
  the same pattern but weren't build-verified in this environment.
- fix(login): `LoginView::paint()` only null-checked `hs_field_lbl_` before
  drawing the homeserver field's rounded-rect background/border, unlike the
  username/password blocks right below it, which correctly gate on
  `->visible()` too — so switching from the OAuth form to the password form
  hid the native homeserver field but kept painting its border every frame.
  Added the missing `visible()` check.
- feat(compose): the slash-command popup hid itself entirely once the typed
  prefix matched nothing, and separately hard-capped suggestions at 8 rows,
  silently dropping `/gif` and `/selfie` from the unfiltered list whenever
  more than 8 commands existed. Decoupled the engine's filtering cap from
  the popup's display cap: `ListPopupBase` (shared by the Mention,
  Shortcode, and SlashCommand popups) now scrolls its row list via
  `tk::ScrollableBase` instead of just clamping to a fixed visible-row
  count, and `SlashCommandController` falls back to the full command list
  instead of hiding when nothing matches the typed prefix. New
  `test_tk_slash_command_popup.cpp` covers the >8-row scroll, wheel
  scrolling, and out-of-viewport `set_selected_index`/pointer-accept paths.
- Pulled in upstream BetterText changes: a public per-range
  `BetterTextSetTextStyle` API (used by the composer emoji-sizing change
  below to size just the emoji runs within a text control), plus several
  document-model fixes and expanded document-model tests.
- feat(emoji): extends the existing message-row inline-emoji treatment to
  the two places that still rendered emoji at plain body size: the
  composer bar, live as-you-type on all four platforms, and the room
  list's last-message preview. Composer sizing is backend-specific — GTK
  via a `GtkTextTag`, Qt via `QTextCharFormat` under `QSignalBlocker`,
  macOS via `NSTextStorageDelegate`, Win32 via BetterText's new per-range
  `BetterTextSetTextStyle` — driven by a shared `segment_emoji_runs()`
  helper extracted from `MessageListView` into `html_spans`. Fixed two
  bugs found while wiring this up: Qt's `replace_range()` (used by the
  shortcode popup to insert a picked emoji) ran signal-blocked and so never
  triggered the resize pass, and both Qt and Win32 measured the composer's
  height before resizing its emoji, causing the composer to visibly jump
  one keystroke late. Switching the room-list preview from `build_text` to
  `build_rich_text` (needed for per-run sizing) exposed that
  `build_rich_text` never implemented single-line ellipsis truncation on
  Qt or GTK (unlike `build_text`), nor hard-break folding on any backend —
  long or multi-line previews would have overflowed the row instead of
  truncating. Added the missing truncation to Qt's `build_rich_text`, the
  missing `pango_layout_set_ellipsize` call to GTK's, and hard-break
  folding in `RoomListView`; macOS/Windows already truncated native
  rich text correctly. New `test_html_spans.cpp` and
  `test_room_list_preview_emoji.cpp`.
- fix(room-header): `on_pointer_up` called `topic_layout_->link_at()` for
  any click landing anywhere in the header without first checking it fell
  inside `topic_rect_`. Qt's `FuzzyHit` (and macOS's unbounded
  nearest-line search) resolve to the closest character regardless of
  distance, so clicking the room name or avatar could still open the
  topic's link. Added the same `rect_contains` guard already used by
  `RoomInfoPanel`/`JoinRoomView`.
- feat(tk): the Room Settings → Permissions tab has four independent
  `FormLayout`s, one per `SettingsGroup` box (Default Role, Messages,
  Membership, Advanced), each sizing its label column to only its own
  rows — so the combo boxes in different groups started at different
  horizontal offsets instead of lining up. New `FormLayoutGroup` lets any
  number of `FormLayout`s register with a shared owner and computes the
  label-column width as the max across every registered member instead of
  just its own rows; `FormLayout::set_label_group()` opts a form in (and
  safely unregisters on destruction/reassignment). `RoomPermissionsSection`
  now owns one `FormLayoutGroup` and passes it to all four of its forms.
- fix(macos): the composer emoji-sizing change (above) reapplied the
  `InlineEmoji` font size from an `NSTextStorageDelegate` callback, which
  mutated `NSTextStorage`'s permanent attributes while that same edit's own
  `processEditingForTextStorage` cycle was still unwinding — racing the
  edit's pending layout-manager notification and leaving the just-typed
  emoji glyph with a corrupted (zero) advance width, present in the buffer
  but invisible until a later edit forced a relayout. Moved the reformat
  onto `NSLayoutManager`'s temporary-attributes API (Apple's documented
  mechanism for overlaying a visual attribute onto text as it's edited),
  applied from `-textDidChange:` after the edit's layout has fully settled,
  removing the `NSTextStorageDelegate` and its recursion guard entirely.
- fix(macos): the room-list preview's switch to `build_rich_text` (above)
  exposed that macOS's `CTLayout` measured a single-line-elided layout's
  height from only the first character's font, ignoring any later, taller
  run — but the real CoreText draw call positions the shared baseline from
  the line's true typographic ascent, which *does* grow with an oversized
  emoji, so the reported metrics silently disagreed with what was actually
  drawn and the baseline (dragging the surrounding text with it) drifted
  down whenever a row's preview contained emoji. Fixed `CTLayout` to
  measure and expose the real per-content ascent for the single-line-elided
  path (building the truncated line eagerly instead of guessing from the
  first character's font); the full-box-height `ascent()` behavior used
  elsewhere (e.g. reaction chips) is untouched. `RoomListView.cpp` now
  anchors the preview baseline at the fixed spot a plain, emoji-free line
  would occupy — using a cached reference layout of the same style/trim —
  instead of centering by the emoji-inflated height, so regular text no
  longer moves and an oversized emoji grows from that fixed baseline,
  clipped to the row's bottom half if needed.

#### 2026-07-13

- fix(windows): pulled a BetterText upstream fix where the password
  field's masking character was written as a literal `L'•'` wide-char
  literal in `ComposedDisplayText` (both the plain-text and IME-composition
  masking paths) — depending on the source file's encoding as seen by the
  compiler, that literal doesn't reliably resolve to U+2022 BULLET, so the
  password field could render the wrong glyph instead of dots. Replaced
  with the explicit `L'•'` escape in both places.
- feat(login): adds `m.login.password` as a fallback login path for
  self-hosted homeservers without an OIDC/MAS provider, gated behind a new
  build-time `TESSERACT_ENABLE_LEGACY_LOGIN` flag (default `ON`, modeled on
  `TESSERACT_ENABLE_CALLS`). A new shared `build_configured_client()`
  de-duplicates OAuth/native client setup; session storage becomes a tagged
  `SessionEnvelope{OAuth, Native}` so `restore_session`/`export_session`/
  `logout` all branch on one JSON shape, with a hand-written deserializer so
  pre-existing (untagged) `session.json` files from before this change
  still restore correctly as legacy OAuth sessions. New `password_login`
  module + `legacy_login_ffi` shim, plus a homeserver capability probe
  (`GET /_matrix/client/v3/login`) feeding `LoginView`'s auto-detect.
  `Client::login_password` + `DiscoveryResult::supports_password` on the
  C++ side, gated behind `TESSERACT_LEGACY_LOGIN_ENABLED`. `LoginView`
  gains a two-step flow: a "Sign in with password" button appears under
  the OAuth button once the homeserver is confirmed to support it,
  switching to a dedicated username/password screen with a Back link,
  reusing the existing `arm_pending_login_`/`finalize_login_` completion
  path. ROADMAP.md item closed out. GTK4/Qt6 build-verified (1163 ctest
  passing, both `TESSERACT_ENABLE_LEGACY_LOGIN=ON` and a from-scratch
  `=OFF` configuration); Windows/macOS builds, and a real end-to-end test
  against a self-hosted Synapse with no OIDC/MAS configured — including
  the refresh-token behavior on a server without MSC2918 support — have
  since been verified on all platforms.
- feat(ui): replaced the flat per-Surface `FileDropHandler` callback with
  `tk::Widget` virtuals (`on_file_drop`/`dispatch_file_drop`) mirroring the
  existing pointer-event dispatch shape, so `RoomView`,
  `ImagePackEditorView`, and `UserPackEditor` each claim drops on their own
  instead of shells manually checking hand-rolled rect accessors. Replaced
  the old whole-surface "Drop to attach" overlay with the same claim-based
  shape for hover feedback (`on_drag_hover`/`dispatch_drag_hover`), so each
  drop target paints its own localized highlight (compose bar, the specific
  hovered pack section, the personal pack grid) instead of one overlay
  covering the entire window regardless of what would actually accept the
  drop. Also fixes the native compose text field swallowing file drags
  before the Surface ever saw them (inserting the dropped file's path as
  text instead of attaching it) on Qt6 and macOS, and on GTK4 by moving the
  drop target from the drawing area to the shared `GtkOverlay` ancestor in
  the capture phase. Windows needed no equivalent fix: BetterText's edit
  control is an unregistered child HWND, and Win32's OLE drag-drop already
  walks up to the nearest registered ancestor (the Surface) by contract.
  New `tests/cpp/test_tk_host_file_drop.cpp` plus coverage added to the
  image-pack-editor/room-settings/room-view/settings-view test suites.
  Verified on Linux (GTK4 + Qt6, full test suite) and confirmed working
  on-platform on all four platforms (compose bar, room editor, personal
  pack editor, drag-hover highlight).

## v0.8.14 — 2026-07-12

### Summary

- fix(views): wire drag-drop into the personal image pack editor in Settings, which never had `set_on_file_drop` wired on its own native surface
- fix(views): drop the unimplemented "paste to add" hint from the image pack tile placeholder
- fix(ui): stop the sticker right-click save menu from leaking through room overlays (settings/info/profile panels) via stale hit-test geometry
- feat(views): show `:shortcode:` tooltips for inline custom emoji hovered in the timeline, matching the emoji/sticker picker grids
- fix(windows): stop the emoji/sticker picker's shortcode tooltip from freezing every animation in the app while visible — `Host::show_tooltip`'s same-owner refresh now only requests a repaint when the text/anchor actually changed, instead of unconditionally on every call
- fix(macos): implement `CTRunDelegate`-based inline-image box reservation in the CoreText canvas backend, fixing custom MSC2545 emoji not rendering inline in the timeline at all on macOS (composer/pickers were unaffected — they don't use this code path)
- feat(image-packs): surface MSC2545 packs from any Space (direct or nested-ancestor) the current room belongs to, in the emoji/sticker pickers and the shortcode popup, alongside the existing personal/current-room/subscribed-room scopes
- refactor(tk): replace the hand-maintained per-shell native-field theming list with a generic `tk::Widget::apply_theme()` tree traversal, fixing several fields the old list missed entirely (Qt6's `SettingsWidget`/`JoinRoomDialog`/pop-out `RoomWindow`, and macOS's join-room dialog surface never re-theming past its initial light-mode construction)
- feat(tk): add a generic `tk::Host`-owned tooltip system (dwell-delay, popup-suppressed, custom-drawn above the whole widget tree) and migrate all 8 hand-rolled hover/tooltip sites (RoomHeader, RoomInfoPanel, ComposeBar, MessageListView action pills, LocationMapPanner, AboutSection cache rows, AdvancedSection, TabbedGridPicker) onto it, deleting the old per-platform native tooltip code (Win32 `TOOLTIPS_CLASS`, Qt `QToolTip`, and the macOS/GTK `NSPopover`/`GtkPopover` popovers that were only styled to look like native tooltips)
- fix(theme): sync every Qt6 native text field's color on theme change instead of just 2 of 13, fixing black-on-dark text in the quick switcher and other search/edit fields
- fix(forward-picker): wire `ForwardRoomPicker::on_close` on all four shells so Escape/outside-click actually resets the native search field
- fix(media-viewer): stop a video-lightbox pagination leak (missing wheel-swallow) and fix gallery pagination backpressure/race/shutdown-abort
- fix(build): exclude CG test surface files from the unity build to fix `macos-appkit-x86_64-release`
- feat(windows): vendored BetterText (`third_party/bettertext`), a from-scratch D2D/DirectWrite Win32 text control
- feat(image-viewer): copy the displayed image to the clipboard from the full-window lightbox, beside the save button
- feat(composer): render custom MSC2545 emoji inline, end to end
- feat(room-media-view): add a room media gallery — grouped by month oldest to newest, older months paginate in on scroll-up, opened from a new "Media (N)" row in the room info panel
- feat(popout): close 13 feature-parity gaps between the main window and pop-out room windows (file save, jump-to-original, pin/unpin, edit-last-message, retry/abort send, inline video/GIF autoplay, forward-message picker, room media gallery, visible-row media priority, open-DM-in-new-window, confirm dialogs, macOS composer popups, Windows image resolver), plus hiding native overlays under an open confirm dialog and routing incoming-call banners/calls to whichever window has the room open
- perf(timeline): a just-sent message's local echo could take seconds to appear in the timeline under background load
- perf(timeline): instant room switch — stop auto-filling a short viewport, bottom-anchor short content, keep warm favorites' subscriptions alive
- perf(timeline): fill a full window from cache on room open instead of the smaller scroll-increment batch
- build: enable CMake unity builds (`TESSERACT_UNITY_BUILD`, default ON)
- perf(compose): first (superseded) attempt at the slow local echo
- build: merged the four `linux-{gtk,qt6}-{debug,release}` CMake presets into `linux-debug`/`linux-release`
- build: moved the per-platform `tk::` backend implementations out of the cross-platform `tesseract_tk` library into each platform's own target
- docs(roadmap): rewrote ROADMAP.md as a single priority-ordered backlog (tiers by urgency)
- docs: call out the Matrix Authentication Service + Sliding Sync server requirement in the README and landing page
- test(message-list): fix 18 tests broken by the new bottom-anchored short-content layout
- fix(windows): route BetterText emoji glyphs through Tesseract's bundled Noto Color Emoji font
- fix: multiline messages weren't rendering as multiline
- fix: thread replies dropped `formatted_body`, silently losing MSC2545 custom emoji and @mention `matrix.to` links
- fix: close the reaction/emoji picker on an outside click, and keep a message row's action buttons visible while the picker is open
- fix: don't drop the room-switch anti-reflow gate on a same-room timeline reset (e.g. a pagination refill)
- fix(timeline): decrypt old events on jump-to-date in encrypted rooms
- fix(theme): stop pre-auth status labels (login/join-room/QR-grant) from baking in literal colors instead of reading the theme palette
- fix(macos): drop a redundant `NSCell` image-property redeclaration on `TKEmoticonCell`
- fix(build): wire up Windows test-target linking (UNICODE, D2D/D3D11/DWrite, `screen_capture_win32.cpp`, video capture)
- fix(cpack): declare the `qt6-image-formats-plugins` runtime dependency for the .deb/.rpm packages
- fix(ci): bundle Qt6 image-format plugins (WebP/TIFF/ICO, etc.) in AppImage builds
- fix(room-media-view): widen the month-key buffer to silence a release-build format-truncation warning
- fix(linux): correctly detect OS dark/light mode via the XDG desktop-settings portal on Qt6 and GTK4
- fix(macos): link CoreMedia/Foundation for the tests target's `video_capture_macos.mm`
- fix(windows): render composer mention pills as real inline chips instead of plain `@Name` text
- fix(room-media-view): cancel the in-flight backward-pagination task when the gallery closes
- fix(sync): include pinned events in the room-list change fingerprint so pin/unpin always reaches the UI
- fix(image-packs): load MSC2545 image packs by both their stable and unstable event names and combine the results, instead of stopping at the first one found
- docs(readme): credit BetterText in the acknowledgements
- fix(windows): restore clipboard image paste in the BetterText composer, which swallowed Ctrl+V internally before it could reach the existing image-paste check
- feat(image-packs): redesign the Emojis & Stickers pack editor into a multi-pack list with position-aware drag-drop, replacing the old single-pack-at-a-time combobox
- feat(space-settings): extend room settings (and the Emojis & Stickers tab) to space roots
- fix(image-packs): render animated stickers/emoji correctly in pack editor tiles instead of leaving them blank
- feat(image-packs): editable pack name inline in the Emojis & Stickers tab
- feat(image-packs): commit pack edits through `RoomSettingsView`'s shared Accept/Cancel footer instead of a standalone one
- feat(image-packs): gate pack creation/editing/removal behind the room's actual power levels
- feat(image-packs): add a global "Emojis & Stickers" settings tab for the personal pack and cross-account pack subscriptions
- fix(image-packs): scope emoji/sticker pickers and pack discovery to the personal pack, current room, and explicitly subscribed rooms, and fix per-room pack discovery under sliding sync
- feat(image-packs): persist pack edits back to the server for both the personal pack and room/space packs, closing the last stub in the editor
- fix(edits): stop showing edited plain-text messages as a bare `*` — a `ruma-events` fallback quirk stamped a synthetic HTML body on edit events that a raw-JSON re-read picked up instead of the real `m.new_content`

### Details

#### 2026-07-12

- fix(views): the global Settings window is hosted on its own native
  surface per shell, separate from the main app surface, and only the main
  surface ever had `set_on_file_drop` wired up — dropping an image onto the
  personal pack editor in Settings never reached any handler (on Qt6, drops
  weren't even accepted at the OS level). Added
  `SettingsView::user_pack_list_rect()` / `add_user_pack_dropped_image()`,
  gated on the Emojis & Stickers tab actually being the one selected
  (mirrors `RoomSettingsView`), and wired each shell's Settings surface the
  same way the room/space pack editor already was. New tests in
  `test_tk_settings_view.cpp`.
- fix(windows): hovering a custom emoji/sticker tile long enough for its
  shortcode tooltip to appear in the Emoji/Sticker picker froze every
  visible animation (GIF/APNG emoji, animated stickers, everywhere in the
  app) for as long as the tooltip stayed on screen, resuming the instant it
  was dismissed. `TabbedGridPicker::paint()` (shared base of `EmojiPicker`/
  `StickerPicker`) calls `Host::show_tooltip()` unconditionally on every
  paint frame while a cell is hovered — it has no hover-transition event of
  its own, since `GridView` tracks `hovered_index_` itself. `Host::
  show_tooltip`'s same-owner refresh branch called `request_repaint()`
  unconditionally too, even when the text/anchor hadn't changed, so once
  the tooltip became visible this created a self-sustaining `paint() →
  show_tooltip() → request_repaint() → paint()` loop needing no pointer
  movement. Harmless on Qt6/GTK4/macOS, whose toolkits coalesce redundant
  invalidations, but fatal on Win32: `request_repaint()` is a raw
  `InvalidateRect` on the picker's own popup HWND, and `WM_PAINT` is
  regenerated/serviced ahead of `WM_TIMER` on the same thread's message
  queue — starving the single app-wide `SetTimer` that drives every
  animation's frame advance. Fixed by making `Host::show_tooltip`'s
  same-owner refresh idempotent: only repaint when the text or anchor
  actually changed. New regression test in `test_tk_tooltip.cpp`; 1147 C++
  tests passing. Windows-specific freeze, so the fix is unverified live in
  this environment — needs a build/manual check on Windows.
- fix(macos): custom emoji (MSC2545 image-pack emoticons) never rendered
  inline in the timeline on macOS, though they worked fine in the composer
  and pickers. `MessageListView::substitute_image_placeholders` stuffs a
  U+FFFC object-replacement character into each `is_image` `TextSpan`,
  expecting each backend's `build_rich_text` to reserve a fixed-size box for
  it via its own native inline-object mechanism (`IDWriteInlineObject` on
  Windows, `PangoAttrShape` on GTK4, `QTextObjectInterface` on Qt6) so
  `paint_span_images`'s `selection_rects()` lookup has a real box to paint
  the decoded bitmap into. `ui/macos/tk/canvas_cg.cpp`'s CoreText backend
  never got the same treatment — the U+FFFC code unit fell through as
  ordinary text with no reserved box, no fallback glyph guarantee. Added a
  `CTRunDelegate` (fixed `InlineEmoji`-sized ascent/width, zero descent, no
  draw callback) attached to each `is_image` span's range, mirroring the
  other three backends. The composer and Emoji/Sticker pickers were
  unaffected since neither goes through this shared `TextLayout` path — the
  composer's typed emoji use a native `NSTextAttachment`, and the pickers
  draw `tk::Image` directly into grid cells. Should also fix the shortcode
  hover tooltip on macOS as a side effect, since `emoji_at_world` relies on
  the same `selection_rects()` box. macOS-only change; unverified in this
  environment (no Xcode toolchain here) — pending a build/manual check on
  macOS.
- fix(views): the image pack tile placeholder's empty-state hint mentioned
  pasting an image to add one, but paste-to-add isn't wired up for that
  placeholder yet — dropped the misleading hint text.
- fix(ui): the main-window sticker-save right-click shortcut queries
  `MessageListView::sticker_hit_at()` directly against raw screen
  coordinates on all four platforms, bypassing `RoomView`'s already-correct
  `dispatch_right_click` overlay gating. `sticker_geom_` isn't cleared when
  the timeline stops painting, so a right-click over
  `RoomSettingsView`/`RoomInfoPanel`/`UserProfilePanel` could still hit
  stale sticker geometry from the room content underneath and pop the
  save-sticker menu through the overlay. Guarded each platform's handler
  with `RoomView::is_overlay_open()`.
- feat(views): hovering an MSC2545 `<img data-mx-emoticon>` span in the
  message timeline now shows its `:shortcode:` via the existing `Host`
  tooltip mechanism, matching the emoji/sticker picker grids. The shortcode
  was already carried on `TextSpan::image_alt`; a new section-aware
  `emoji_at_world()` hit-test mirrors `paint_span_images`' box walk to find
  the hovered image under the pointer.
- refactor(tk): the fix below (2026-07-12, "the Ctrl+K quick switcher's
  search field...") patched the symptom — a per-field `set_text_color`
  if-chain in `MainWindow::apply_theme_ui_()` — without changing the
  mechanism, so every future native field would still need someone to
  remember to add it to that same list. `tk::Widget` gained a virtual
  `on_theme_changed(const Theme&)` hook (default no-op) and a non-virtual
  `apply_theme(const Theme&)` that recurses into every child, including
  hidden ones — unlike `paint`/`paint_overlay`, a hidden native field still
  needs correct colors queued for when it next becomes visible. The 13
  shared views that own a shell-supplied native field (`ComposeBar`,
  `RoomInfoPanel`, `RoomSearchBar`, `RoomListView`, `QuickSwitcher`,
  `MessageSearchView`, `ForwardRoomPicker`, `EncryptionSetupOverlay`,
  `QRGrantView`, `RoomSettingsView`, `ImagePackEditorView`, `SettingsView`,
  `JoinRoomView`) push their own field's color from `on_theme_changed()`;
  every shell's `apply_theme_ui_()` and each pop-out `RoomWindow`/
  `CallWindow::apply_theme()` collapses to one
  `surface->root()->apply_theme(t)` call per owned surface. Native fields
  moved from shell-owned `unique_ptr` to `shared_ptr` — `Host::
  make_text_field()`/`make_text_area()` themselves are unchanged, since
  `shared_ptr` has a converting assignment from `unique_ptr&&` — so each
  owning view can hold a `weak_ptr` instead of a pointer that could dangle.
  Auditing every native-field call site for this refactor surfaced further
  gaps the old per-field list missed entirely: Qt6's `SettingsWidget`
  (account settings fields), `JoinRoomDialog` (the join-room alias field),
  and pop-out `RoomWindow` (its own compose text area) never pushed
  `set_text_color` at all; macOS's join-room dialog surface was permanently
  stuck on `tk::Theme::light()` from construction, never re-themed on any
  subsequent theme change. New test:
  `Widget::apply_theme visits every descendant, including hidden ones`.
  1141 C++ tests passing. Verified on Qt6/GTK4 (both build clean); Win32/
  macOS mirror the same pattern but weren't build-verified in this
  environment.
- fix(theme): the Ctrl+K quick switcher's search field (and several other
  Qt6 native fields) showed black text after switching to dark mode. Qt6's
  `QLineEdit`/`QTextEdit` hold an explicit `QPalette` that must be manually
  kept in sync with the app's `Theme` — unlike GTK4 (CSS-driven theming),
  macOS (`NSColor.labelColor`, a dynamic system color), or Win32 (a global
  `WM_CTLCOLOREDIT` message handler), none of which need an explicit sync.
  `MainWindow::apply_theme_ui_()` only re-applied `set_text_color()` to 2 of
  the shell's 13 native text fields on a theme change (`roomTextArea_`,
  `roomSearchField_`); every other field — `quickSwitchField_`,
  `messageSearchField_`, `forwardPickerField_`, `findInRoomField_`,
  `topicTextArea_`, `roomSettingsNameField_`, `roomSettingsTopicArea_`, the
  three image-pack fields, and the encryption/QR-grant fields (three of
  which had no color set at all) — kept whatever color it got at
  construction and never updated again. Fixed by refreshing all 13 fields
  in `apply_theme_ui_()` and giving the three encryption/QR fields an
  initial color. Qt6-only; the other three shells don't set an explicit
  text color anywhere and re-theme automatically.
- fix(forward-picker): auditing every native field for the theme bug above
  turned up a dangling `if (mainApp_ && mainApp_->forward_picker())` in the
  Qt6 shell with no body — clang-tidy's misleading-indentation warning
  caught it. The intended statement, wiring `forward_picker()->on_close`,
  was simply missing; `quick_switcher()` and `message_search()` both wire
  their `on_close` the same way, but `forward_picker()` never did on **any**
  of the four shells. Net effect: closing the forward-message picker via
  Escape or an outside click never reset `forwardPickerField_` (visibility/
  focus state). Fixed on all four shells, mirroring the existing
  `quick_switcher`/`message_search` pattern.
- fix(media-viewer): several related bugs surfaced while investigating why
  opening the room media viewer left background work running, delayed
  local echo, or could hang app shutdown. `VideoViewerOverlay` never
  swallowed wheel input while open (unlike `ImageViewerOverlay`), so
  scrolling over the fullscreen video lightbox fell through to the room
  timeline and silently drove backward pagination, contending with the send
  queue's durability write on matrix-sdk-sqlite's shared write connection.
  The `paginate_tasks` Rust task registry was the one registry never
  force-aborted in `stop_sync()`, unlike `sync_tasks`/`media_tasks`/etc., so
  an in-flight pagination task could block app shutdown. Neither the media
  lightbox nor active voice/audio playback was torn down on room switch,
  leaving them running against a room the user had left. `RoomMediaView`
  never opted into `autofill_only_when_empty`, so `ListView`'s "fill on
  open" autofill re-fired `on_near_top` on every relayout in media-sparse
  rooms, spamming pagination. The gallery's retry/accumulate loop judged
  progress from rendered row count, populated by a separate, much slower
  diff-streaming task than the pagination round itself, causing the loop to
  race ahead on stale state — added a dedicated FFI path
  (`paginate_media_view_back_async` / `on_media_view_paginate_result`) that
  reads an authoritative Image/Video count directly from the SDK timeline,
  decoupled from that race, and replaced a fixed magic-number retry target
  with `RoomMediaView::estimated_capacity()`. Added render-gap backpressure
  in `ShellBase` that defers further pagination rounds when the render
  queue falls more than 24 items behind, resuming once the renderer catches
  up. 5 new tests; 1120 C++ tests passing.
- fix(build): several `test_tk_*.cpp` files have a file-scope
  `using namespace tk;`, which leaks for the rest of a unity-batched
  translation unit. When the batcher placed one of those files ahead of
  `tk_test_surface_cg.cpp`/`canvas_cg.cpp`, the leaked using-directive made
  `tk::Point`/`tk::Rect` ambiguous with the global `Point`/`Rect` QuickDraw
  types pulled in transitively via `CoreGraphics.h`'s `MacTypes.h`, breaking
  `macos-appkit-x86_64-release`. Excludes both files from unity batching
  (mirrors the existing exclusion for `canvas_qpainter.cpp` on the Qt6
  branch).

#### 2026-07-11

- fix(edits): editing a plain `m.text` message (no formatting) rendered as a
  bare `*` instead of the corrected text. `ruma-events`'
  `make_replacement_body()` unconditionally synthesizes an empty
  `FormattedBody::html("")` and prepends `"* "` to it for `Text`/`Emote`/
  `Notice` edits, even when the edit has no HTML at all — so the outgoing
  edit event's top-level fallback fields become `format:
  org.matrix.custom.html`, `formatted_body: "* "`, even though the real
  text is preserved correctly and unprefixed in `m.new_content`.
  `resanitized_formatted_body` (`timeline_convert.rs`) re-reads the raw
  JSON of the *latest* event contributing to a timeline item — for an
  edited message that's the edit event itself — and was pulling
  `content.format`/`content.formatted_body` straight from the top level,
  picking up that bogus `"* "` fallback and overriding the already-correct
  resolved content computed upstream. Now checks whether the raw event is
  a replacement (`content["m.relates_to"]["rel_type"] == "m.replace"`) and,
  if so, reads the HTML fallback from `content["m.new_content"]` instead,
  matching how Element/Cinny handle the same spec fallback. Split the pure
  JSON-pointer logic out into `resanitized_formatted_body_from_json` so it
  can be unit-tested without constructing a real `EventTimelineItem`
  (nothing in this codebase mocks one); added 4 regression tests covering
  the plain-text, HTML-edit, non-edit, and no-formatted-new-content cases.
- feat(image-packs): both pack editors staged edits fully in memory but
  only partially (personal pack) or never (room/space packs) wrote them
  back to the server — the last stub left in the editor. Personal-pack
  saves now upload brand-new pasted/dropped images via the existing
  `upload_media` primitive before saving. Room/space packs get a save
  backend built from scratch: `Client::save_room_pack` wholesale-replaces
  a pack's images (matching the editor's full-snapshot staging model, not
  an incremental diff), assigns a fresh collision-free `state_key` for new
  packs, and writes back to whichever of the stable/unstable event types an
  existing pack already uses so no duplicate copy is introduced under a
  type the room didn't already have. `Client::remove_room_pack` empties a
  pack (Matrix has no true state-event delete); discovery now skips
  zero-image room packs so a removed pack actually disappears everywhere
  instead of resurfacing empty under the room's name. Also fixed, found
  while dogfooding: dropped images with no assigned shortcode collided
  (only the last of a multi-image drop survived — now de-duplicated via
  `suggest_shortcode`); dropping an image now suggests a shortcode from its
  filename, and a second drop while the first tile's shortcode field is
  still open correctly reseeds the field instead of showing stale text
  (reset a generation counter, not just a visibility rising edge); clicking
  outside a shortcode field now cancels the edit instead of committing
  stale text; and the shortcode popup's image lookups never checked
  `anim_cache_`, so animated custom emoji rendered blank and fell back to
  plain text on insert even though the same emoji worked fine in the emoji
  picker.
- fix(image-packs): sliding sync never delivers custom `m.room.image_pack`
  state, and packs can use non-empty `state_key`s that MSC2545 doesn't
  require to be empty — so prior discovery only checked one guessed key
  per room and missed rooms with multiple named packs entirely. Replaced
  with a shared per-room full-state fetch (`RoomStateCache`), triggered
  promptly on room switch instead of waiting for an unrelated sync tick,
  and persisted into a lazily-built `room_image_pack_cache` so the Known
  Packs settings page is a fast local read regardless of account size.
  Also scopes what's actually *shown*: emoji/sticker picker tabs and the
  inline `:shortcode:` popup now filter to the personal pack, the room
  currently open, and explicitly subscribed rooms — hiding packs from any
  other recently-visited room. Each pop-out window computes its own
  filtered shortcode list from its own room rather than sharing the main
  window's. Also fixed the global Emojis & Stickers settings tab never
  loading images on Qt6 (the image-provider wiring only ran through a path
  that no-oped before the settings widget was lazily constructed on first
  open).
- feat(image-packs): new `SettingsView` tab (`ImagePacksSection`) for
  managing the personal sticker pack and subscribing to other rooms' image
  packs account-wide: `UserPackEditor` (single-pack tile-grid editor for
  the personal `im.ponies.user_emotes` pack — add/remove/rename images,
  staged until Save) and `KnownPacksList` (checkbox list of every known
  room pack, toggling `m.image_pack.rooms`/`im.ponies.emote_rooms`
  subscription immediately). New Rust/FFI writes back this: remove/rename
  a user-pack image, and subscribe/unsubscribe a room pack (dual-writes
  stable + unstable event types, forces a synchronous aggregator rebuild
  so `ImagePack::is_subscribed` is correct before the call returns).
  `SettingsController`/`ShellBase` wiring across all four shells. Also
  extracted `ImagePackTileGridBase`, a shared base for the tile-grid
  layout/paint/hit-test logic `UserPackEditor` and the room-pack editor's
  `ImagePackSectionList` had duplicated (dropping `UserPackEditor`'s
  one-off favorite-star affordance so the shared paint routine has no
  per-caller divergence); fixed animated stickers in the new tab only
  advancing while the mouse moved (its view lives on its own top-level
  surface that no shell's per-tick animation repaint hook knew about);
  and fixed a latent test-teardown bug the refactor's changed object
  layout surfaced (several `ImagePackEditorView` tests declared the
  widget-under-test before the `TestSurface` supplying its images, so the
  D2D-backed surface was torn down before the widget holding images
  created from it — reordered so the surface fixture outlives the widget).
- feat(image-packs): gate pack editing behind actual room permissions —
  any user could previously create/edit/remove packs regardless of power
  level. Added `Client::can_set_room_image_packs` (mirrors the existing
  `can_set_room_name` family: checks power level against both the stable
  `m.room.image_pack` and unstable `im.ponies.room_emotes` state event
  types) and threaded it through `ImagePackEditorView::set_field_permissions()`,
  which now gates Create, remove chips, the usage toggle, rename/shortcode
  editing, and paste/drop, while still allowing header-click-to-select-
  active since that doesn't mutate anything. Wired once in
  `ShellBase::seed_image_pack_tab_`, covering all four shells; also fixed
  pop-out room windows never seeding the tab's pack data at all.
- feat(image-packs): fold the Emojis & Stickers tab into
  `RoomSettingsView`'s shared Accept/Cancel footer instead of its own
  independent one (which hid the shared footer whenever the tab was
  selected), now that the editor's permanent home is room/space settings.
  `ImagePackEditorView` loses its own buttons in favor of
  `set_committing()`/`build_result()`, and its staged snapshot is
  delivered via a new `RoomSettingsChanges::image_packs` field (optional,
  populated only when `ImagePackEditorView::has_changes()`), matching the
  "only report what changed" contract every other settings field already
  follows.
- feat(image-packs): clicking a pack's name header now turns it into an
  inline rename field, mirroring the existing per-tile shortcode-editing
  pattern. Also fixed a bug uncovered while wiring it: the image-pack
  native fields (and the General tab's name/topic fields) were hardcoded
  to resolve against `room_view_`'s `RoomSettingsView` instance only, so
  they silently never worked when a space's settings tab was open instead
  — each shell now resolves via a small helper
  (`active_room_settings_view_()` / `activeRoomSettingsView_()` /
  `_activeRoomSettingsView`) that picks whichever instance, room or
  space-root, is actually open.
- fix(image-packs): `make_static_image_provider_with_fetch_` only ever
  checked `image_cache_`, so an animated WebP/GIF pack image's frames sat
  decoded in `anim_cache_` but the provider kept returning `nullptr` for
  it, leaving the tile blank forever. Now checks
  `anim_cache_.current_frame()` first, mirroring
  `make_picker_image_provider_`'s pattern already used by the emoji/
  sticker pickers. Shared helper, so this also fixed the Windows compose
  box's inline custom-emoji preview for animated custom emoji.
- feat(space-settings): image packs (MSC2545) are ordinary room state, so
  a space can host its own the same way any room can — no reason to hide
  the Emojis & Stickers tab there like Media (which has no meaning for a
  space, since it governs timeline image/video preview rendering).
  Generalized `ShellBase::seed_image_pack_tab_`/
  `handle_image_pack_images_needed_`/
  `handle_image_pack_pending_image_added_` to take an explicit
  `RoomSettingsView*` target (mirroring an earlier fix to
  `stage_room_settings_avatar_upload_`), then wired the full image-pack
  flow onto `SpaceRootView::settings_view()` in all four shells.
- feat(space-settings): added a wrench icon to `SpaceRootView` (top-left,
  mirroring `RoomInfoPanel`'s) that opens `RoomSettingsView` for the
  space, hiding everything with no bearing on a space: the Media tab, the
  encryption toggle, and (until its provider was wired for spaces two
  commits later) the Emojis & Stickers tab. `SpaceRootView` owns its own
  `RoomSettingsView` instance rather than routing through `RoomView`'s,
  avoiding any `MainAppWidget`-level view-swap bookkeeping. `tk::SideTabView`
  gained `set_tab_visible()` to hide a tab dynamically, and
  `RoomSecuritySection` gained `set_encryption_field_visible()` to hide
  just the encryption row.
- feat(image-packs): redesigned `ImagePackEditorView` from a single-pack-
  at-a-time combobox into a scrollable list of every room pack at once,
  each with its own header (name, usage toggle, remove chip) and image
  grid (hover remove chip, click-to-edit shortcode). Clicking a header
  selects it as the active pack for paste and as the drop fallback; a
  fixed create-row adds new empty packs. True position-based drop
  targeting required extending `tk::FileDropHandler` with a `tk::Point`,
  threaded through all four platform backends (Qt6, GTK4, Win32, macOS)
  with their own coordinate-space conversions, plus native-overlay wiring
  for Win32/macOS which never had it before. Also fixed a Qt6
  text-rendering bug where an unset `max_width` was treated as an
  internal 8192px sentinel, mispositioning centered labels. Rewrote all
  `ImagePackSectionList`/`ImagePackEditorView` tests for the multi-pack
  model. Qt6 is build-verified end to end; GTK4/Win32/macOS are verified
  by static reading only (no toolchain available in this environment).
- fix(windows): `BetterTextArea` handles Ctrl+V internally as a text-only
  paste and never lets `WM_PASTE` reach the host subclass proc, so the
  existing `WM_PASTE`-based image-paste check was dead code for keyboard
  paste in the new composer. Intercepts at `WM_KEYDOWN` instead, mirroring
  the fix already applied to the old windowless RichEdit backend.

#### 2026-07-10

- feat(popout): close 13 feature-parity gaps between the main window and
  pop-out room windows, found by auditing every `RoomView` callback for
  wiring that existed on one side but not the other. Refactored
  `PopoutRoomWidget` onto `tk::Stack` first (its hand-rolled measure/arrange/
  paint were fully redundant with `Stack`'s defaults), which made adding new
  overlay members trivial, then closed: file-attachment save dialogs;
  jump-to-original-message; pin/unpin plus the pinned-messages banner;
  Up-arrow-to-edit-last-message (Windows/macOS were missing it); retry/abort
  of a failed send (previously only worked *from* a pop-out, not into the
  main window); inline autoplay video/GIF in the timeline; the
  forward-message picker (new `ForwardRoomPicker` overlay, per-platform
  native search field); the room media gallery (new `RoomMediaView`
  overlay); visible-row media/avatar lazy-fetch prioritization;
  open-DM-in-a-new-window; a new self-contained `ConfirmDialog` overlay; and
  macOS pop-out shortcode/slash/GIF composer popups (porting the
  Controller+NSPanel pattern already used for @mention). Most of these are
  single shared-code edits in `RoomWindowBase`/`ShellBase` that fix all four
  platforms at once. Finally, while a `ConfirmDialog` is open in a pop-out,
  the native compose text area and search fields are hidden so they don't
  paint over the modal backdrop, and `PopoutRoomWidget::on_layout_changed`
  (added earlier but never connected on any platform) is now wired to
  trigger a relayout — fixing a latent bug where opening/closing the confirm
  dialog never re-laid-out the pop-out window. Incoming-call banners (and
  their dismissal) now resolve to whichever window — main or pop-out — is
  currently displaying the room, via `ShellBase::room_view_for_room_()`, and
  pop-outs wire `on_start_call` the same way the main window does
  (`call_session_` is a process-wide singleton, so this is safe everywhere).
  Verified via `tesseract_win32`/`tesseract_tests` builds after each batch,
  with the pre-existing 22 ctest failures (2 segfaults + 20 fails —
  QuickSwitcher, i18n, encryption-setup overlays) confirmed unrelated via a
  stash/rebuild/retest against the unmodified baseline. Qt6/GTK4/macOS
  changes mirror the Windows pattern closely but could only be
  compile-verified on Windows in the environment this landed from.
- fix(image-packs): MSC2545 defines a stable event type for the per-room
  pack (`m.room.image_pack`) and the enabled-rooms pointer
  (`m.image_pack.rooms`), each with a legacy `im.ponies.*` unstable
  equivalent. `rebuild_image_packs` previously probed the unstable name
  first and **stopped at the first hit**, so a room or account that had
  only written the stable name — or had partially migrated, with images
  under one name the other lacked — silently lost those images. Added
  `merge_pack_contents()` (unions `images` by shortcode, unstable wins a
  collision; fills pack-level metadata from whichever side has it) and used
  it at all four read sites: the rooms-pointer account data, the local
  state-store fast path, the HTTP fallback, and the implicit
  all-joined-rooms enumeration. No FFI or C++ changes — every consumer only
  ever sees the final merged `Vec<ImagePack>`.
- fix(sync): `pin_event`/`unpin_event` only send the `m.room.pinned_events`
  state event and return; the sync loop picks up the change but only
  forwards a fresh room snapshot to C++ when `room_list_fingerprint()`
  differs from the previous emit, and `pinned_events` wasn't one of the
  tracked fields. A pin/unpin that didn't also touch unread/name/avatar/
  recency etc. left the fingerprint unchanged, so the pinned-messages
  banner and the message action's Pin/Unpin label stayed stale until some
  unrelated change (or an app restart) happened to flush it. Added a
  joined-event-id field to the fingerprint key so pin/unpin always perturbs
  it; also fixed the cargo-test-only pure-Rust `ffi::RoomInfo` stub in
  `lib.rs`, which had drifted out of sync and was missing
  `pinned_events`/`PinnedEvent`, blocking the new regression test from
  compiling.
- fix(room-media-view): closing the gallery only reset client-side
  bookkeeping and never cancelled the in-flight `paginate_back_async` tokio
  task, so it kept running after close; a stale result on a same-room
  reopen could also be misattributed to the new session since correlation
  was by `room_id` alone. Added a real `cancel_paginate_back` primitive
  (mirroring `cancel_media_group`'s `AbortHandle` registry) through the Rust
  SDK, cxx bridge, and `Client` wrapper, and `close_room_media_view_()` now
  actually cancels the gallery's own pending request instead of abandoning
  it.
- fix(windows): `BetterTextArea::insert_mention` only ever inserted plain
  `"@Name "` text and `set_mention_colors()` was a no-op, so Windows never
  drew a mention chip while Qt6/GTK4/macOS all do. Now renders the pill
  offscreen via a WIC-backed D2D render target plus the existing
  `tk::Canvas`/`CanvasFactory` abstraction, inserts it as a real BetterText
  image run (same mechanism as custom-emoji pills), and wires
  `set_mention_colors()` through from the shell on composer creation and
  theme change.
- fix(linux): the deprecated Qt6 `Settings.Read` D-Bus method double-wraps
  its return value in an extra variant layer, which made
  `QVariant::toInt()` silently yield 0 instead of the real color-scheme
  value — the fallback path (used whenever Qt's own `colorScheme()` is
  unavailable, e.g. Qt < 6.5) could never report Dark. Switched to
  `ReadOne`, which returns a single, correctly-typed value. GTK4 now ports
  the same XDG desktop-settings portal query used by the Qt backend:
  `GtkSettings`' `gtk-application-prefer-dark-theme` is app-controlled, not
  desktop-driven (only libadwaita apps get automatic portal sync), and
  previously relied entirely on distro-specific bridges like
  `kde-gtk-config` plus a signal that never fires live on Wayland; the old
  `GtkSettings` read is kept only as a last-resort fallback.
- fix(macos): `video_capture_macos.mm` uses `CMSampleBufferGetImageBuffer`
  (CoreMedia) and `NSString`/`NSNumber` (Foundation), but the macOS branch
  of the `tests` target never linked either framework — only
  `ui/macos/CMakeLists.txt`'s app target did. Since `add_subdirectory(tests)`
  is unconditional, every default macOS build (including CI) failed to link
  `tesseract_tests`.
- fix(room-media-view): widened the month-key buffer (8 → 32 bytes) used by
  `compute_month_key`; the previous size was tight enough that some
  release-build toolchains flagged the `snprintf` call with
  `-Wformat-truncation` even though it never actually truncated.
- docs(readme): credit BetterText in the acknowledgements.
- feat(room-media-view): add a room media gallery listing every image/video
  in the current room, grouped by month (oldest month at the top, newest at
  the bottom, mirroring the chat timeline), with older months paginating in
  as the user scrolls up. Opened from a new "Media (N)" row in the room info
  panel, under the notification-mode selector — `N` is the count of
  image/video rows already synced into the local timeline, not a server-side
  total. Reuses the room's existing `Timeline` subscription rather than
  adding new Rust/FFI surface (matrix-sdk has no media-filtered read API;
  raw `paginate_back_async` batches are filtered to Image/Video client-side),
  and reuses the existing `ImageViewerOverlay`/`VideoViewerOverlay`
  lightboxes for cell clicks. The gallery is scoped to the chat-panel area
  (the sidebar stays visible/usable behind it) and closes automatically when
  its `RoomInfoPanel` entry point closes or the active room changes.
  Pagination is a retry/accumulate loop: since a raw batch is unfiltered, a
  media-sparse stretch of history may need several backend round-trips
  before enough media turns up, reaches the room's start, or a per-gesture
  retry cap is hit.
- perf: the gallery's denser thumbnail grid and more aggressive backward
  pagination surfaced several pre-existing relayout/invalidation costs that
  a normal chat timeline rarely triggers hard enough to notice:
  - Win32 and macOS's `on_media_bytes_ready_` called the surface's
    `relayout()` directly and synchronously on every decoded-image
    completion — a full app-wide `arrange()` per completion. GTK4 and Qt6
    already routed this through the coalescing `schedule_relayout_()`
    helper (which folds a burst of calls into one deferred pass); Win32 and
    macOS now match.
  - `MessageListView::notify_image_ready()` called the blanket
    `invalidate_data()` whenever a newly-decoded image's URL matched a row
    already loaded in the timeline, forcing a full re-measure of every row
    (rebuilding any not-yet-cached row's text layout) on every match. Now
    calls the existing targeted `invalidate_row()` for just the matched
    row(s).
  - `MessageListView` kept doing this relayout work even while fully
    covered by the gallery overlay and thus invisible. Added
    `set_relayout_suppressed()`: while suppressed, `arrange()` short-circuits
    before the expensive base `ListView::arrange()`; data mutations
    (messages_, pagination) still apply normally underneath, and the
    deferred dirty state collapses into a single catch-up re-measure on the
    next real `arrange()` once the gallery closes.
- fix(timeline): decrypt old events on jump-to-date in encrypted rooms.
  Jump-to-date builds a `TimelineFocus::Event` timeline whose `/context`
  events live only in an in-memory `EventFocusedCache` that is never
  persisted; the matrix-sdk redecryptor (R2D2) only finds UTDs to retry in
  the store or the live room cache, so it never re-decrypted this focused
  cache's historic events when a backup key arrived. Now pre-fetches the
  room's backed-up Megolm sessions via `download_room_keys_for_room()`
  before building the focused timeline (a single bounded
  `GET /room_keys/keys/{roomId}`, a no-op when backup isn't set up), so the
  `/context` events decrypt inline during the build. Best-effort — a
  failure is logged and doesn't block the jump.
- fix(theme): `LoginView`/`JoinRoomView`/`QRGrantView` status and discovery
  labels called `set_colour()` with hardcoded hex literals, which
  permanently overrides `Label`'s per-paint fallback to the theme palette —
  so they stayed tuned for light mode and never reacted to dark mode or a
  live theme switch. Now resolved from `ctx.theme.palette` in each view's
  `paint()` instead, matching the surrounding chrome. Also fixes a
  Windows-only ordering bug where `branding_surface_` was constructed after
  the startup `apply_current_theme_()` call, so `BrandView` never received
  the resolved system theme on first launch.
- test(message-list): fix 18 C++ tests broken by the bottom-anchored
  short-content layout (`set_anchor_content_bottom()`, above) — every
  affected test hardcoded pixel coordinates written for the old top-anchored
  layout (hover/click points, pixel-sample offsets, an image-hit-test scan
  range), which now miss the row entirely. Added a `bottom_anchor_pad()`
  test helper computing the same pad the widget applies internally, and used
  it to shift the affected probes. No production code changes — the tests
  were simply out of date.
- fix(macos): `NSCell` already declares an atomic, strong `image` property;
  redeclaring it nonatomic on `TKEmoticonCell` conflicted with the inherited
  attribute and silenced auto-synthesis, producing two warnings in the
  macos-appkit release build.

#### 2026-07-09

- perf(timeline): a just-sent message's local echo could take seconds to
  appear in the timeline under background load, even though the same message
  showed up promptly in the room list. Root cause was tokio async-worker
  starvation in the SDK: the CPU-bound `Timeline::init_focus` build (run on a
  room switch, collecting cached events into an `imbl::Vector`) executed on an
  async worker and starved the matrix-sdk send-queue / diff-generation tasks
  that emit the echo. Both `init_focus` builds (`subscribe_room`,
  `subscribe_room_at`) now run on tokio's blocking pool via `spawn_blocking` +
  a runtime `Handle`, leaving every async worker free; blocking-pool threads
  inherit the widened 8 MB stack the macOS deep-recursion guard needs. This
  supersedes the 2026-07-08 attempt below, which misattributed the delay to the
  C++ UI queue (that queue also carries the prompt room-list update, so it was
  never the bottleneck) — those changes still land as general perf wins.
- perf(timeline): instant room switch — no viewport auto-fill, bottom-anchor,
  warm favorites. Switching to a room fired a back-pagination the instant the
  cached tail didn't fill the viewport (`ListView`'s arrange-time
  `on_near_top` auto-fire), which crosses the event-cache gap the SDK plants
  behind a freshly-synced tail and resolves it with a `/messages` round-trip
  — a spinner on every switch. matrix-sdk-ui pagination is store-first (it
  only networks at a true gap), so the fix is to stop forcing that
  pagination on open and let it happen on scroll, mirroring Element X.
  `ListView` gains `set_autofill_only_when_empty()` (the arrange-time
  auto-fire now only triggers for a genuinely empty list — enabled on the
  message list, thread lists keep fill-on-open) and
  `set_anchor_content_bottom()` + `content_top_pad()` (content shorter than
  the viewport anchors to the bottom, as chat expects, with the pad applied
  consistently to paint and hit-testing so clicks/hover stay aligned).
  `ShellBase::prune_warm_subscriptions_` also keeps favorite rooms in the
  keep-set so a favorite that's been opened is never evicted past
  `kWarmRoomsMax` — returning to it stays instant for the session.
- perf(timeline): fill a full window from cache on room open. The initial
  history load used the same 50-event scroll increment as a scroll-up
  fetch, so a maximized desktop window opened only partly filled even when
  far more recent events were already cached on disk. matrix-sdk pagination
  is store-first — it reads cached chunks from disk and only reaches the
  network at a genuine gap — so raising the initial-fill count pulls more
  already-cached history straight from disk with no server round-trip; a
  room with a genuine gap right behind the tail does at most one
  `/messages` to bridge it. Adds `kInitialFillBatch = 100` for the on-open
  paginate; scroll-up increments keep using `kPaginationBatch = 50`.
- feat(windows): vendored BetterText (`third_party/bettertext`), a from-scratch
  D2D/DirectWrite Win32 text control, as a new backend for
  `NativeTextField`/`NativeTextArea`. Adds change/submit notifications,
  content-height query, single-line mode, placeholder + password rendering,
  inline IME composition with candidate-window positioning, opt-in scrollbar,
  per-axis padding, and real inline bitmap rendering — so custom emoji now
  render inline in Windows compose fields (via the existing mxc media-fetch
  pipeline) instead of the old plain-text fallback. Wired alongside the
  existing EDIT/RichEdit-backed classes (not yet removed, pending manual
  verification).
- fix(windows): route BetterText emoji glyphs through Tesseract's bundled Noto
  Color Emoji font (via a `IBetterTextFontProvider` adapter over the existing
  `build_emoji_fallback()` collection) so emoji in compose/search/settings
  fields match the message list instead of the OS "Segoe UI Emoji" fallback.
- fix: multiline messages weren't rendering as multiline — two independent
  `<br>`/newline bugs. `markdown.rs`'s "has real formatting" detector counted a
  trailing newline as formatting (so plain multiline text was needlessly sent
  with a `formatted_body`, unlike Element); and `html_spans.cpp` only treated a
  self-closed `<br />` as a line break, so the bare `<br>` the HTML5 sanitizer
  re-serializes produced no break at all.
- fix: thread replies dropped `formatted_body`, silently losing MSC2545 custom
  emoji and @mention `matrix.to` links — `RoomView`'s thread-panel send path
  forwarded an empty formatted body. All four shells (and the shared popout
  base) now rebuild `(body, formatted_body)` from the live compose draft the
  same way the non-thread `on_send` path does.
- fix: close the reaction/emoji picker on an outside click, and keep a message
  row's action buttons visible while the picker is open — the picker briefly
  stealing input focus was clearing the row's hover state (Qt6/GTK4/macOS).
  Also clears pending-reaction state on outside-click dismissal so the next
  compose-bar emoji pick doesn't wrongly fire as a reaction. Windows was
  already unaffected.
- fix: don't drop the room-switch anti-reflow gate on a same-room timeline
  reset (e.g. a pagination refill), which was revealing rows with unresolved
  image/preview heights — the overlapping-messages-on-room-switch symptom.
  `RoomSwitchGateKeeper::reset_within_switch()` now re-arms an active gate
  instead of tearing it down.
- build: enable CMake unity builds (`TESSERACT_UNITY_BUILD`, default ON),
  cutting a clean full C++ rebuild from ~60s to ~40s. Required renaming ~150
  colliding file-local constants and ~60 test-fixture symbols now sharing a
  translation unit; also `fix(macos)` renamed colliding
  `kPanelWidth`/`kPanelHeight` in EmojiPicker/StickerPicker and silenced
  `-Wsubobject-linkage` in AboutSection/GtkSniTrayIcon.
- fix(build): wire up Windows test-target linking (UNICODE, D2D/D3D11/DWrite,
  `screen_capture_win32.cpp`, video capture).

#### 2026-07-08

- feat(image-viewer): copy the displayed image to the clipboard from the
  full-window lightbox, beside the save button. New
  `tk::Host::set_clipboard_image` with native backends for Qt6, GTK4, Win32
  (WIC → CF_DIBV5) and macOS; a self-dismissing "Copied to clipboard" toast
  confirms (a status-bar message would sit behind the scrim). The Qt backend
  sets `x-kde-force-image-copy` so copies land in KDE Klipper even with "Ignore
  images" enabled. Opt-in per overlay, so the video overlay is unaffected. 5
  new tests.
- perf(compose): first (superseded) attempt at the slow local echo — see the
  2026-07-09 entry for the actual fix. These changes still landed as general
  perf wins: Qt/Win32/macOS avatar + Qt message-tile image decode moved off the
  UI thread; media/room relayout finalizers coalesced onto the shared
  `schedule_relayout_`; an arriving map tile triggers a plain repaint instead
  of a full O(timeline) `invalidate_data()` re-measure; and plain-text send
  routed through the mutation worker pool like every other composer mutation,
  with failures surfacing via the per-message ◷→⚠/retry indicator.
- build: merged the four `linux-{gtk,qt6}-{debug,release}` CMake presets into
  `linux-debug`/`linux-release`, which configure and build both the GTK4 and
  Qt6 UIs in one pass (new `TESSERACT_UI=linux`); CI still passes
  `-DTESSERACT_UI=qt6` to produce the single-backend .deb/.rpm/AppImage.
- build: moved the per-platform `tk::` backend implementations (canvas/host/
  audio/video/screen-capture) out of the cross-platform `tesseract_tk` library
  into each platform's own target (genuinely shared GStreamer code stays);
  follow-on include-path/ARC fixes for the Qt6, GTK4 and macOS backends.
- docs(roadmap): rewrote ROADMAP.md as a single priority-ordered backlog
  (tiers by urgency), dropping everything already shipped.

#### 2026-07-07

- fix(cpack): declare the `qt6-image-formats-plugins` runtime dependency for
  the .deb/.rpm packages.

#### 2026-07-06

- feat(composer): render custom MSC2545 emoji inline, end to end. Picking a
  custom emoji from the picker or shortcode autocomplete inserts a real inline
  image pill in the composer (mirroring the @mention pill mechanism per
  platform), sends proper `<img data-mx-emoticon>` HTML, and renders that tag
  as an inline image in the read-only timeline. Windows keeps a plain-text
  fallback (RichEdit's GDI object-drawing pass is incompatible with the
  DXGI/D2D swap-chain rendering — later addressed by the BetterText backend).
  Inline images use U+FFFC + each backend's native inline-object mechanism
  (`IDWriteInlineObject`, `PangoAttrShape`, `QTextObjectInterface`). SDK
  re-sanitizes the raw event JSON with `data-mx-emoticon` allow-listed
  (matrix-sdk-ui's sanitizer strips it), and `image_packs.rs` now uses ruma's
  typed MSC2545 `PackInfo`/`MxcUri`. Fixes an XSS: the attacker-controlled
  shortcode was interpolated into `alt`/`title` unescaped.
- fix(ci): bundle Qt6 image-format plugins (WebP/TIFF/ICO, etc.) in AppImage
  builds — they were never installed on the AppImage runners, so those formats
  silently failed to load despite working in .deb/.rpm.
- docs: call out the Matrix Authentication Service + Sliding Sync server
  requirement in the README and landing page.

## v0.8.13 — 2026-07-06

### Summary

- feat(room-settings): the Permissions tab warns and disables Accept when a staged change would lock the user out of editing permissions
- fix(macos): linked `CoreMedia`/`CoreVideo` into `tesseract_tk`
- fix(macos): linked `CoreAudio` explicitly into `tesseract_tk`

### Details

- feat(room-settings): the Permissions tab now warns and disables Accept
  if a staged change would lock the current user out of ever editing room
  permissions again — raising "Change permissions" above their own level,
  or lowering "New members" out from under an account with no personal
  power-level override — mirroring the existing encryption "can't undo"
  warning. The warning only fires when the user can actually edit
  permissions in the first place; otherwise every combo is already
  disabled and there's nothing staged to warn about. Determining the
  user's own effective power level uses ruma's `RoomPowerLevels::for_user`
  rather than reading the `users`/`users_default` fields directly, since
  room versions 12+ give creators an "infinite" power level that never
  appears in the `users` map at all — a naive lookup misreports a creator
  as unprivileged. New `Client::room_own_power_level`.

- fix(macos): linked `CoreMedia`/`CoreVideo` into `tesseract_tk` — those
  frameworks were only linked into the `tesseract_macos` executable, but
  `video_capture_macos.mm` (which uses their pixel-buffer APIs) lives in
  the shared library, causing undefined-symbol errors at link time.

- fix(macos): linked `CoreAudio` explicitly into `tesseract_tk` —
  `host_macos.mm`/`audio_capture_macos.mm`'s device-enumeration APIs were
  only resolving transitively through the calls/WebRTC stack, so linking
  failed without `TESSERACT_ENABLE_CALLS`.

## v0.8.12 — 2026-07-05

### Summary

- feat(room-settings): `RoomSettingsView` is now a tabbed layout (General / Media / Security & Privacy / Permissions) via `tk::SideTabView`
- feat(location): clicking a location message's map now opens it on openstreetmap.org
- feat(screenshare): the screen-share picker shows real per-source thumbnails (captured off the UI thread) instead of placeholder tiles
- feat(room-settings): a wrench icon in the room-info panel opens a new full-panel view for editing the room's avatar
- feat(timeline): an opt-in "Show room join/leave events" setting surfaces membership transitions in the timeline
- feat(voice): a voice message that finishes playing on its own now automatically starts the next voice message from the same sender in the room, if any
- feat(calls): MatrixRTC voice and video calls (MSC4143) via LiveKit, behind `TESSERACT_ENABLE_CALLS`
- feat(compose): `/selfie` slash command opens a full-surface camera overlay with a countdown and mirrored live preview
- feat(settings): audio/video device selection in Settings → Media
- feat(rooms): bridged-room detection (MSC2346) suppresses the call button and threads panel for bridged rooms and shows a Bridged badge
- feat(room-list): a phone icon now appears on rooms with an active call
- feat(calls): the call button and incoming-call banner are hidden when the server doesn't advertise LiveKit/MSC4143 transport support
- feat(spaces): `SpaceRootView`
- feat(message-list): image/file/video captions are now linkified
- build: set the global `CMAKE_CXX_STANDARD` to 20
- perf(idle CPU): fixed the app burning CPU while idle with the window hidden, traced via `perf`
- perf(animated images): an animated inline sticker/GIF no longer forces a full repaint of the entire visible UI on every frame
- perf(inline video): revisiting a room with an inline/autoplay video resumes the paused player instead of rebuilding the hardware decode session
- ci: added a manual per-platform installer build workflow
- refactor(shell): centralised the user context menu into `ShellBase::build_user_menu_items_()` across all four platforms
- Decoupled the in-flight-request spinner from the GIF animation timer
- Unread-room prefetch now includes notifying (Count/Mention) rooms, not just quiet unreads
- Refactored `MainAppWidget`'s widget tree: shared traversal/keyboard dispatch primitives
- Unjoined-room space-preview fetches are now cancellable: navigating away from a space aborts still-in-flight summary requests
- fix(rtc): starting an audio-only call was still signaling `m.call.intent="video"` to the room
- fix(compose): `ComposeBar::set_enabled`/`enabled()` now correctly override `tk::Widget`'s virtual instead of silently hiding it
- fix(macos): fixed a bitwise operation between mismatched types in the screen-capture code (`screen_capture_macos.mm`)
- fix(sdk): `backfill_room_silent()` now drains its Timeline's diff-processing stream to quiescence before dropping it
- fix(call): mute, video-mute, and screen-share state now survive switching the call overlay between Docked, Floating
- fix(macos-x86_64): fixed a build error and two warnings specific to the x86_64 release build
- fix(room-list): avatar changes for a room or DM counterpart now update the room list live
- fix(unread): state events (e.g. another member's avatar change) can no longer produce a stray
- fix(linux-qt): messages arriving no longer steal window focus on Wayland
- fix(room-info): the room topic now updates immediately in both the info panel and the header on save
- fix(ui): the close and edit-topic icon buttons in the room-info and user-profile panels are now actually visible
- fix(selfie): `/selfie` is now a strict no-op when no camera is present or macOS permission was denied
- fix(scroll): fixed two bugs letting pagination move what the user was looking at instead of only growing the scrollable range
- fix(login): the Add Account cancel button now correctly returns to the main app on Win32/macOS
- fix(room-list): edited messages show the real edited content in the last-message preview instead of the spec-mandated asterisk
- fix(room-list): image/sticker last-message thumbnails now respect the media privacy setting instead of always fetching and rendering
- fix(ui): fixed the pointer-move regression from the widget-routing refactor above
- fix(windows): fixed a shutdown hang where a large in-flight JPEG decode could block shutdown for seconds
- fix(deps): pinned `webrtc-sys` to `=0.3.35` in `Cargo.toml`
- fix(pins): a room switch no longer clobbers a pin-state update that was computed just before the switch landed
- fix(room-view): switching rooms now closes any open overflow menu or call popup instead of leaving it open over the new room
- fix(gui): restored button hover across the whole app
- fix(quick-switcher): recent-room names in the "Recent" strip are now correctly centred under their avatars
- fix(list-view): a room switch keeps loading history until the timeline fills the viewport or the room's history is exhausted

### Details

- fix(rtc): starting an audio-only call was still signaling
  `m.call.intent="video"` to the room, so it showed up in the timeline (and
  on the receiving client) as "started a video call". The `audio_only` flag
  chosen in the UI never made it past `Client::rtc_start_call` — it's now
  threaded through the cxx bridge into `rtc::session::start_call` and on
  into the MSC4075 ring notification, every MSC3401 `m.call.member`
  join/resend/sticky-refresh, and the MSC4195 slot/member helpers.

- build: set the global `CMAKE_CXX_STANDARD` to 20, fixing MSVC C7589
  ("defaulted comparison operators require /std:c++20") on any target that
  didn't explicitly request `cxx_std_20` via `target_compile_features`.

- fix(compose): `ComposeBar::set_enabled`/`enabled()` now correctly
  override `tk::Widget`'s virtual instead of silently hiding it, fixing a
  missing-virtual-override compiler warning.

- fix(macos): fixed a bitwise operation between mismatched types in the
  screen-capture code (`screen_capture_macos.mm`).

- feat(room-settings): `RoomSettingsView` is now a tabbed layout
  (General / Media / Security & Privacy / Permissions) via `tk::SideTabView`,
  matching the app-wide Settings view instead of the old single-page
  layout; every field across all four tabs is staged locally and only sent
  on Accept. General gained a Room ID row that copies to the clipboard with
  toast feedback (new `Toast` widget). Media adds a per-room MSC4278
  media-previews override (Always/Never/Use global default). Security &
  Privacy exposes encryption (locks once enabled — there's no undo),
  join rule, guest access, and history visibility, each gated by its own
  `can_set_room_*` permission check and read via an on-demand `GET /state`
  fetch rather than trusting sliding sync. Permissions (new) mirrors
  Element's "Roles & Permissions" tab — default role, invite/kick/ban,
  message/settings/permissions defaults, and @room notifications — via a
  typed `RoomPowerLevelsFfi` struct. Also fixes a tab-switch relayout bug
  and hoists `enabled`/`set_enabled` out of `Button`/`CheckButton`/
  `SwitchButton`/`ComboBox` and into `tk::Widget` itself.

- fix(sdk): `backfill_room_silent()` now drains its Timeline's
  diff-processing stream to quiescence before dropping it, instead of
  discarding the stream immediately while the Timeline (the last strong
  reference) stayed alive until the function returned — could otherwise
  abort matrix-sdk-ui's internal diff-application task mid-batch, tripping
  a benign but noisy "a DateDividerAdjuster has not been consumed with
  run()" error log.

- perf(idle CPU): fixed the app burning CPU while idle with the window
  hidden, traced via `perf`. matrix-sdk's default cross-process store lock
  spawns a lease-renewal task that writes to the event-cache and crypto
  SQLite stores every 50ms for the entire session, purely to guard against
  a second OS process touching the same store — since Tesseract already
  enforces a single running instance per profile, that guard was disabled
  (`CrossProcessLockConfig::SingleProcess`).

- perf(animated images): fixed a bug where an animated inline sticker/GIF
  forced a full repaint of the *entire* visible UI (room list, header,
  compose bar, every message row) on every ~16ms animation tick, instead of
  just the animated region — confirmed via `perf` to cost roughly 50% of
  CPU while otherwise idle with a sticker animating. Root cause was a
  `Canvas::clip_rect()` bug on Qt6/macOS: an empty clip after intersection
  (meaning "this pane doesn't overlap the current damage rect at all") was
  being treated the same as "no clip set," so every pane sharing the
  surface repainted and re-shaped text on every frame. Win32 needed the
  same fix plus new plumbing (it had scoped `InvalidateRect` calls but
  never wired the resulting dirty rect into the paint pass). GTK4 has no
  partial-invalidation API for a single widget at all, so it needed a
  different mechanism: each currently-animating, currently-visible image
  now gets its own small overlay `GtkDrawingArea`, since GTK4's per-widget
  render-node caching means invalidating one small widget doesn't force
  its siblings to redraw (the same trick `GtkVideo` uses).

- perf(inline video): switching back to a room with an inline/autoplay
  video was paying to tear down and rebuild a hardware decode session
  (e.g. a CUDA context on the Qt6/FFmpeg backend) every time, even when
  it was the exact same video already seen before — the cost is tied to
  `play()`/`setSourceDevice()` itself, not to constructing a fresh player
  object. Revisiting the same video now resumes an already-loaded,
  paused (not stopped) player directly, skipping the fetch/decode entirely.

- fix(call): mute, video-mute, and screen-share state now survive
  switching the call overlay between Docked, Floating, and Popout modes —
  they were previously excluded from the state snapshot and silently
  reset to off on every mode switch.

- feat(location): clicking a location message's map now opens it on
  openstreetmap.org, centred on the pin (a plain click is distinguished
  from a pan drag via a no-movement threshold).

- feat(screenshare): the screen-share picker now shows real per-source
  thumbnails (captured off the UI thread) instead of placeholder tiles,
  with a new `capture_thumbnail()` platform interface (DXGI Desktop
  Duplication / one-shot `PrintWindow` on Windows, `SCScreenshotManager`
  on macOS 14+). Fixed several Linux stability bugs found along the way:
  a black-tile bug with three independent causes in the xdg-desktop-portal
  + PipeWire pipeline (wrong node-lookup ID space, premature D-Bus session
  teardown, a lost frame callback on restart), a UI freeze on stopping a
  capture if the pipeline stalled mid-negotiation, and unclosed portal
  sessions leaking past process lifetime. Also fixed Windows window
  capture rendering solid black for GPU-composited apps (browsers,
  Electron), a macOS thumbnail deadlock (two nested waits on the same
  serial queue), and a participant's screen-share tile incorrectly
  inheriting their camera/mic mute state.

- fix(macos-x86_64): fixed a build error and two warnings specific to the
  x86_64 release build (an invalid ObjC ivar initializer, a
  misplaced-namespace function, and a `__bridge_transfer` leak on a
  non-ARC target).

- fix(room-list): avatar changes for a room or DM counterpart now update
  the room list live — the fingerprint gating whether a fresh snapshot
  reaches the UI didn't include the avatar URL fields, so an avatar-only
  change looked identical to "nothing changed" and was silently dropped
  until some unrelated change happened to perturb it.

- fix(unread): state events (e.g. another member's avatar change) can no
  longer produce a stray, permanently-stuck notification badge. matrix-sdk's
  unread-message count excludes state events but its notification/mention
  counts don't apply the same filter, so a room-wide "All Messages" push
  rule matched the state event and bumped the count — and since state
  events never advance the read-receipt target, that count never cleared.
  Notification/highlight counts are now clamped to the real unread-message
  count.

- fix(linux-qt): messages arriving no longer steal window focus on
  Wayland — `QApplication::alert()` flashes the taskbar on X11 but Qt's
  Wayland backend can turn it into a real activate request, raising the
  window even without user interaction. The D-Bus notification already
  sent alongside it is the correct passive signal, so the `alert()` call
  was dropped.

- feat(room-settings): a wrench icon in the room-info panel opens a new
  full-panel view for editing the room's avatar, display name, and topic,
  with per-field power-level gating; nothing is sent until Accept, and
  Cancel discards all staged edits.

- fix(room-info): the room topic now updates immediately in both the
  info panel and the header on save, instead of waiting for the SDK to
  echo the state event back through sync (sometimes only after a
  restart).

- fix(ui): the close and edit-topic icon buttons in the room-info and
  user-profile panels are now actually visible — `Variant::Icon` buttons
  only paint their own hover/press background and rely on the caller to
  draw the glyph, which neither panel was doing.

- fix(selfie): `/selfie` is now a strict no-op (no overlay flash) when no
  camera is present or macOS permission was previously denied, instead of
  opening an overlay that could never show a frame. The countdown also no
  longer starts until the first frame actually arrives, so an OS camera
  permission dialog doesn't eat into it.

- feat(timeline): an opt-in "Show room join/leave events" setting
  (Settings → Appearance, default off) surfaces join/leave/kick/ban/invite/
  knock state-event transitions in the timeline. Consecutive same-action
  events collapse into one summary line with stacked avatars (e.g. "Alice,
  Bob and 3 others joined the room"); clicking expands each into its own
  line.

- ci: added a manual per-platform installer build workflow, so a single
  broken platform can be rebuilt from the Actions tab without re-running
  the full 5-platform release matrix.

- fix(scroll): fixed two related bugs letting a pagination-triggered
  content change move what the user was actually looking at, instead of
  only growing the scrollable range around it. Backward pagination
  (scrolled to the top) no longer pushes current content out of view in
  favor of newly-loaded history above it; forward pagination while
  browsing history no longer yanks the view down to the bottom on every
  page load (that auto-scroll is now correctly limited to a live message
  arriving while already pinned to the tail).

- feat(voice): a voice message that finishes playing on its own now
  automatically starts the next voice message from the same sender in the
  room, if any. Getting this right end-to-end surfaced three bugs: a
  same-thread audio backend re-entering the progress callback during a
  manual row switch (spuriously triggering auto-advance mid-switch); the
  auto-advance target almost always being off-screen and cold on its first
  play, which an existing on-screen-only retry guard blocked; and
  natural-completion detection relying on a playback position reset that
  Qt/AVAudioPlayer never actually do — each platform's audio backend now
  reports end-of-media via its own native signal instead.

- feat(calls): MatrixRTC voice and video calls (MSC4143) via LiveKit, behind
  `TESSERACT_ENABLE_CALLS`. `RtcSession` handles join/leave and audio/video
  muting; end-to-end encryption uses HKDF key derivation matching Element
  Call's wire format so calls interoperate with Element X and Element Call;
  echo cancellation runs through each platform's native audio device
  manager. The call overlay UI (`ParticipantTile`, `CallOverlayWidget`) has
  four modes — Docked (a strip above the messages), DockedExpanded (fills
  the content area), Floating (draggable, position persisted), and Popout
  (a dedicated OS window via `CallWindowBase`) — plus mute/video/hang-up
  controls, a call duration timer, and a pinned-tile 70/30 grid split.
  `IncomingCallBanner` surfaces MSC4075 ring notifications. Shipped with a
  string of correctness fixes: floating-overlay drag tracking (was running
  at half speed due to a stale local-vs-world coordinate mixup), participant
  tile pin/avatar/mic-badge layout, pin-icon color sync, and the docked
  panel staying visible when returning to docked mode in the wrong room.

- feat(compose): `/selfie` slash command opens a full-surface camera
  overlay with a 3-second countdown and mirrored live preview; the
  captured still is JPEG-encoded via each platform's native API (Qt
  `QImage`, `GdkPixbuf`, `NSBitmapImageRep`, WIC) and inserted as a
  compose-bar attachment. Disabled while a call is active. Windows and
  macOS needed follow-up fixes: hiding native text-field overlays while the
  camera overlay is open, Media Foundation initialization ordering and an
  `ARGB32`-capable video processor (Windows), and BGRA/JPEG color-channel
  swaps plus a click-to-dismiss dangling-pointer crash (macOS).

- feat(settings): audio/video device selection in Settings → Media —
  microphone, speaker, and camera dropdowns, enumerated at settings-open
  time and applied at the next session start. Backed by `QMediaDevices` +
  `GstDeviceMonitor` (Qt6), `GstDeviceMonitor` (GTK4), WASAPI
  `IMMDeviceEnumerator` + Media Foundation (Win32), and
  `AVCaptureDeviceDiscoverySession` + CoreAudio (macOS). A new
  `tk::FormLayout` widget (auto-sizes the label column to the widest label)
  replaced three separate rows that had been misaligned, and each combo
  gained a label plus i18n coverage.

- refactor(shell): centralised the user context menu (Settings → Add
  Account → QR → Log Out → Quit) into `ShellBase::build_user_menu_items_()`
  for all four platforms, replacing hand-rolled per-shell menu code.

- fix(login): the Add Account cancel button now correctly returns to the
  main app on Win32/macOS — switching back to the already-active account is
  a no-op, so the UI refresh that hides the login view was never reached;
  it's now called explicitly on cancel.

- feat(rooms): bridged-room detection (MSC2346 `uk.half-shot.bridge` state
  events) suppresses the call button and threads panel for bridged rooms
  and shows a 🌉 Bridged badge in the room-info panel, alongside a SQLite
  cache of bridge status for rooms outside the local state store's default
  sync scope.

- feat(room-list): a phone icon now appears on rooms with an active call,
  driven by `m.call.member` state (filtered to non-expired memberships via
  `active_memberships()`).

- feat(calls): the call button and incoming-call banner are hidden
  entirely when the server doesn't advertise LiveKit/MSC4143 transport
  support (probed once via `get_server_info`).

- fix(room-list): edited messages now show the real edited content in the
  last-message preview instead of the spec-mandated `"* "` fallback
  asterisk, which was previously truncated to just `"*"` by the HTML
  first-line splitter.

- fix(room-list): image/sticker last-message thumbnails now respect the
  media privacy setting instead of always fetching and rendering.

- feat(spaces): `SpaceRootView` — a centred summary panel (avatar, name,
  topic, joined/unjoined child counts) shown when a joined space itself is
  selected, mirroring `RoomPreviewView`'s layout for unjoined rooms.

- Decoupled the in-flight-request spinner from the GIF animation timer —
  the spinner no longer keeps a 62.5 Hz timer alive for the whole sync
  session; it gets its own independent tick that stops when the in-flight
  count drops or all windows are hidden.

- Unread-room prefetch now includes notifying (Count/Mention) rooms, not
  just quiet unreads, since those are the most likely to be opened next.

- Refactored `MainAppWidget`'s widget tree: shared traversal/keyboard
  dispatch primitives, focused internal containers, and a shared native
  overlay geometry registry. This wrapped the lightbox/dialog overlays and
  the floating-call layer in two always-visible pass-through containers,
  which regressed all-app pointer-move (see next entry).

- fix(ui): fixed the pointer-move regression from the widget-routing
  refactor above — the new overlay/call-layer containers didn't override
  `dispatch_pointer_move`'s "claim self if no child absorbs the hit"
  default, so they swallowed every pointer-move before it reached the room
  view, breaking message-row hover.

- fix(windows): fixed a shutdown hang where a large in-flight JPEG decode
  could block shutdown for seconds — `decode_image()` now runs each call in
  its own COM apartment instead of marshaling through the shared STA
  message queue.

- fix(deps): pinned `webrtc-sys` to `=0.3.35` in `Cargo.toml` — without an
  explicit pin the resolver could select 0.3.36, which is incompatible with
  the `libwebrtc` version this project depends on.

- Unjoined-room space-preview fetches are now cancellable: navigating away
  from a space aborts any still-in-flight `get_space_child_summary_async`
  calls for the old space instead of letting them complete uselessly.

- fix(pins): a room switch no longer clobbers a pin-state update that was
  computed just before the switch landed.

- fix(room-view): switching rooms now closes any open overflow menu or call
  popup instead of leaving it open over the new room.

- fix(gui): restored button hover across the whole app. The overlay/
  call-layer containers introduced by the widget-routing refactor above
  also needed a `hit_test` override matching the `dispatch_pointer_move`
  fix already made for them — `Host`'s button-hover tracking uses
  `hit_test`, not `dispatch_pointer_move`, so buttons app-wide (composer
  sticker/emoji/mic, room header) stopped highlighting on hover until this
  landed.

- fix(quick-switcher): recent-room names in the "Recent" strip are now
  correctly centred under their avatars — the paint code was manually
  pre-centring the draw origin on top of the canvas backend's own
  `halign=Center` centring, double-applying the offset on Qt6.

- fix(list-view): room switch now automatically keeps loading history until
  the timeline fills the viewport (or the room's real history is
  exhausted), instead of only loading a single fixed batch and requiring a
  manual scroll to trigger more.

- feat(message-list): image/file/video captions are now linkified — bare
  URLs render as clickable links, reusing the same rich-text pipeline as
  regular message bodies.

## v0.8.11 — 2026-06-30

### Summary

- fix(macos): fixed a stack-overflow crash on macOS when a thread's timeline reset while the message list was mid-layout
- fix(media): media requests no longer appear to freeze in rooms that trigger many of them at once

### Details

- fix(macos): fixed a stack-overflow crash on macOS when a thread's timeline
  reset while the message list was mid-layout. `set_repaint_requester` was
  wired to `Surface::relayout()` — a full synchronous layout pass — instead of
  a deferred OS repaint like the other three platforms (`update()` /
  `gtk_widget_queue_draw()` / `InvalidateRect`). `MessageListView::arrange()`
  applying a deferred scroll for a just-arrived event called
  `scroll_to_event_id()`, which fired the repaint requester, which re-entered
  `relayout()` from inside the layout pass it was already running, recursing
  until the stack guard page was hit (~18,600 frames in the field crash
  report). Fixed by routing the macOS repaint requester through
  `host().request_repaint()` (`setNeedsDisplay:`), matching the other shells.

- fix(media): media requests no longer appear to freeze in rooms that trigger
  many of them at once, and the in-flight indicator no longer lingers
  indefinitely after switching away from a media-heavy room. Several
  contributing issues, found via a debug-tooltip instrumentation pass that
  labels each in-flight request with its cancellation group:
  - The download gate's hard ceiling on total in-flight requests counted
    long-stale (presumed-stuck) slots the same as healthy ones, so a burst of
    legitimately-needed media against a merely slow homeserver could block
    *all* further downloads until something timed out, in discrete
    freeze-then-burst waves. A second, longer stale threshold now exempts
    those slots from the ceiling too, while a shorter one already protected
    the soft per-lane limit.
  - The initial-room-open media prefetch fetched a fixed trailing window of
    50 timeline events regardless of how many rows actually fit on screen;
    it now scales to the message list's real viewport height.
  - Sender/read-receipt avatars and reaction-emoji images for timeline rows
    were fetched outside any cancellable group, so they kept downloading
    after the user left the room. They're now grouped with the rest of the
    room's media.
  - The dominant cause: every room switch eagerly fetched an avatar for
    *every* member of the room (for mention-pill / mention-click
    resolution), uncancelled — a large room's full membership reproduced the
    "looks frozen" symptom on every switch. Removed the bulk fetch entirely
    in favor of an on-demand fetch when a mention pill actually renders
    (already implemented on Qt6; now wired on GTK4, Win32, and macOS too,
    closing a platform gap where the supporting cache field existed but was
    never connected). The room-info panel's member-avatar list gained the
    same on-demand fetch, which it had been missing and was relying on the
    now-removed bulk prefetch to keep populated.

## v0.8.10 — 2026-06-29

### Summary

- fix(verification): device-verification lookups now retry with exponential back-off instead of failing on the first miss
- fix(message-list): thread chip and action-button hit rectangles are now cleared on every programmatic scroll
- fix(macos): macOS app packages no longer require Homebrew for voice/video

### Details

- fix(verification): device verification now retries `get_verification_request`
  and `get_verification` lookups with exponential back-off (up to 7 attempts,
  starting at 50 ms, doubling each attempt) instead of failing on the first
  miss. Under load the matrix-sdk crypto store can lag behind the sync handler
  that fires the verification-started callback, causing a single-shot lookup to
  find nothing and silently drop the request. Presence polling is also bounded
  to `PRESENCE_POLL_CONCURRENCY = 4` concurrent coroutines to prevent a
  thundering-herd on sync start.

- fix(message-list): thread chip and action-button hit rectangles are now
  cleared on every programmatic scroll — `scroll_to_event_id`, wheel, and
  pointer-drag. A new `clear_scroll_hit_geometry_()` helper resets the hit
  geometry, hovered-row geometry, `hover_target_`, and `hover_chip_idx_` in one
  call; previously stale rects caused phantom thread-chip taps and mis-fired
  action buttons after the timeline scrolled to an event. Covered by a new
  Catch2 test in `test_tk_message_list_threads`.

- fix(macos): macOS app packages no longer require Homebrew for voice/video.
  `libopus.dylib` is now copied into `Contents/Frameworks` at `POST_BUILD`;
  `install_name_tool` rewrites its `LC_ID_DYLIB` and the binary's
  `LC_LOAD_DYLIB` entry to `@rpath`-relative paths, and
  `@executable_path/../Frameworks` is added as `BUILD_RPATH`/`INSTALL_RPATH`.
  The existing `--deep` codesign step covers the bundled dylib automatically.
  The x86_64 CI packaging job now builds a shared `libopus.dylib` (was a static
  archive, which `install_name_tool` rejects) and correctly sets
  `PKG_CONFIG_PATH` and `PKG_CONFIG_ALLOW_CROSS=1` so `audiopus_sys` can locate
  the source-built library in cross-compile jobs.

## v0.8.9 — 2026-06-24

### Summary

- feat(room-list): "Group unread rooms" toggle in Appearance settings (off by default)
- feat(timeline): sender display names are tinted using a hash of the sender's Matrix user ID
- feat(room-list): space rooms now show their **topic** as the one-line preview line in the Spaces section instead of a last-message snippet
- fix(room-list): when "Group unread rooms" is on
- fix(presence): the forbidden-presence set
- fix(macos): fixed a dangling-reference crash on scroll-up in the timeline
- fix(video): on macOS, byte-swap R and B when AVFoundation delivers RGBA instead of the requested BGRA format
- fix(account-picker): the Win32 account-picker popup now scales correctly on HiDPI displays
- fix(audio): fixed a crash on Qt6 during app shutdown

### Details

- feat(room-list): "Group unread rooms" toggle in Appearance settings (off by
  default). When enabled, a new **Unread** section appears above Favorites in
  the room list, collecting every room with a visible unread indicator
  (notification count, highlight count, or an unmuted quiet unread) regardless
  of type — DMs, normal rooms, and rooms inside spaces. Rooms in the Unread
  section are suppressed from their normal section until they have no more
  unreads. The section's collapse state (`room_section_unread_collapsed`)
  persists across restarts alongside all other section states. Off by default.
  892 C++ tests.

- fix(room-list): when "Group unread rooms" is on, space-child rooms with
  unreads now bypass the space-child exclusion and appear in the Unread section
  at the root room list. Previously an unread room nested inside a space was
  invisible in the root view until the user drilled into the space. Muted rooms
  with only quiet unreads remain excluded (`UnreadStyle::None`). The fix lives
  in the new testable `filter_root_rooms()` free function alongside
  `classify_room_section()`; 7 new Catch2 tests cover the boundary cases. Also
  consolidates the four per-shell `refreshRoomList()` bodies (~90 LOC each)
  into a single `ShellBase::refresh_room_list_()`, aligning macOS nested-space
  visibility with Qt6/GTK4/Win32 (child-spaces now hidden from the root list on
  all platforms).

- feat(timeline): sender display names are tinted using a hash of the sender's
  Matrix user ID, mapped into an 8-hue palette tuned for contrast against the
  chat background in both light and dark mode. The color is stable across
  display-name changes because it keys on the mxid.

- feat(room-list): space rooms now show their **topic** as the one-line preview
  line in the Spaces section instead of a last-message snippet. Spaces are
  containers rather than chat rooms, so the topic is more informative. Falls
  back to name-only (centred) when the topic is absent.

- fix(presence): the forbidden-presence set — users that return 403 on presence
  requests — was in-memory only and lost on restart, causing unnecessary
  re-polls every session. It is now persisted to a `presence_forbidden` table
  in `app_cache.db` (the same DB as the media and room-summary backoff state).
  The set is restored before the polling task launches at sync start, so
  subsequent sessions skip those users from the first tick.

- fix(macos): fixed a dangling-reference crash on scroll-up in the timeline.
  `request_more_history` took `room_id` by `const&`; the caller passed a
  by-value local `std::string`, so the reference was already dangling by the
  time the `dispatch_async` block ran on the background thread. The cxx
  bridge's UTF-8 validation in `Str::Str` then threw `std::invalid_argument`
  on the garbage bytes. Taking `room_id` by value lets the ObjC++ block copy
  it at capture time.

- fix(video): on macOS, some videos caused AVFoundation to deliver
  `kCVPixelFormatType_32RGBA` instead of the requested BGRA format, swapping
  red and blue channels in the rendered frame. The video decoder now checks the
  actual format of each `CVPixelBuffer` and byte-swaps R and B when RGBA is
  delivered. Non-8-bit packed formats (e.g. 10-bit planar HDR) are left
  untouched. The common BGRA path remains an unmodified `memcpy`.

- fix(account-picker): the Win32 account-picker popup appeared at a fraction of
  its intended size on HiDPI displays because the hardcoded 260 × 56 DIP
  constants were passed unscaled to `CreateWindowExW` and `SetWindowPos`. Both
  calls now go through `dip_to_phys()`. Redundant inner re-declarations of the
  same constants removed.

- fix(audio): fixed a crash on Qt6 during app shutdown. `QFFmpegMediaPlayer::stop()`
  emits `positionChanged` synchronously, which triggered `fire_progress()` →
  `on_progress()` → `repaint_requester_()` into a widget tree already being
  torn down, crashing inside `QWidgetPrivate::update()`. `QtAudioPlayer`'s
  destructor now nulls `on_progress` before calling `player_.stop()`, so the
  guard in `fire_progress()` short-circuits the callback.

## v0.8.8 — 2026-06-22

### Summary

- feat: Forward message action
- feat(windows): "Show App" entry added at the top of the Win32 system-tray context menu
- feat(timeline): action pill buttons now show tooltip labels on hover
- feat: `forward_event` is now fully async
- perf: convert `fetch_source_bytes`, `fetch_url_bytes`, and `fetch_gif_bytes` to non-blocking async FFI
- perf(media): user avatars are now fetched lazily for visible rows only
- perf(media): backed-off retry requests are now scheduled at the lowest priority (`PRIO_BACKOFF = 0`)
- refactor(gst): GStreamer init consolidated
- i18n: 11 strings introduced since v0.8.7 are now wrapped in `tk::tr()` / `tk::trf()` and registered in `tesseract.pot` and `es.po`
- perf(media): reqwest upgraded to 0.13 (was 0.12 for tile / URL-preview fetches, 0.13 via matrix-sdk for media)
- perf(media): media prefetch is now gated to the last 50 events (`kMediaPrefetchWindow`) on room switch
- fix(roomlist): spaces now propagate `unread_count` from their children in addition to notification and highlight counts
- fix: five code-review findings addressed
- fix(timeline): `m.file` events now display the MSC2530 user caption below the file card
- fix(timeline): geometry maps (`image_geom_`, `video_geom_`, etc.) were unconditionally cleared at the start of every paint
- fix(timeline): sender avatars could be evicted by the 30-minute thumbnail LRU TTL while the app sat idle
- fix(timeline): clear stale hit-test geometry on room switch even mid-animation, so clicks no longer open the wrong room's media
- fix(compose): typing text between two semicolons with no matching emoji shortcode no longer causes infinite recursion in the shortcode-popup logic
- fix(rooms): room title changes now trigger a live room-list update

### Details

- feat: Forward message action. A "Forward message" item now appears in the ⋯
  more menu for any non-redacted, non-pending event (including messages from
  other users). Tapping it opens `ForwardRoomPicker` — a modal overlay with a
  NativeTextField search bar and a two-section list: selected rooms pinned above
  a divider, filtered unselected rooms below. Avatars load lazily using the same
  mechanism as QuickSwitcher. The Rust FFI `forward_event` fetches the original
  event, strips `m.relates_to` (preventing cross-room relation chains), and
  re-sends the raw content to each target room; all msgtypes (text, image, file,
  video, etc.) are preserved. Provider wiring and `on_forward_requested` live
  once in `ShellBase::wire_main_app_widget_()`, shared across all four platform
  shells. 870 C++ tests.

- perf: convert `fetch_source_bytes`, `fetch_url_bytes`, and `fetch_gif_bytes`
  to non-blocking async FFI, eliminating the last three synchronous media-fetch
  calls that pinned a C++ worker thread for a full network round-trip. A new
  `send_gif_from_urls_async` fetches image and preview in parallel via
  `tokio::join!` and sends as `m.video` (MP4) or `m.image` (WebP/GIF); the old
  `fetch_gif_bytes` + `send_gif_video` callers across all four shells and
  `RoomWindowBase` are removed. Also converts the remaining `block_on` FFI
  methods to async.

- perf(media): user avatars are now fetched lazily for visible rows only. On
  room switch, `build_rows_` previously called `ensure_user_avatar_` for every
  sender and read-receipt user in the snapshot (O(all events) before layout had
  run). A new `on_visible_avatars_changed` callback fires from each paint with
  the deduplicated sender + read-receipt URLs of the visible rows; `ShellBase`
  calls `ensure_user_avatar_` for those only. Two new `MediaKind` variants
  (`Sticker`, `Reaction`) replace the Qt6-only `prep_row_media_` override and
  `mediaImageSizes_` map.

- perf(media): backed-off retry requests are now scheduled at the lowest
  priority (`PRIO_BACKOFF = 0`), below `PRIO_NORMAL` and `PRIO_VISIBLE`. URLs
  that previously failed and are re-queued after their backoff window expires no
  longer compete with fresh prefetch for gate slots. `MediaQueue::prioritize()`
  is guarded to prevent a visible-row scroll from promoting a backoff entry.

- refactor(gst): GStreamer init consolidated. The four anonymous-namespace copies
  of `ensure_gst_init` in `audio_gtk.cpp`, `video_gtk.cpp`, and
  `video_decode_gst.cpp` are replaced by a single `tk::gst::ensure_gst_init()`
  in `gst_hw_probe.cpp`. After init, `libavutil` is dlopened (versioned
  fallbacks for FFmpeg 4–8) and `av_log_set_level(AV_LOG_QUIET)` silences
  FFmpeg noise emitted before `gst-libav` is lazily loaded. A crash-sentinel
  file (`XDG_RUNTIME_DIR` preferred, `/tmp` fallback, `O_CREAT|O_EXCL|O_NOFOLLOW`)
  is written before the call and removed on success; if the process crashes
  inside `av_log_set_level` the sentinel survives and the next run skips
  suppression.

- fix(roomlist): spaces now propagate `unread_count` from their children in
  addition to notification and highlight counts. Previously the quiet-unread dot
  never appeared on a space whose children had only un-notifying unread messages.
  The most-recent-activity timestamp is also propagated for the quiet-unread case
  so spaces sort by recency correctly when no notifications are present.

- fix: five code-review findings addressed — `ignore_user_async` and
  `unignore_user_async` in `client.cpp` now have the `if (!impl_) return` guard
  that all other methods carry (crash on logout while a block/unblock is in
  flight); the spoiler-reveal sticker branch passes `MediaKind::Sticker` to
  `ensure_media_image_` instead of the default `MediaKind::MediaImage` (wrong
  decode size + bypassed sticker repaint routing); Win32 GIF preview decoding is
  moved out of the `on_bytes` callback and into a `run_async_` worker (WIC must
  not run on the message pump); `client_` is snapshot on the UI thread before
  dispatching GIF `run_async_` workers (data race); `pending_media_` is drained
  immediately when `client_` is null at dispatch time (inflight-URL leak
  blocking future fetches of the same URL).

- feat(windows): "Show App" entry added at the top of the Win32 system-tray
  context menu, mirroring the existing Linux Qt6 tray menu.

- feat(timeline): action pill buttons now show tooltip labels on hover. All
  five buttons (react, reply, reply-in-thread, edit, more) fire through the
  existing `on_show_tooltip` / `on_hide_tooltip` pipeline; no shell changes
  were needed. The pill background is also raised from `subtle_hover` to
  `subtle_pressed` for better contrast against message content.

- feat: `forward_event` is now fully async. The blocking `rt.block_on()` is
  replaced with `rt.spawn()`, correlating results back via `on_forward_done` /
  `on_forward_failed` callbacks and a `request_id` token. `ForwardRoomPicker`
  stays open during forwarding, shows an inline "Forwarding to N rooms…"
  status line, then either auto-closes on full success or displays per-room
  error lines with a Dismiss button. `ShellBase` tracks in-flight forwards in
  `pending_forwards_`. Adds 8 Catch2 tests for the handler state mutations.

- i18n: 11 strings introduced since v0.8.7 are now wrapped in `tk::tr()` /
  `tk::trf()` and registered in `tesseract.pot` and `es.po`. Covered strings:
  `ForwardRoomPicker` labels ("Forward message", "Forward", "Forward ({0})",
  "No rooms", "Unnamed room", "Cancel") and action-pill tooltip labels ("Add
  reaction", "Reply", "Reply in thread", "Edit", "More").

- perf(media): reqwest upgraded to 0.13 (was 0.12 for tile / URL-preview
  fetches, 0.13 via matrix-sdk for media). Both HTTP clients now share
  identical H2 window sizing (4 MiB/stream, 16 MiB/connection), 60 s timeout,
  and `build_user_agent()` string. `PriorityGate` fixed limits replaced with
  AIMD: additive-increase on a clean slot release (+1, up to max),
  multiplicative-decrease when >50% of active slots are stalled at release
  time (halved, floor = max/8). Starting limits raised 12/10 → 32/24 (fg/bulk)
  to suit HTTP/2 multiplexing.

- perf(media): media prefetch is now gated to the last 50 events
  (`kMediaPrefetchWindow`) on room switch. Previously `build_rows_()` called
  `prep_row_media_()` for every event in the SDK snapshot, producing hundreds
  of avatar/thumbnail/URL-preview HTTP requests for off-screen content.
  Pagination backfill no longer pre-fetches media; newly-visible rows trigger
  on-demand fetches via a new `ensure_row_media_()` overload called from
  `on_visible_rows_changed_()`.

- fix(timeline): `m.file` events now display the MSC2530 user caption below
  the file card, matching the existing behaviour for `m.image` and `m.video`.
  `filename` is set only when the sender supplied the MSC2530 `filename` field;
  `body` then carries the user-visible caption.

- fix(timeline): geometry maps (`image_geom_`, `video_geom_`, etc.) were
  unconditionally cleared at the start of every paint. During animation-tick
  partial repaints only the dirty animated region is redrawn, so rows outside
  it were skipped — leaving their hit-test entries permanently empty until the
  next full repaint. The "Load image" pill on suppressed-media tiles stopped
  responding whenever a spinner or GIF was animating. The clear is now guarded
  behind `!ctx.anim_damage`.

- fix(timeline): sender avatars could be evicted by the 30-minute thumbnail
  LRU TTL while the app sat idle, because `avatar_provider_` used `peek()`
  (raw pointer, `use_count() == 1`). `try_acquire_image_()` now holds an
  `ImageRef` for the sender avatar on every materialized row, and
  `notify_image_ready()` pins avatars on first network arrival without
  triggering a row-height relayout.

- fix(timeline): the animation-tick geometry-clear guard introduced to fix the
  partial-repaint issue above could leave stale `image_geom_` / `video_geom_`
  entries from the old room alive when an animation was in progress during a
  room switch, causing clicks to open media viewers for the wrong room.
  Hit-test geometry is now cleared eagerly in `begin_switch_loading()`
  alongside `messages_.clear()`, regardless of animation state.

- fix(compose): typing text between two semicolons with no matching emoji
  shortcode no longer causes infinite recursion in the shortcode-popup logic.

- fix(rooms): room title changes now trigger a live room-list update. The
  Rust sync fingerprint omitted `RoomInfo::name`, so a pure `m.room.name`
  state event produced an identical fingerprint and the snapshot emit was
  suppressed — the UI showed the stale title until an unrelated event (new
  message, read receipt) happened to perturb the fingerprint.

## v0.8.7 — 2026-06-18

### Summary

- feat(timeline): quoted/reply blocks now show a pointing-hand cursor on hover
- fix(macos): notifications for the active room are no longer suppressed when the app window is hidden
- fix(packaging): Arch PKGBUILD no longer fails with "cannot stat" errors. The manual `mv`/`sed` rename steps were redundant
- fix(privacy): remove machine hostname from MAS device display name and HTTP User-Agent

### Details

- fix(macos): notifications for the active room are no longer suppressed when
  the app window is hidden. The active-room suppression guard now requires the
  window to be visible (`winVisible && winFocused`); a hidden window could
  retain `isKeyWindow` during animations or while a sheet is attached, causing
  the notification and dock bounce to be silently dropped. The dock-bounce
  condition is extended from `winVisible && !winFocused` to `!winFocused` so
  the Dock receives an attention request even when the window is fully hidden.

- feat(timeline): quoted/reply blocks now show a pointing-hand cursor on hover,
  matching the visual feedback given by links and file cards. Clicking a quoted
  block scrolls to the original message.

- fix(packaging): Arch PKGBUILD no longer fails with "cannot stat" errors. The
  manual `mv`/`sed` rename steps were redundant — CMake's `install(CODE)` block
  already renames the binary, desktop file, and icon to `tesseract-matrix`
  during `cmake --install`.

- fix(privacy): remove machine hostname from MAS device display name and HTTP
  User-Agent. Device sessions now appear as "Tesseract on Linux/macOS/Windows"
  in the homeserver session list; the User-Agent is
  `Tesseract/<version> (<os>)`. Drops the `hostname` crate.

## v0.8.6 — 2026-06-17

Changes since v0.8.5:

### Summary

- feat(macos): the macOS dock icon shows the total notification count as a red badge aggregated across all accounts
- feat(win32): Win32 body font raised by 1 pt above the OS system font size
- chore(packaging): Debian changelog, Arch `PKGBUILD`
- fix(linux): drop `libavutil` / `libavutil-dev` dependency

### Details

- feat(macos): macOS dock icon now shows the total notification count as a red
  badge (aggregated across all signed-in accounts), kept in sync via the existing
  `notify_tray_unread_` call sites. Clicking the dock icon raises the window
  (including when it was hidden via the tray toggle) and navigates to the
  highest-priority unread room, matching the tray-icon click behaviour.

- feat(win32): Win32 body font raised by 1 pt above the OS system font size.
  `lfMessageFont` reports Segoe UI 9 pt; the new `kBodyFontPtOffset = 1` makes
  the body read at 10 pt, which is closer to the sizing used by modern chat
  clients. All other font roles derive additively from the body base, so the
  entire UI scales uniformly.

- fix(linux): drop `libavutil` / `libavutil-dev` dependency. `av_log_set_level`
  was a no-op once gst-libav installs its own `av_log` callback on GStreamer
  init; `GST_DEBUG=0` already silences all FFmpeg output routed through gst-libav.
  Removed the dead call, the `pkg_check_modules` wiring in both Linux
  `CMakeLists`, and `libavutil-dev` from the CI apt installs.

- chore(packaging): Debian changelog, Arch `PKGBUILD`, and `PACKAGING.md` are
  now generated by CMake `configure_file()` from `*.in` templates, so the
  package version automatically tracks the `project(VERSION …)` declaration
  in `CMakeLists.txt`. The `tesseract` (Qt6) and `tesseract-gtk` (GTK4) Debian
  packages now carry `Conflicts:` fields; Debian `Build-Depends` replaces
  `cargo`/`rustc` with `rustup`.

## v0.8.5 — 2026-06-17

Changes since v0.8.4:

### Summary

- feat: automatic GitHub release update checker
- feat(emoji): render Unicode emoji in message bodies at ~125% body font size (`FontRole::InlineEmoji`, `(base+1)×5/4` pt, minimum 6 pt)
- feat(fonts): inherit the system body font size on all four backends
- feat(logging): `sdk_log_level` key in `app_settings.json` controls the Rust tracing subscriber level (default `warn`)
- feat(sdk): upgrade matrix-rust-sdk 0.17 → 0.18
- perf(spaces): convert `get_space_child_summary` and `get_server_info` from blocking `block_on` to async FFI
- i18n: QR-code grant flow and upload-limit strings added to the translation table (`tesseract.pot`)
- refactor(macos): 144 of 159 `using`-block bridge entries replaced with explicit `MacShell` C++ API declarations
- fix(spaces): unjoined-room summaries are now fetched proactively on space entry rather than waiting for the room list to scroll a row into view
- fix(spaces): room-list scroll position and selection are restored when returning from a space drill-in
- fix(spaces): eliminated UI-thread `SH_FFI` lock acquisitions that blocked on worker `block_on` calls and caused visible freezes
- fix: crash and hide-instead-of-quit when quitting with a pop-out window open
- fix(win32): fixed slower-than-real-time video playback and a crash-on-paint race in the Win32 video renderer
- fix(compose): pasting rich text from the clipboard into the composer on Qt6 now inserts plain text only
- fix(compose): composer input font now matches message body text (`FontRole::Body`, i.e. system base size) instead of a hardcoded pixel size
- fix: add `FontRole::Caption` for section headers and reply preview text, and correct the `font_role_pt` offsets back to their legacy values
- fix(gtk4): emoji and sticker pickers now initialise at widget construction so they are ready on the first key-press rather than on first explicit open
- fix(qt6): font cache index-mapping assertion eliminated
- fix: hyperlinks inside block-structured messages (headings, lists, blockquotes) are now clickable; the block parser was dropping `href` attributes
- fix: code-block background left-alignment on Qt6; leading whitespace stripped in `html_to_blocks` so backgrounds start at the correct indent
- fix: edit-banner height in `ComposeBar` reduced to a single text row

### Details

#### 2026-06-17

- perf(spaces): convert `get_space_child_summary` and `get_server_info` network
  calls from blocking Rust `block_on` (which pins a C++ worker thread for the
  full HTTP round-trip) to async FFI. Rust spawns tokio tasks that run HTTP on
  Rust's own thread pool and deliver results via `EventHandlerBridge` callback;
  no C++ thread is pinned during the wait. With the previous design, four
  concurrent 30-second-timeout summary fetches could saturate the 4-thread pool
  and make the entire client unresponsive. The worker pool is reduced from 4 → 2
  threads; it now only serves fast synchronous SQLite reads. `RoomSummary::from_json`
  added to deserialise callback payloads. 870 C++ tests.

#### 2026-06-16

- fix(spaces): unjoined-room summaries are now fetched proactively on space
  entry rather than waiting for the room list to scroll a row into view.
  Stub rows whose summary has not yet arrived are hidden rather than shown as
  empty entries; cancelled fetches (on space exit or account switch) are
  tracked by a generation counter so late callbacks are discarded.

- fix(spaces): room-list scroll position and selection are restored when
  returning from a space drill-in. macOS pagination spinner no longer stalls
  after the first page.

- fix(spaces): eliminated UI-thread `SH_FFI` lock acquisitions that blocked
  on concurrent worker-thread `block_on` calls, causing visible freezes while
  space summaries were loading.

- feat: automatic GitHub release update checker. Runs at startup and on a
  timer; displays a banner when a newer release is available. A "Check for
  updates automatically" toggle in Settings → Privacy controls whether the
  check fires at all.

- feat(emoji): render Unicode emoji in message bodies at ~125% body font size
  (`FontRole::InlineEmoji`, `(base+1)×5/4` pt, minimum 6 pt). Emoji-only
  captions under images, stickers, and other media render at 2× body size
  (`FontRole::BigEmoji`) rather than body text size.

- feat(fonts): inherit the system body font size on all four backends — Qt6
  `QApplication::font()`, GTK4 `GtkSettings gtk-font-name`, Win32
  `SystemParametersInfo NONCLIENTMETRICS`, macOS `NSFont.systemFontSize`. All
  per-role sizes are additive offsets from the live base, so the UI scales
  with the user's accessibility font-size setting.

- feat(logging): `sdk_log_level` key in `app_settings.json` controls the Rust
  tracing subscriber level (default `warn`). Noisy log sources suppressed by
  default in release builds: `matrix_sdk::http_client` tracing, Qt multimedia
  FFmpeg/PipeWire SPA warnings, and FFmpeg `av_log` output.

- feat(sdk): upgrade matrix-rust-sdk 0.17 → 0.18.

- fix: crash and hide-instead-of-quit when quitting with a pop-out window open.
  `ShellBase` sets a `tearing_down_` flag on destruction to suppress virtual
  callbacks from the debounce timer; `MainWindow` tracks `explicitly_quitting_`
  to bypass the hide-to-tray behaviour that normally intercepts the close event.

- fix(win32): video playback was running slower than real-time due to a frame
  pacing bug; also fixed a crash-on-paint race condition in the Win32 video
  renderer.

- fix(compose): pasting rich text from the clipboard into the composer on Qt6
  now inserts plain text only, preserving the room's own formatting pipeline.

- fix(compose): composer input font now matches message body text (`FontRole::Body`,
  i.e. system base size) instead of a hardcoded pixel size.

- fix: add `FontRole::Caption` for section headers and reply-to preview text;
  correct all `font_role_pt` offsets back to the legacy hardcoded values (a
  prior commit had shifted every role +1 pt incorrectly).

- fix(gtk4): emoji and sticker pickers now initialise at widget construction so
  they are ready on the first key-press rather than on first explicit open.

- fix(qt6): font cache index-mapping assertion eliminated.

- fix: hyperlinks inside block-structured messages (headings, lists, blockquotes)
  are now clickable; the block parser was dropping `href` attributes.

- fix: code-block background left-alignment on Qt6; leading whitespace stripped
  in `html_to_blocks` so backgrounds start at the correct indent.

- fix: edit-banner height in `ComposeBar` reduced to a single text row.

- i18n: QR-code grant flow and upload-limit strings added to the translation
  table (`tesseract.pot`).

- refactor(macos): 144 of 159 `using`-block bridge entries replaced with
  explicit `MacShell` C++ API declarations, reducing ObjC++ coupling.

## v0.8.4 — 2026-06-15

Changes since v0.8.3:

### Summary

- feat(spaces): persist MSC3266 space-child summaries to `app_cache.db` for instant display
- feat(msc4108): gate the "Sign in with QR code" UI on server capability
- feat(profile): implement MSC4133 extended user profiles
- feat(sdk): enable HTTP/2 multiplexing for media downloads
- feat(spaces): unjoined rooms section + `RoomPreviewView` across all four shells
- feat(sdk): extend `InFlightGuard` RAII coverage to all non-sync HTTP calls
- feat(statusbar): animate the in-flight indicator as a spinning ring instead of a static dot
- feat(debug): label every `InFlightGuard` with a human-readable operation name, shown in the status-bar spinner tooltip
- feat(login): MSC4108 QR-code login
- feat(markdown): block-level rendering in sent and received messages
- feat(timeline): block scroll and show a deferred scrim + spinner during historical event navigation
- feat(media): persist exponential backoff state for failed media fetches across sessions
- feat(tk): make `CheckButton` text `FontRole` configurable
- i18n: wrap all previously untranslated user-facing strings and regenerate the `.pot` template
- fix(pickers): emoji shortcode tooltip no longer vanishes immediately on hover
- fix(rooms): lazy-load unjoined space-child avatars from paint instead of eagerly on space navigation
- fix(rooms): replace the all-at-once MSC3266 batch fetch with lazy per-room summary fetching and per-room exponential backoff
- fix(preview): `RoomPreviewView` now hides the compose-bar native-text overlay while open
- fix(timeline): snap to the live bottom when returning from a historical view
- fix(popout): restore full secondary-window functionality

### Details

#### 2026-06-15

- feat(spaces): persist MSC3266 space-child summaries to `app_cache.db` for
  instant display. Fetched summaries are written to the per-account SQLite
  cache on arrival and reloaded on startup, so the "Not joined" section
  populates from disk before any network round-trips complete; live fetches
  still update both the cache and the view.

- feat(msc4108): gate the "Sign in with QR code" UI on server capability.
  The login-view button is shown only when the homeserver advertises the
  required `m.login.token` / device-authorization grant flows, so the option
  never appears on servers that don't support MSC4108.

- fix(pickers): emoji shortcode tooltip no longer vanishes immediately on
  hover. `TabbedGridPicker` rebuilt its hover-tracking state on every paint,
  clearing the hovered index before the tooltip timer could fire; replaced
  with a stable tracked index so tooltips appear and persist correctly.

#### 2026-06-14

- feat(profile): implement MSC4133 extended user profiles. Three new profile
  fields are exposed in the account settings panel and the user-profile info
  panel: **Pronouns** (`io.fsky.nyx.pronouns`), **Timezone**
  (`us.cloke.msc4175.tz`), and **Biography** (`gay.fomx.biography`). Each uses
  a stable read key (`uk.tcpip.msc4133.<field>`) with an unstable write key;
  the implementation reads stable then falls back to unstable, and always writes
  stable. A `get_extended_profile` / `set_extended_profile` FFI pair wraps
  matrix-sdk's `get_raw_account_data_event` / `set_account_data`; a re-fetch
  after write keeps the UI in sync without stale data. `AccountSection` and
  `UserProfilePanel` each gain three `NativeTextField` rows with a loading /
  saving state. 283 Rust + 861 C++ tests.

- feat(sdk): enable HTTP/2 multiplexing for media downloads. The reqwest media
  client is configured with `.http2_prior_knowledge()` so parallel MXC downloads
  can share a single TCP connection. `MEDIA_BULK_PERMITS` is raised from 6 to 10
  to take advantage of the extra bandwidth, reducing connection overhead on a
  busy room open.

- fix(rooms): lazy-load unjoined space-child avatars from paint instead of
  eagerly on space navigation. The same `on_room_avatar_needed` / `paint_row`
  callback path used for joined rooms now drives unjoined-room-row avatar
  fetches, so navigating into a large space no longer fires 500+ avatar requests
  immediately on fetch.

- feat(spaces): unjoined rooms section + `RoomPreviewView` across all four shells.
  When navigating into a space, a collapsible **"Not joined"** section appears
  below the joined rooms, listing every space-child room the user hasn't joined.
  Clicking an unjoined row opens `RoomPreviewView` — a right-side panel showing
  the room name, avatar, topic, member count, and a **Join** button — without
  changing the active room. The feature is built across the full stack: a new
  `space_children_all()` Rust SDK function (with `space_children()` refactored
  to delegate to it) returns both joined and unjoined direct children; MSC3266
  room summaries are fetched concurrently via `join_all` with `InFlightGuard`
  RAII and a generation counter that cancels in-flight requests when the space
  changes; `RoomListView` renders the new `kSecSpaceUnjoined` section
  (collapsible, setting `room_section_space_unjoined_collapsed`); `MainAppWidget`
  gains `show_room_preview` / `hide_room_preview` virtual slots; `RoomPreviewView`
  is a new `ui/shared/views/` widget wired in all four shells (Qt6, GTK4, Win32,
  macOS).

- feat(sdk): extend `InFlightGuard` RAII coverage to all non-sync HTTP calls.
  Previously only a subset of operations incremented the in-flight count; now 71
  SDK operations across room_list, account, send, verification, recovery,
  image_packs, pins, tags, and timeline modules all carry an `InFlightGuard` so
  the status-bar indicator accurately tracks every in-flight Matrix API call.

- feat(statusbar): animate the in-flight indicator as a spinning ring instead of
  a static dot. An animated ring (set of small dots on a 16 px orbit animating
  at a constant angular velocity) replaces the coloured dot; color thresholds are
  unchanged (green ≤ 1, amber ≤ 10, red > 10). Shared draw logic lives in a new
  `tk::draw_inflight_indicator` helper in `tk/inflight_dot.h`; each platform
  backend wraps it in a canvas widget (`InflightDotWidget` on Qt6; analogous
  implementations on GTK4, Win32, and macOS).

- feat(debug): label every `InFlightGuard` with a short human-readable operation
  name; hovering the status-bar spinner shows a tooltip listing all currently
  in-flight operations by name (e.g. "upload media", "paginate backward",
  "fetch profile"), making it easy to identify slow or stuck requests.

- fix(rooms): replace the all-at-once MSC3266 batch fetch with lazy per-room
  summary fetching and per-room exponential backoff. The unjoined space-child
  section now requests one room's summary at a time as rows scroll into view;
  the previous batch used a single concurrent `join_all` with no escape hatch,
  so one slow child blocked the whole section — replaced with a single-room
  `get_space_child_summary` call that fails fast and retries with backoff.

- fix(preview): `RoomPreviewView` now hides the compose-bar native-text overlay
  while open, and wraps a long room topic in a scrollable container so it
  doesn't overflow the panel.

- feat(login): MSC4108 QR-code login — an existing logged-in device can scan
  a QR code displayed on a new device to grant it a session without re-entering
  credentials. A new `QRGrantView` shared widget shows the QR code and walks
  through the confirm step; `ShellBase` runs the Rust `qr_grant` state machine
  (`check_qr_grant_state` polling, `on_qr_grant_state` callbacks). Wired on all
  four shells (Qt6, GTK4, Win32, macOS).

- i18n: all previously untranslated user-facing strings are now wrapped in the
  appropriate i18n helper and the `.pot` template is regenerated (~310 → ~520
  translatable entries). Affected surfaces include the shared views, all four
  platform shells, and the settings / login / search / compose areas.

#### 2026-06-13

- feat(markdown): block-level rendering in sent and received messages. Headings
  (`#` through `######`), unordered and ordered lists (including nested),
  blockquotes, and tables now render visually in `MessageListView` across all
  four canvas backends. Headings use a heavier weight (`FontRole::UiSemibold`);
  list items indent with correct bullet / ordinal; blockquotes get an accent
  left-border stripe; tables lay out in a fixed-width columnar grid. Complements
  the existing inline styles (bold, italic, code, strikethrough) and syntax
  highlighting.

- feat(timeline): block scroll and show a deferred scrim + spinner during
  historical event navigation. When the user jumps to a historical event
  (jump-to-date, event permalink, pinned message) the timeline freezes scroll
  input and overlays a translucent scrim with a centered spinner until the SDK
  delivers the focused event; the scrim then fades and scroll unlocks. Prevents
  inadvertent position changes during the async load.

- feat(media): persist exponential backoff state for failed media fetches across
  sessions. The `media_fetch_failed_` in-memory backoff cache (30 s → 30 min) is
  now serialised to the per-account `app_cache.db` SQLite so a network error at
  end of session doesn't cause a flood of re-requests on the next startup.
  Cleared on cache-wipe and on a successful fetch as before.

- fix(timeline): snap to the live bottom when returning from a historical view.
  After navigating away from the live end (jump-to-date, event permalink) and
  dismissing the historical context, the timeline now scrolls to the current
  bottom of the live feed instead of staying anchored at the historical position.

- fix(popout): restore full secondary-window functionality — thread list, thread
  open, in-room search (Ctrl+F), and jump-to-date now all work inside pop-out
  room windows. Missing wiring in `RoomWindowBase` prevented the shared
  `RoomView`-level features from reaching the pop-out's room context.

- feat(tk): make `CheckButton` text `FontRole` configurable. A new
  `set_font_role(FontRole)` API lets callers override the default label weight /
  size; used by Settings checkboxes to match the surrounding body text.

## v0.8.3 — 2026-06-13

Changes since v0.8.0:

### Summary

- feat(search): in-room find-in-conversation search bar
- feat(ui): replace per-platform jump-to-date dialogs with a single shared `DatePickerView`
- feat(search): show on-disk search index size in Settings
- feat(search): show live index stats under the Settings toggle
- feat(search): full-text message search across all rooms, including encrypted
- feat(switcher): start a DM by mxid from the quick switcher
- feat(matrix-uri): navigate to the event from a `matrix.to` / `matrix:` permalink
- feat(media): prioritize visible-row downloads and never freeze on stuck fetches
- feat(room-list): highlight rooms with unread messages but no notification
- refactor(search): search index moved to its own `search_index.db`
- perf(timeline): batch FFI events to eliminate per-message redraws during pagination
- perf(room-switch): perceived-latency pass on opening a room
- perf(room-switch): warm timelines + bounded subscriptions
- perf(room-switch): consolidate the four shells' duplicated subscribe/paginate workers into one shared `ShellBase::start_room_subscription_`
- refactor/fix(ffi): eliminate the room-switch UI freeze
- refactor(shell): hoist account restore, login/logout, account-switch, tray aggregation and sync-error handling into shared `ShellBase` methods
- refactor: extract shared widgets/helpers from the toolkit and views
- refactor(sdk): collapse the eight near-identical media-send paths and simplify `TimelineEvent` construction
- refactor(client): fold the 36 event-bridge preambles into a `with_handler` wrapper and parse SDK JSON with `nlohmann`
- refactor(views,app): begin decomposing the `MessageListView` (7219 → 6120 LOC) and `ShellBase` god-objects
- chore: remove dead code across every module and clear the auto-fixable clippy backlog
- fix(html): collapse raw whitespace in `formatted_body` text nodes
- fix(ui): reaction chip key text changed from `FontRole::Title` to `FontRole::Body` so long text reaction keys aren't bold
- fix(search): search card no longer shrinks while the user is typing
- fix(win32): pump the STA message queue while joining worker threads on shutdown
- fix(search): search results now appear immediately
- fix(ui): remove `TextHAlign::Center` from unbounded empty-state layouts on Qt
- fix(search): Settings toggle now correctly relayouts after toggling the search index
- fix(search): address nine code-review findings
- fix(macos): build fixed after search additions
- fix(voice): play voice / audio messages on a single click
- fix(multi-window): Ctrl+click on the account picker re-ran the primary window's full startup in the spawned window
- fix(shell): route the Windows and macOS shells through the shared `ShellBase` timeline/message handlers
- fix(shell): close a multi-window/logout use-after-free
- fix(win): unsubscribe the previous account's open room on account switch (was leaking the subscription, so the old account kept streaming)
- fix(client): serialize all `Client` FFI access under `ffi_mu`
- fix(sdk): switch to `parking_lot` mutexes/rwlocks so a lock panic can't poison the lock and cascade across the cxx FFI boundary
- fix(client): survive a wrong-typed field in `app_settings.json` instead of crashing at startup
- fix(views): clamp invalid numeric HTML entities to U+FFFD and fix a formatting-stack underflow under the tag-depth cap
- fix(win): persist the save-time DPI in `WindowGeometry` and rescale window geometry on restore

### Details

#### 2026-06-13

- feat(search): in-room find-in-conversation search bar. **Ctrl+F** (Win32 /
  Qt6 / GTK4) and **⌘F** (macOS) opens a `RoomSearchBar` anchored below the
  room header. Results highlight matching rows in the timeline with a tinted
  accent overlay and arrow-key navigation (↑ / ↓ wraps) jumps to each hit.
  A **Paginate** checkbox automatically back-paginates when no more matches
  exist in the current window, fetching older history in a loop until a match
  is found or the start of the timeline is reached. The bar closes on Esc;
  the status bar confirms fetch progress. Shared code in `ui/shared/views/`
  (`RoomSearchBar`, `MessageListView::set_search_matches` /
  `clear_search_matches`); wired in all four shells. 832 C++ tests.
- fix(html): collapse raw whitespace in `formatted_body` text nodes. Literal
  newlines and runs of spaces between inline elements in HTML-formatted messages
  now collapse to a single space (CSS `white-space: normal` semantics), matching
  browser and Element rendering. Entity-encoded characters (`&#10;`, `&#9;`) and
  explicit line breaks (`<br>`, `<p>`) are unaffected.
- fix(ui): reaction chip key text changed from `FontRole::Title` (15 pt
  semibold) to `FontRole::Body` (13 pt regular) so long text-based reaction
  keys (e.g. `● update-go-modules-graph`) no longer appear bold. Chip
  dimensions are unchanged (height is driven by a settings constant, not the
  font size).
- refactor(search): search index moved to its own `search_index.db`. The FTS5
  tables (`message_index`, `message_fts`, `search_state`) now live in a
  dedicated per-account `search_index.db` instead of sharing `app_cache.db`
  with the unrelated `backfill_ts` table. Each database has a single
  responsibility and can be cleared independently. `clear_caches` and `logout`
  close and delete both files; existing `app_cache.db` files are untouched (old
  search tables become inert dead weight until manually pruned). Purely internal
  Rust — no FFI or C++ changes.
- fix(search): search card no longer shrinks while the user is typing. All four
  shells call `relayout()` immediately after `set_query()`, which cleared results
  and caused `arrange()` to compute the minimum card height before the next
  search response arrived. The card now tracks its peak height
  (`max_card_h_`) since the overlay was opened and only ever grows.
- perf(timeline): batch FFI events to eliminate per-message redraws during
  pagination. The Rust SDK previously emitted one `VectorDiff` per message; each
  crossed the FFI boundary individually and posted a separate layout pass. An
  `EmitOp` accumulator now collects a full `stream.next()` poll before any FFI
  crossing, coalescing runs of same-kind ops into five new bridge callbacks:
  `on_messages_prepended`, `on_messages_appended`, `on_messages_updated_batch`,
  `on_thread_messages_prepended`, `on_thread_messages_appended`. `MessageListView`
  adds `prepend_messages`/`append_messages` bulk-insert paths and drops the old
  `pending_prepends_` buffer (batching now happens in Rust). One layout pass per
  pagination response instead of one per message.
- fix(win32): pump the STA message queue while joining worker threads on
  shutdown. Worker threads call `decode_image_()` which uses the WIC
  `IWICImagingFactory` stored in the D2D backend — that factory is STA-bound and
  some WIC codec operations dispatch back to the STA via the message queue. A
  plain `join()` in `drain()` blocked the STA pump, causing deadlock.
  `MainWindow::on_destroy()` now uses `CoWaitForMultipleHandles` with
  `COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES` to keep the pump
  alive until each thread finishes.
- fix(search): search results now appear immediately. `handle_search_results_ui_`
  and `handle_search_failed_ui_` called `set_results()` but never requested a
  relayout, so the results card stayed invisible until the user pressed Up/Down.
  `schedule_relayout_()` now runs after every result update.
- fix(ui): remove `TextHAlign::Center` from unbounded empty-state layouts on Qt.
  Qt's QPainter backend clamps `max_width=-1` to 8192 px; with center alignment
  this shifted the draw origin ~4000 px off-screen. `MessageSearchView` and
  `QuickSwitcher` both computed a centered origin manually, so removing
  `halign=Center` is the complete fix.
- feat(ui): replace per-platform jump-to-date dialogs with a single shared
  `DatePickerView`. Removes four native date pickers (Win32 `MonthCal`, GTK4
  `gtk_calendar`, Qt6 `QCalendarWidget`, macOS `NSDatePicker`) and replaces them
  with a shared widget in `ui/shared/views/`. The picker registers as a
  `tk::Host` popup, handles pointer and wheel input through the shared dispatch
  layer, and supports scroll-wheel year navigation (wheel over the year token
  navigates years; elsewhere navigates months). `ShellBase::handle_date_jump_()`
  consolidates the previously duplicated `timestamp_to_event` +
  `subscribe_room_at` logic.
- fix(search): Settings toggle now correctly relayouts after toggling the search
  index. The `on_change` handler showed/hid stats labels synchronously but the
  surface only repainted (not relayouted) on pointer events, so newly-visible
  labels had unset bounds and painted at the window origin.
- feat(search): show on-disk search index size in Settings. A new
  `search_index_size_bytes` FFI call queries the SQLite `dbstat` virtual table
  to measure the space used by `message_index`, `message_fts*`, and
  `search_state`. Called once when Settings opens and cached in
  `cached_index_bytes_`; the result is rendered as "· ~1.2 MB" alongside the
  message/room count.
- feat(search): show live index stats under the Settings toggle. While the
  Privacy → Search toggle is enabled, a summary line shows the message and room
  count and the oldest indexed timestamp ("12,431 messages across 38 rooms ·
  indexing…" → "· up to date" once the backfill completes). The shell polls
  every 2 s while Settings is open and the backfill is still running, then
  stops. Stats come from a new synchronous `search_index_stats()` FFI backed by
  a single aggregate scan in `search::stats()`.
- fix(search): nine code-review findings addressed: GTK Ctrl+Shift+F keyval
  (`GDK_KEY_F` → `GDK_KEY_f`); backfill now skips the foreground room;
  index-toggle SQLite work moved off the UI thread; backfill-resume marker
  (`backfill_done`) set only after a complete crawl; account-switch clears
  in-flight search state; `Set(true, None)` diff removes the old index entry;
  `relative_time` year-boundary fix; debounce search queries at 120 ms;
  room display names resolved C++-side from the cached room list.
- fix(macos): build fixed after search additions.

#### 2026-06-11

- feat(search): full-text message search across all rooms, including encrypted.
  A global search overlay (**Ctrl+Shift+F** / **⌘⇧F**) backed by a local SQLite
  FTS5 index of decrypted message bodies — the only approach that can search E2EE
  history, since the Matrix server-side `/search` endpoint never sees plaintext.
  Indexing is **opt-in** (Settings → Privacy → Search, off by default).  Enabling
  lazily backfills history per joined room; disabling clears the index.  Population
  is incremental: every message is indexed on the live timeline-diff and pagination
  paths plus a one-time background history backfill on first enable.  The overlay
  (`ui/shared/views/MessageSearchView`) reuses the QuickSwitcher pattern; results
  show room · sender · snippet; clicking jumps to the message via the existing
  event-permalink scroll/focus path.  New FFI: `Client::search_messages` /
  `set_search_indexing_enabled`, `SearchHit`, `on_search_results` /
  `on_search_failed`.  Shared code in `ShellBase` / `EventHandlerBase`; wired in
  all four shells (Qt6, GTK4, Win32, macOS).  11 new Rust unit tests.  Verified
  on Qt6.  *(In-room find bar (Ctrl+F) planned as follow-up.)*

- perf(room-switch): perceived-latency pass on opening a room. `subscribe_room`
  no longer emits an empty `on_timeline_reset` before the populated one; the UI
  instead clears the previous room's rows the instant the user clicks
  (`MessageListView::begin_switch_loading`) and shows a clean loading view — a
  centered spinner only if the load outlasts ~500ms, so warm/fast switches show
  nothing transient and the old room never lingers under the new header. The
  display gate (`RoomSwitchGateKeeper`) reserves media height from intrinsic
  `media_w`/`media_h` instead of blocking on image/video decode, so the list
  reveals on text-ready (timeout 400→150ms); the content-addressed body-layout
  cache is retained across switches (no per-switch `link_cache_` clear) so
  returning to a room reuses shaped text; and the per-switch blocking
  `load_prefs_json()` (`rt.block_on` on the UI thread) is gone — the room layout
  is rebuilt from in-memory state via a new `Prefs::room_layout` and saved on the
  async path. New Catch2 suite `test_room_switch_gate` + added prefs / layout-cache
  / loading-state cases.
- perf(room-switch): warm timelines + bounded subscriptions. `subscribe_room`
  reuses a still-live, non-focused timeline (restart its streaming task to
  re-emit the current items) instead of dropping and rebuilding, so returning to
  a recently-viewed room is instant. A bounded warm-subscription LRU
  (`ShellBase::prune_warm_subscriptions_`) keeps the active room + open tabs +
  pop-out-pinned + the newest few visited rooms and unsubscribes the rest,
  capping the previously-unbounded growth of live timelines / sliding-sync
  subscriptions over a long session. New `test_shell_warm_subscriptions`.
- perf(room-switch): consolidate the four shells' duplicated subscribe/paginate
  workers into one shared `ShellBase::start_room_subscription_`. `subscribe_room`
  runs on the single-thread mut pool and is dispatched on every switch (it emits
  the reset that repopulates the view), while the blocking network back-pagination
  moves to the shared pool so it never holds the one mut thread and blocks the
  next switch's reset — fixing a spurious loading spinner on rapid A↔B switching,
  and removing the `in_flight` guard that could skip the reset-producing subscribe
  entirely. Adds an O(1) `ShellBase::room_by_id_` index replacing the per-switch
  O(n_rooms) scans (space-detection, `set_room`, tab-bar metadata). All shared
  code; wired in all four shells (Qt6, GTK4, Win32, macOS) and verified on each.
  813 C++ + 226 Rust tests.

#### 2026-06-10

- feat(switcher): start a DM by mxid from the quick switcher. Typing a leading
  `@` flips the Ctrl/⌘+K switcher into a user-search mode that live-filters a
  roster of known users (DM partners + members of joined rooms, built lazily on
  a worker thread and cached per account in `ShellBase`). Typing a full
  `@user:server` debounce-resolves the profile via a new
  `Client::resolve_user_profile` FFI (matrix-sdk `fetch_user_profile_of`) to
  confirm the user exists before offering the row; activating it opens/creates
  the DM through the existing `handle_open_dm_` path. `@`-detection and
  rendering live entirely in `views/QuickSwitcher` + `ShellBase`, so no platform
  shell changed. Adds 8 C++ tests (734 → 742).
- feat(matrix-uri): navigate to the event from a `matrix.to` / `matrix:`
  permalink. Event permalinks parsed end-to-end but dispatch was a stub — it
  searched only the loaded timeline window and silently failed when the event
  wasn't already loaded. The `Kind::Event` case now routes through the existing
  `try_scroll_to_room_event_` path (deferred scroll + focused
  `subscribe_room_at` / `/context` fallback, the same machinery reply / pinned /
  thread jumps use) and highlights the target row. An event link to a not-yet-
  joined room stashes the event id and consumes it in the Join completion so the
  timeline jumps to the highlighted event once the join lands. Shared-layer only
  (`ShellBase`); parsing already covered by the `matrix_uri` Rust tests.
- refactor/fix(ffi): eliminate the room-switch UI freeze. ~60 `ClientFfi` bridge
  methods took `&mut self`, forcing the C++ `Client` to serialise every FFI call
  behind one coarse `std::mutex` held across blocking `block_on`s — so a worker
  mid-`subscribe_room` (timeline build) froze the UI thread the instant it did a
  cheap read during a room switch. Converted the read / lightweight-dispatch
  methods to `&self` (moving `thread_lists` / `thread_timelines` behind
  `parking_lot::RwLock` and scoping every map guard so none is held across a
  `block_on`), then replaced `ffi_mu` with a `std::shared_mutex`: readers take a
  shared lock (`SH_FFI`), the ~15 genuine writers (`start_sync`,
  `restore_session`, `logout`, `oauth_*`, …) keep the exclusive lock
  (`MUT_FFI`). The UI thread's room-switch reads now run concurrently with a
  worker's timeline build instead of freezing behind it.
- fix(voice): play voice / audio messages on a single click. A play click on a
  clip whose bytes weren't cached yet returned early (it only kicked off a
  background fetch + repaint), so the user had to click twice. The cold-cache
  click now arms a pending play and retries it from `MessageListView::arrange()`
  — the relayout the fetch's `on_ready` already drives — so playback starts on
  the first click; cleared on room switch. Shared-layer only. Adds 4 C++ tests
  (742 → 746).
- feat(media): prioritize visible-row downloads and never freeze on stuck
  fetches. The room timeline eagerly enqueues *every* row's media at once and
  the SDK drained it through a FIFO-fair `tokio::Semaphore`, so the media for
  the rows the user is actually looking at queued behind the off-screen backlog
  and could not be reordered. Replaced the per-lane semaphore with a
  `PriorityGate` over a pure `MediaQueue` (priority desc, then FIFO seq);
  `fetch_media_async` gained a priority and a new `prioritize_media(group, ids)`
  raises still-parked fetches so a just-scrolled-to row jumps the queue. A
  `MessageListView::on_visible_range_changed` callback (frame-coalesced,
  de-duped), re-exposed via `RoomView` and bound once in the shared
  `wire_main_app_widget_`, drives it across all four shells + the thread panel.
  Stall reclamation: a slot held past an 8 s deadline stops counting against the
  lane limit (matrix-sdk media is an opaque await with no progress hook), so the
  queue keeps flowing past stuck downloads while they drain in the background; a
  hard ceiling (2× the lane) bounds total in-flight. Adds 12 Rust + 5 C++ tests
  (208 → 220 Rust, 746 → 751 C++).
- feat(room-list): highlight rooms with unread messages but no notification.
  Rooms whose activity doesn't notify (set to "mentions only", or any room whose
  push rules don't notify) had no indication at all because every unread
  affordance keyed on `notification_count`. `RoomInfo` now also carries
  `unread_count` (`Room::num_unread_messages()`) and `muted`
  (`cached_user_defined_notification_mode`); a new header-only
  `unread_style_for()` maps the counts + mute state to `{None, Dot, Count,
  Mention}`, and `RoomListView` renders a **bold name + small neutral dot** for
  quiet unread (mirrored on a collapsed section header). Muted rooms are
  excluded. Also fixed `room_list_fingerprint` — the room-list update-dedup gate
  encoded only notification state, so read-clearing a quiet room left the dot
  stuck — to include the quiet-unread state. Adds 2 Rust + 5 C++ tests
  (220 → 222 Rust, 751 → 756 C++).

#### 2026-06-09

Pre-launch hardening, cross-platform deduplication, and the start of a
god-object decomposition — driven by a full-tree code review
(`docs/CODE_REVIEW_2026-06-09.md`). Test suite grew 703 → 734 C++ (Catch2 /
ctest) and 204 → 208 Rust. Remaining decomposition work is tracked in
`docs/TODO-phase5-remaining.md`.

- fix(multi-window): Ctrl+click on the account picker re-ran the primary
  window's full startup in the spawned window, re-restoring every account —
  so the account picker showed each account twice and the new window opened on
  the wrong account. Gate startup behind a shared
  `ShellBase::is_secondary_window_startup_()` predicate (manager already
  populated + account pinned + no client bound yet); a spawned window now binds
  the pinned account without touching disk. Added a duplicate-`user_id` guard
  to `AccountManager::add_account`, and deferred the Win32 `start_login()` to
  the message loop so `set_initial_account()` lands first.
- fix(shell): route the Windows and macOS shells through the shared
  `ShellBase` timeline/message handlers (delete their redundant overrides) —
  fixes in-thread replies leaking into the main timeline and lost scroll/focus
  restore on room-switch, which only those two shells exhibited.
- fix(shell): close a multi-window/logout use-after-free — guard every
  worker→UI continuation with a `ShellBase::alive_` liveness token, capture the
  active `AccountSession` (not the raw `Client*`) in worker bodies, and pass the
  `Client` explicitly to `RoomWindowBase::fetch_source_bytes_`.
- fix(win): unsubscribe the previous account's open room on account switch
  (was leaking the subscription, so the old account kept streaming).
- fix(client): serialize all `Client` FFI access under `ffi_mu` — the
  unlocked read/async wrappers raced the `&mut self` writers on the worker
  pools (aliasing UB); the unlocked `list_room_threads` HashMap read was the
  one provable data race.
- fix(sdk): switch to `parking_lot` mutexes/rwlocks so a panic while holding a
  lock can't poison it and cascade a panic across the cxx FFI boundary (UB);
  cap the notification sender- and room-avatar downloads at `NOTIF_IMAGE_CAP`;
  return errors instead of `expect()`-panicking on URL parse in OAuth.
- fix(client): survive a wrong-typed field in `app_settings.json` (catch
  `json::exception`, fall back to defaults) instead of crashing at startup;
  detect a corrupt `accounts.json` (quarantine it, don't silently drop every
  account); offload session persistence off the SDK sync-callback thread.
- fix(views): clamp invalid numeric HTML entities (surrogates / NUL /
  > U+10FFFF) to U+FFFD and fix a formatting-stack underflow under the tag-depth
  cap; route popup click/hover and outside-click dismiss in the GTK4/Win32/macOS
  `tk::Host` backends (previously Qt-only, so `ComboBox` dropdowns were broken
  on the other three).
- fix(win): persist the save-time DPI in `WindowGeometry` and rescale window
  geometry on restore, so a window saved on a HiDPI display is no longer mis-
  sized when restored at a different scale (e.g. a Remote Desktop session).
- refactor(shell): hoist account restore / login-finalize / account-switch /
  logout / tray aggregation / sync-error handling / slash-command dispatch /
  settings-controller construction / OAuth temp-dir setup into shared
  `ShellBase` methods, removing ~1,250 LOC of duplication across the four native
  shells and a class of drift bugs (e.g. macOS soft-logout showed a stale
  display name; macOS account-switch leaked per-room state).
- refactor: extract shared widgets/helpers from the toolkit and views —
  `ListPopupBase` (Mention/Shortcode/SlashCommand popups), `MediaOverlayBase`
  (image/video viewers), `ScrollableBase` (ListView/GridView), `TabbedGridPicker`
  (emoji/sticker pickers), `Host::dispatch_pointer_*` (the four hosts' pointer
  state machine + popup routing), `media_utils::draw_avatar` (~16 call sites),
  and shared canvas `FontRole`-weight + `initials_of` helpers.
- refactor(sdk): collapse the eight near-identical media-send paths via
  `build_attachment_config`/`do_send_attachment`; replace the 29-element
  positional `TimelineEvent` tuple with partial-struct construction; decompose
  the ~1,000-line `start_sync` into named watcher/handler helpers.
- refactor(client): fold the 36 event-bridge `guard + load + null-check`
  preambles into a `with_handler` wrapper; parse SDK JSON with `nlohmann`
  instead of two hand-rolled scanners (incl. bespoke UTF-16 surrogate decoding).
- refactor(views,app): begin decomposing the `MessageListView` (7219 → 6120
  LOC) and `ShellBase` god-objects — extract `TimelineMediaController`
  (voice+audio playback), `SpoilerRevealer`, `ReadReceiptTracker`,
  `LocationMapPanner` (also clears its geometry each paint, fixing an unbounded
  `map_rect_geom_` leak), `TimelineVideoPlaylist`, `RoomSwitchGateKeeper`,
  `UrlPreviewCardDisplay`, `LinkLayoutCache`, and `ThreadPanelController`.
- chore: remove dead code across every module (blocking-variant `Client` APIs
  + their FFI bindings, unused widgets/setters/fields/virtuals, a
  self-silencing Rust import shim) and clear the auto-fixable clippy backlog.

## v0.8.0 — 2026-06-07

Changes since v0.1.10:

### Summary

- feat(ui): replace Unicode glyphs and hand-drawn shapes with a unified Lucide SVG icon set throughout the shared UI layer
- feat(links): clicking a `matrix.to` or `matrix:` URI in a message body now navigates within the app instead of opening the browser (MSC2312)
- build: `icons.h` is regenerated automatically at build time whenever a source SVG changes
- fix(windows): premultiply alpha in `create_image_rgba` before uploading to Direct2D
- fix(macos): correct the macOS URI-scheme handler broken on the x86_64 build by the matrix.to commit

### Details

#### 2026-06-07

- feat(ui): replace Unicode glyphs and hand-drawn shapes with a unified
  Lucide SVG icon set throughout the shared UI layer. 16 monochrome Lucide
  line icons (ISC) embedded under `ui/icons/lucide/` and rasterized via
  nanosvg at runtime. Replaced sites: composer bar (emoji, sticker, mic,
  stop), message hover-action bar (react, reply, thread, edit, more) and its
  overflow menu (delete, pin), image and video viewers (close, download),
  voice/audio/inline-video play buttons, room-list join (+) button, and
  room-header jump-to-date and thread-list buttons. A new `tk::IconCache`
  (`svg.h`/`svg.cpp`) holds each rasterized icon and re-rasterizes only when
  the DPI scale or tint changes, so icons stay crisp on HiDPI and recolor
  correctly on light/dark theme switches via a new `rasterize_svg(..., Color
  tint)` overload. `PopupMenu::Item` gained an optional `svg_icon` field.
- build: `icons.h` is now regenerated automatically at build time whenever
  any source SVG under `ui/icons/lucide/` changes, without requiring a CMake
  reconfigure. Generation logic moved to `cmake/generate_icons.cmake`
  (invoked via `add_custom_command` with each SVG as a `DEPENDS` input);
  `tesseract_tk` depends on a new `tesseract_icons` target so every
  translation unit that includes `icons.h` gets an up-to-date header.
- fix(windows): premultiply alpha in `create_image_rgba` before uploading to
  Direct2D. The upload format is `GUID_WICPixelFormat32bppPBGRA`
  (premultiplied), but the path had done only a bare RGBA→BGRA channel swap,
  so transparent pixels from the SVG tinting path retained non-zero RGB and
  rendered as a visible coloured rectangle behind every Lucide icon on
  Windows. Fixed by applying `(channel × a + 127) / 255` premultiplication
  to match Cairo and CoreGraphics.
- fix(macos): correct the macOS URI-scheme handler broken on the x86_64
  build by the matrix.to commit: the `application:openURL:options:` signature
  (iOS-only) is replaced with the correct macOS 10.13+ `application:openURLs:`
  delegate method; a forward declaration for `_openJoinRoomSheetWithPrefill:`
  is added to the private class extension so it is visible before
  `@implementation`.
- feat(links): clicking a `matrix.to` or `matrix:` URI in a message body now
  navigates within the app instead of opening the browser (MSC2312). A room
  link for a joined room navigates directly via the new
  `RoomInfo::canonical_alias` field; an unknown room opens the join dialog
  pre-filled with the alias or room ID. User links open the profile panel;
  event links scroll to the target event. OS-level `matrix:` scheme
  registration on all four platforms: Linux — `tesseract.desktop` gains
  `MimeType=x-scheme-handler/matrix` and `%U`; Qt6 forwards URIs via
  `QLocalServer`, GTK4 via `G_APPLICATION_HANDLES_OPEN`; Windows — writes
  `HKCU\Software\Classes\matrix` at launch (no admin rights), `WM_COPYDATA`
  forwards to the running instance; macOS — `CFBundleURLTypes` in
  `Info.plist`, `application:openURL:options:` in `AppDelegate`. New
  `sdk/src/matrix_uri.rs` (19 Rust unit tests) parses both URI formats via
  ruma validators; `ShellBase::open_matrix_link()` owns the shared navigation
  and defers URIs received before login completes via `pending_matrix_link_`;
  `ShellBase::setup_link_clicked_()` replaces the four per-shell hardcoded
  `open_in_browser` lambdas so in-app links are intercepted before the
  browser fallback.

## v0.1.10 — 2026-06-07

Changes since v0.1.9:

### Summary

- fix(ui): when `restore_session()` fails at startup (e.g. no network)
- fix(ui): when sync loses connectivity at runtime (`sync_offline` / `sync_error` callback context)
- fix(build/macos): the macOS DMG no longer ships a broken code signature

### Details

#### 2026-06-07

- fix(ui): when `restore_session()` fails at startup (e.g. no network), the login
  view now shows a modal `AlertDialog` overlay with title "Connection Error" and
  Retry / Sign In buttons instead of silently presenting the bare login form.
  `AlertDialog` is a new shared widget modeled on `ConfirmDialog` but not
  backdrop-dismissible, with up to two configurable action buttons
  (`open(Options, primary_cb, secondary_cb)` / `close()` / `is_open()`). Wired on
  all four shells (Qt6, GTK4, Win32, macOS). The erroneous
  `SessionStore::clear_account()` call on any restore failure in the GTK4, Qt6, and
  Win32 shells is removed — session data must survive on disk so Retry can re-attempt
  the restore; only `handle_auth_error()` (triggered by `sync_auth_error`) should
  clear accounts. The misleading Qt6 "Account expired" status message and
  `save_index({})` wipe on the all-fail path are also removed.
- fix(ui): when sync loses connectivity at runtime (`sync_offline` / `sync_error`
  callback context), a 32 px amber "No internet connection — reconnecting…" banner
  appears at the top of the chat panel and auto-hides when `RoomListState` returns
  to `Running`. `MainAppWidget::set_offline(bool)` drives the banner visibility;
  `ShellBase` tracks the `offline_` flag and exposes `handle_offline_ui_()` /
  `handle_online_ui_()` virtual hooks; `EventHandlerBase` wires `sync_offline` /
  `sync_error` to the former and `RoomListState::Running` (when transitioning from
  offline) to the latter. All four shells benefit with no per-shell changes.
- fix(build/macos): the macOS DMG no longer ships a broken code signature. CMake's
  bundle install step rewrites build-dir rpaths via `install_name_tool`, which
  invalidates the `POST_BUILD` ad-hoc signature. An `install(CODE)` re-sign step
  now runs unconditionally after the rpath rewriting — ad-hoc (`-`) when no
  `TESSERACT_MAC_CODESIGN_IDENTITY` is configured (CI builds), real identity with
  `--options runtime --timestamp` when one is set. `--deep` is also added to the
  real-identity install path (was already present in the build-dir signing step).

## v0.1.9 — 2026-06-06

Changes since v0.1.8:

### Summary

- feat(ui): the room list auto-scrolls the most-recent unread room into view when new messages arrive
- feat(ui): code blocks and inline code in message bodies now render on a tinted background
- feat(ui): message rows show the event timestamp (HH:MM) tucked under the sender avatar
- feat(ui): room list
- feat(ui): room list
- feat(shell): room navigation history
- feat(gif): the GIF preview strip animates via WebP/GIF frames decoded off-thread
- feat(gif): when sending a GIF from search results
- feat(ux): the status bar briefly shows an error when a file drop is refused as unreadable or over the upload limit
- feat(gif): `/gif <query>` slash command opens an inline GIF search strip (Klipy SDK) above the composer
- feat(gif): the GIF picker strip animates previews in two stages — a static JPEG thumbnail, then the animated WebP/GIF
- feat(media): MSC4278 `Private` mode now exempts the local user's own uploads from public-room media suppression (`Off` still blocks everything)
- change(ui): the hover-action pill no longer has an inline delete/redact button. A new `⋯` overflow button opens a `PopupMenu`
- perf(ui): incoming messages no longer re-measure or re-shape the whole room
- perf(ui): `ListView` gained targeted (incremental) height invalidation
- perf(ui): per-message relayout is coalesced
- change(ui): default message-grouping window raised from 60s to 300s (`Settings::message_group_interval_s`)
- perf(media): inline images (MediaImage / MediaThumbnail) are now decoded off the UI thread on Qt6, Win32, and macOS
- refactor(async): text sends, reactions, pagination, room join/leave/invite- accept
- fix(ui): enlarge the section-header expand/contract chevron from `FontRole::Small` to `FontRole::UiSemibold` for legibility
- fix(shell): clicking a space in the room list now drills into it instead of also selecting it as the active room
- fix(gtk): room-list Ctrl+click now opens the room in a new tab
- fix(shell): rooms visited during rapid back/forward navigation could get permanently stuck showing no content
- fix(video/qt6): reset the `QMediaPlayer` source device between clips so Qt's FFmpeg backend rebuilds the demuxer
- fix(video/win32): fix diagonal shearing on hardware-decoded videos
- fix(gif): room-list last-message preview now shows "sent a GIF" for `fi.mau.gif` vendor-hint events from bridges
- fix(macos): accounts that never triggered a token refresh kept the old per-user Keychain item indefinitely
- fix(media): fetch the room avatar when `set_room()` is called directly, not only on room-list paints
- fix(views): drag-and-drop into pop-out room windows now works on all four platforms
- fix(views): pressing Ctrl/⌘+K from a focused pop-out brings the main window forward before opening the quick switcher
- fix(shell): pop-out windows are now torn down before their `ShellBase` registry entries on shutdown
- fix(media): animated GIFs and stickers evicted from `anim_cache_` are re-fetched when they scroll back into view
- fix(win32): the compose bar no longer loses keyboard focus on window re-activation

### Details

#### 2026-06-06

- feat(ui): the room list auto-scrolls the most-recent unread room into view
  when new messages arrive, so an unread room is never hidden below the fold.
  With several unread rooms the newest wins; a space counts when any of its
  child rooms is unread (`ShellBase::apply_space_child_counts_` now also rolls a
  space's `last_activity_ts` up to its newest unread child). Low-priority and
  Inactive rooms are excluded. The scroll is minimal (already-visible rooms
  aren't disturbed) and only genuinely newer activity re-triggers it (a per-view
  `last_unread_scroll_ts_` gate), so unrelated room-list updates never yank the
  scroll position. Implemented on a new deferred
  `tk::ListView::scroll_to_index_deferred` that applies once row heights are
  re-measured. Optional via a new "Scroll to rooms with new messages" Appearance
  checkbox (`Settings::autoscroll_unread_rooms`, default on). All shared code, so
  every shell (Qt6, GTK4, Win32, macOS) benefits. 9 new Catch2 tests.
- change(ui): the hover-action pill no longer has an inline delete/redact button.
  A new `⋯` overflow button opens a `PopupMenu` — a new shared widget added to
  `tesseract_tk` — that contains "Delete message" (destructive, own messages)
  and "Pin / Unpin message" (moderators). The action pill stays visible while
  the popup is open so context is preserved as the user moves the cursor into
  the menu.
- fix(ui): section header expand/contract chevron enlarged from `FontRole::Small`
  (8 pt) to `FontRole::UiSemibold` (11 pt semibold) so it is clearly legible
  inside the 28 pt header row. No HiDPI scaling bug — both the CoreText and
  DirectWrite backends handle DPI correctly at draw time.

#### 2026-06-05

- perf(ui): incoming messages no longer re-measure or re-shape the whole room.
  `MessageListView` body text layouts are cached and shared across measure,
  paint, and repaints — validity-keyed on width / theme / spoiler-revealed /
  content and LRU-bounded — so room switches and repaints stop rebuilding text
  layouts. The cache reuses the existing event-id-keyed `link_layout_cache_`
  (already read by link/selection hit-testing).
- perf(ui): `ListView` gained targeted (incremental) height invalidation —
  `invalidate_row` / `insert_row` / `erase_row` plus a new
  `ListAdapter::height_dependency_span`. A single inserted/updated/removed
  message re-measures only a bounded day-block neighbourhood and rewalks the
  row-offset prefix sum from there, instead of re-measuring every row. The
  message-list override implements the day-block rule (continuation look-back +
  adjacent virtual-row visibility); read-marker moves still force a full
  rebuild. `invalidate_data()` (room list, `set_messages`) is unchanged, and a
  size-mismatch safety net falls back to a full rebuild.
- perf(ui): per-message relayout is coalesced. New shared
  `ShellBase::schedule_relayout_` folds a burst of incoming-message relayout
  requests into one deferred (still synchronous) layout pass, so a sync flood no
  longer triggers a whole-tree measure+arrange per message. Native-overlay
  positioning timing is unchanged. Wired into the incoming-message handlers;
  all four shells benefit (shared code). 10 new Catch2 tests.

- feat(ui): code blocks and inline code in message bodies now render on a
  tinted background. A new `code_bg` palette token (light `0xD9DCE3`, dark
  `0x2E3138`) is filled behind code runs in `MessageListView::paint_body_text`.
  Fenced ```` ``` ```` blocks draw a single rounded panel enclosing the whole
  block (the contiguous `code_block` span run is gathered and its per-line
  rects unioned); inline `` `code` `` gets a tight per-run tint. A new
  `code_block` flag on `tk::TextSpan` distinguishes the two.
- feat(ui): message rows show the event timestamp (HH:MM) tucked under the
  sender avatar. The first message of a group always shows it; continuation
  rows (avatar hidden) show it on hover, vertically centred against the
  message line.
- change(ui): default message-grouping window raised from 60s to 300s
  (`Settings::message_group_interval_s`) — consecutive messages from the same
  sender within 5 minutes collapse into continuation rows.
- feat(ui): room list — section headers are now sticky. While scrolling a long
  section, its header pins to the top of the list and is pushed up by the next
  section's header. The pinned header is interactive (click to collapse/expand,
  hover highlight); `RoomListView` overrides `dispatch_pointer_down/move` so it
  wins over the inner `ListView` rows beneath it. `ListView::row_world_rect`
  was promoted to public for the overlay geometry.
- feat(ui): room list — a room's title renders semibold (`FontRole::SidebarName`)
  when it has unread messages; section-header titles are now black
  (`text_primary`) on a higher-contrast background (new `section_header_bg` /
  `section_header_hover` palette tokens).
- fix(shell): clicking a space in the room list now drills into it instead of
  also selecting it as the active room (the space title could appear in the
  room header). The `is_space` drill guard was missing from the live click
  path; added to all four shells (Qt6, GTK4, Win32, macOS).
- fix(gtk): room-list Ctrl+click now opens the room in a new tab, matching the
  other shells (the GTK handler was missing the modifier check).
- feat(shell): room navigation history — Alt+Left / Alt+Right (Cmd+[ / Cmd+]
  on macOS) traverse a per-session back-forward history of visited rooms, like
  browser navigation. `ShellBase` maintains a `room_nav_history_` vector (capped
  at 100 entries) with a cursor; `after_active_room_changed_` appends on every
  organic room visit; a `room_nav_in_progress_` guard prevents re-entrant
  history appends during a back/forward step. Shortcuts wired on all four
  shells: Qt6 (`QShortcut` / `ApplicationShortcut`), GTK4
  (`GtkShortcutController` / `GTK_SHORTCUT_SCOPE_GLOBAL`), Win32 (accelerator
  table + `WM_COMMAND`), macOS (`NSEvent` local monitor). 10 new Catch2 tests.
- fix(shell): rooms visited during rapid back/forward navigation could get
  permanently stuck showing no content — even after clicking them in the room
  list — until the app restarted. The subscribe worker that loads a room's
  timeline only cleared `pagination_[room].in_flight` when the room was still
  the active one on return; navigating away mid-flight left `in_flight = true`
  forever, causing every subsequent `onRoomSelected` to bail out immediately.
  Fixed on all four shells (Qt6, GTK4, Win32, macOS) by always clearing
  `in_flight` when the worker completes, regardless of current room.
- feat(gif): GIF preview strip now animates via WebP / GIF frames decoded
  off-thread (the previous approach decoded the heavier MP4 send-form through a
  full video pipeline per cell). `GifResult` carries a separate `strip_url` /
  `strip_mime` (WebP → GIF → MP4 fallback) for display; the MP4-only fallback
  extracts a frame off-thread via the native video decoder. Decoded frames are
  scaled to the cell size and persisted to `MediaDiskCache` so re-search loads
  skip re-downloading.
- fix(video/qt6): reset `QMediaPlayer` source device to `nullptr` between clips
  so Qt's FFmpeg backend rebuilds the demuxer; previously reusing the same
  pointer was treated as a no-op and the second video decoded through stale
  H.264 state.
- fix(video/win32): fix diagonal shearing on hardware-decoded videos — stride
  was computed from the unpadded width instead of the GPU-aligned row stride;
  fix `IMF2DBuffer::Lock2D` detection and probe common GPU alignment values
  (64–2048 bytes) when 2D metadata is absent. Also correct aspect ratio from
  actual decoded dimensions and sync playback rate to sample `PTS` vs the audio
  engine clock instead of a hardcoded 60 fps `Sleep(16)`.
- fix(gif): room-list last-message preview now shows "sent a GIF" for
  `fi.mau.gif` vendor-hint events from bridges.
- feat(gif): when sending a GIF from search results, prefer MP4 → WebP → GIF
  send order so bridges that cannot re-upload WebP receive a `video/mp4`
  `m.video` instead.
- feat(ux): status-bar briefly shows an error message (auto-clears after 4 s)
  when a file drop is refused because the file can't be read or exceeds the
  server upload limit. Implemented once in `ShellBase` (`show_status_message_` /
  `on_show_status_message_ui_` hooks) and wired on all four shells and all
  pop-out windows.
- fix(macos): accounts that never triggered a token refresh kept the old
  per-user Keychain item indefinitely, causing two system access-confirmation
  dialogs on every launch; migration now runs in `load()` (not only `save()`)
  so it happens on the first startup that reads the legacy item.
- perf(media): inline images (MediaImage / MediaThumbnail) are now decoded off
  the UI thread on Qt6, Win32, and macOS, matching GTK4's existing behaviour.
  Decoded frames are posted back to the UI thread via `post_to_ui_`; all three
  decoders (`QImageReader`, WIC, `CGImageSource`) are documented thread-safe.
- fix(media): room avatar was not fetched when `set_room()` is called directly
  (main and pop-out windows) because the `ensure_room_avatar_` path was only
  triggered by room-list paints; a cache miss at `set_room` time now fires the
  fetch.

#### 2026-06-04

- fix(views): drag-and-drop into pop-out room windows now works on all four
  platforms; a new shared `dispatch_file_drop()` in `views/media_drop` owns the
  MIME routing and size guards, replacing the lambda duplicated across the main
  windows; `ShellBase::extract_drop_media_()` virtual lets each shell run its
  media probe retargeted to the correct pop-out compose bar.
- fix(views): pressing Ctrl/⌘+K from a focused pop-out now brings the main
  window forward before opening the quick switcher (GTK4 gains a per-pop-out
  global-scope shortcut that routes to `request_quick_switch_from_popout()`).
- fix(shell): pop-out windows are now torn down before their `ShellBase`
  registry entries on shutdown, preventing use-after-free when a closing pop-out
  posts back to the main shell.
- feat(gif): `/gif <query>` slash command opens an inline GIF search strip
  (Klipy SDK) above the composer. Results animate in a horizontal strip (↑/↓/Tab
  to navigate, Enter to send, Esc to dismiss); chosen GIFs are sent as an
  autoplaying `m.video` carrying the `fi.mau.gif` vendor hint; E2EE rooms
  encrypt the MP4 and thumbnail. The Klipy `customer_id` is derived from the
  local MXID via SHA-256 so no raw identity leaves the device. Wired on all four
  shells (Qt6, GTK4, Win32, macOS) with the shared `GifEngine` / `GifPopup` /
  `GifController`; unit-tested.
- feat(gif): GIF picker strip animates previews in two stages: a static JPEG
  thumbnail appears immediately while the animated WebP/GIF loads in a
  background worker, then replaces it. Each platform uses its own image backend
  (`decode_image_`) and updates `anim_cache_`; GTK4 and Win32 gained the
  `set_anim_cache()` / `update_anim_regions()` hooks they previously lacked.
- feat(media): MSC4278 `Private` mode now exempts the local user's own uploads
  from public-room media suppression (`Off` still blocks everything). The single
  `media_allowed()` pure function is consulted at both the fetch gate
  (`ShellBase`) and the paint-time placeholder predicate (`MessageListView`).
  Adds a truth-table Catch2 test.
- refactor(async): text sends, reactions, pagination, room join/leave/invite-
  accept, and file uploads converted from blocking C++ worker threads to
  fire-and-forget `rt.spawn()` tokio tasks with `IEventHandler` callbacks:
  `paginate_back_async` / `paginate_forward_async`, `accept_invite_async` /
  `decline_invite_async` / `block_invite_async` / `join_room_async` /
  `leave_room_async` / `invite_user_async`, `send_image_async` /
  `send_video_async` / `send_audio_async` / `send_file_async`. Blocking Rust /
  C++ wrappers removed.
- fix(media): animated GIFs and stickers evicted from `anim_cache_` (30 s
  visibility TTL) are now re-fetched when they scroll back into view; `image_
  provider_` falls through to `ensure_media_image_` on a full cache miss; static
  thumbnails are pinned in `thumbnail_cache_` so they are never swept while
  their row is live.
- fix(win32): compose bar lost keyboard focus on window re-activation (Alt-Tab /
  click from background) because `DefWindowProc` restored focus to the D2D
  canvas HWND (no input handler) rather than the compose field; now redirected
  explicitly after `WM_ACTIVATE` when focus would otherwise land on a non-input
  surface.

## v0.1.8 — 2026-06-03

Changes since v0.1.7:

### Summary

- feat(views): pop-out room windows are now reachable and work end-to-end on all four platforms
- feat(views): targeting a room already open in a pop-out raises that pop-out instead of re-opening it in the main window
- feat(views): Ctrl/⌘+K quick switcher
- feat(ui): the room-info panel topic sizes to its wrapped line count (up to 5 lines) instead of a fixed slot
- feat(ui): room tags
- feat(ui): show worker-pool queue depth in the in-flight indicator tooltip ("fetch: N queued · send: M queued") across all four shells
- feat(ui): clicking the system-tray icon navigates to the first unread room
- feat(ui): use adjacent zoom-level tiles as placeholders during map zoom
- perf(media): media downloads are now non-blocking
- refactor(sdk): use the high-level `NotificationSettings` API for per-room notification mode instead of hand-rolled push-rule requests
- perf(ui): fetch room-list avatars lazily on first paint
- build: add Arch-style security hardening as C++ defaults, gated behind `TESSERACT_ENABLE_HARDENING`
- refactor: deduplicate the tray badge constants and the Linux portal notification-ID sanitizer across the four shells
- i18n: add an autogenerated Spanish translation placeholder
- fix(views): pop-out room info panels now load their member list (and the topic-edit / leave-room / ignore-user actions work)
- fix(views): animated inline media and pop-out emoji/sticker pickers now advance frames without requiring mouse motion
- fix(mentions): wire @mention avatars and a live client across all shells, including pop-out windows
- fix(timeline): preserve the scroll position by the row under the cursor instead of by total content-height delta
- fix(video): honor the `fi.mau.loop` / `fi.mau.gif` / `fi.mau.no_audio` hints on Windows and macOS
- fix(video): repack row-padded Media Foundation frames on Windows
- fix(views): keep the emoji shortcode popup's Up/Down navigation alive across keystrokes
- fix(sdk): detect stalled media downloads faster via HTTP-layer timeouts
- fix(ui): count pending media downloads in the in-flight status-bar tooltip
- fix(ui): order the thread list newest-at-bottom to match the message timeline
- fix(media): back off re-requesting media that fails to fetch
- fix(build): drop the `u8` prefix on the in-flight-dot label literals
- fix(build): silence release-build `-Wformat-truncation` / `-Wstringop-overflow` warnings exposed by the hardening flags
- fix(ui): left-align the space nav-bar title and elide it with an ellipsis
- fix(media): bound every media fetch with a per-request timeout so a stalled request can't pin the in-flight indicator or a worker thread
- fix(threads): backfill the full thread list on every panel open
- fix(ui): center emojis vertically in emoji-picker cells on Qt6
- fix(win32): make all tooltips visible on Windows 11
- fix(ui): video frames were invisible on Windows during playback
- fix(ui): compose-bar auto-height on Windows

### Details

#### 2026-06-03

- feat(views): pop-out room windows are now reachable and work end-to-end on
  all four platforms. Ctrl/⌘+click a room tab to pop it out into its own native
  window (the tab closes). The feature (`open_room_in_new_window`) had been
  fully built but never wired to an entry point, so it had never run; wiring it
  surfaced and fixed a batch of bugs — `PopoutRoomWidget::arrange` now sets its
  own `bounds_` (the zero-rect root had been silently dropping every pointer
  event), thread replies no longer leak into pop-out timelines, and a
  use-after-free on the TabBar-owned `room_id` during `tab_popout_room` is
  avoided by copying the id before `tab_close()` rebuilds the bar.
  `wire_room_view_` now wires forward-pagination, media send (image/video/audio/
  file via a new `encode_for_send_` virtual) and image-paste-as-attachment;
  `finish_init_` seeds the window title from cached room info. Per-platform glue
  (pickers, tooltips, link-hover cursor, native text-area overlay) mirrors each
  main window; Win32 also gets a reusable `Win32PickerPopup`.
- feat(views): targeting a room that is already open in a pop-out now raises that
  pop-out instead of re-opening the room in the main window (which would show it
  twice). A shared `ShellBase::focus_secondary_window_` guards `tab_select_room`,
  `tab_open_room`, and `tab_navigate_room` (room-list click, ctrl/⌘+click,
  notification navigation), and the tray-activation handlers raise a popped-out
  unread room via a new `focus_tray_unread_popout_`.
- feat(views): Ctrl/⌘+K quick switcher — a centered command-palette overlay
  (Slack/Discord-style) for jumping between rooms: a native search field with a
  horizontal "Recent" strip and a full, name-filtered room list below. Up/Down
  navigate, Enter/click jump, Escape/click-outside close. Implemented once in the
  shared toolkit (`views/QuickSwitcher`) and mounted as the topmost overlay in
  `MainAppWidget`; visit-order recency is tracked by a new MRU list in `ShellBase`
  recorded at the `after_active_room_changed_` chokepoint. `NativeTextField` gains
  a `set_on_popup_nav` hook so the arrow keys drive the list while the field holds
  focus; each shell wires the accelerator so it fires even while a native edit
  control has focus (Win32 accelerator table, Qt `QShortcut`, GTK
  `GtkShortcutController`, macOS `NSEvent` monitor).
- fix(views): pop-out room info panels now load their member list (and the
  topic-edit / leave-room / ignore-user actions work). `RoomWindowBase` never
  wired `on_fetch_room_members` — the panel fetches members lazily through that
  callback, which only the main windows set — so the panel opened empty. All four
  callbacks are now wired in the shared base (fixing Qt6/GTK4/Win32/macOS at
  once): members fetch on the read pool and pre-cache avatars before populating
  the view; topic/leave/ignore go through a new `run_async_mut_` helper so
  mutations stay ordered relative to sends; leaving a room from its own pop-out
  closes that pop-out.
- fix(views): animated inline media and pop-out emoji/sticker pickers now advance
  frames without requiring mouse motion. The 60 Hz animation tick repainted only
  the main shell's surfaces, so pop-outs froze until an unrelated event forced a
  repaint; `tick_anim_` now also calls a new `RoomWindowBase::repaint_anim_frame`
  on every owned secondary window (Win32/Qt6/GTK4 override it to also repaint
  their visible pickers; macOS shares the singleton picker panels).
- fix(mentions): wire @mention avatars and a live client across all shells,
  including pop-out windows. Pop-out `MentionController`s never set the popup's
  avatar image provider and never prefetched candidate avatars (so they always
  showed initials), and the macOS main window built its controller before the
  session was restored, leaving a null `Client*` snapshot so the popup never
  appeared. A shared `RoomWindowBase::wire_mention_shell_hooks_()` installs a live
  client getter, an avatar prefetch into the shared cache, and the popup's image
  provider; the macOS main window gets the same hooks inline. (Extends the same
  fix landed for the Win32 main window: the controller now reads the current
  client on each fetch via a `Hooks` getter instead of a stale ctor snapshot.)
- fix(timeline): preserve the scroll position by the row under the cursor instead
  of by total content-height delta. When an image, URL-preview card, or voice
  waveform decoded in or above the viewport, the affected row grew and shoved
  every row below it out from under the mouse (and the hover highlight latched
  onto the wrong message). A new row-top anchor (`ListView::ScrollAnchor`, keyed
  by `event_id` via `ListAdapter::row_key()`) captures the row under the cursor
  and its offset, and restores `scroll_y_` after `rebuild_heights` so growth above
  the anchor shifts scroll in lock-step and growth below leaves it untouched.
  Keyless lists (room list, thread list) fall back to the legacy behavior. +4 tests.
- fix(video): honor the `fi.mau.loop` / `fi.mau.gif` / `fi.mau.no_audio` hints on
  Windows and macOS — only the Qt backend had overridden the no-op defaults, so
  looping videos played once and stopped and `no_audio` was ignored. Win32 applies
  `SetLoop`/`SetMuted` to the `IMFMediaEngine` and rewinds the source reader on
  end-of-stream; macOS sets `AVPlayer.muted` and rewinds to `kCMTimeZero` on the
  play-to-end notification.
- fix(video): repack row-padded Media Foundation frames on Windows. A frame whose
  width isn't 16-px-aligned arrives with a stride larger than `frame_w*4`; the
  decode path had handed it to `make_image_from_bgra` as if tightly packed,
  shearing every row and rendering videos diagonally distorted. The real stride is
  now derived from the buffer length (`len / height`) and repacked row-by-row,
  with a bounds guard against a malformed stride/length.
- fix(views): keep the emoji shortcode popup's Up/Down navigation alive across
  keystrokes. The nav handler was installed only on first show but wiped on every
  compose keystroke by `hide_mention_popup_()`, so after the second keystroke the
  arrows fell through to caret movement. Now installed unconditionally on each
  tick, matching the slash-command popup. All four shells.
- fix(sdk): detect stalled media downloads faster via HTTP-layer timeouts — a 10 s
  `connect_timeout` on both reqwest clients, a 60 s matrix-sdk `request_timeout`
  (via a shared `build_sdk_http_client` helper), and a 10 s per-chunk idle timeout
  on `download_url`'s streaming loop, so connection/transfer stalls surface well
  before the outer `tokio::select!` backstop.
- fix(ui): count pending media downloads in the in-flight status-bar tooltip.
  After the async-media rewrite, requests spend time in `pending_media_` before
  the Rust `InFlightGuard` is created (the guard starts only after a semaphore
  permit is acquired), so the tooltip read zero while downloads queued behind the
  bulk lane. The tooltip's second line is now
  "media: N loading · fetch: N queued · send: N queued", refreshed on every
  `pending_media_` change across all four shells.

#### 2026-06-02

- perf(media): media downloads are now non-blocking. Previously every fetch
  (avatars, thumbnails, full images, stickers/emoji, map tiles, URL previews,
  voice/audio) ran as `rt.block_on` and pinned one of only four read-pool worker
  threads for the whole network round-trip, so opening a busy room could flood
  and starve those threads — a slow full-size download or a dead-server URL
  preview blocked the visible avatars the user was waiting on. Downloads now run
  as async tokio tasks bounded by per-lane semaphores (a wide interactive lane
  for avatars/thumbnails, a narrow bulk lane for full-size/preview/tile/voice)
  and complete via a new `on_media_ready` / `on_url_preview_ready` callback, so
  no download ever pins a thread. New FFI: `fetch_media_async`,
  `fetch_url_async`, `get_url_preview_async`, `cancel_media_group`. Switching
  rooms now cancels the previous room's still-pending timeline media (grouped by
  room) so the new room's media isn't stuck behind the old room's flood. The
  voice play/scrub path, which previously froze the UI thread on an uncached
  clip across all four shells, now warms bytes asynchronously and never blocks.
  C++ side: a shared `fetch_media_pipeline_` (disk-read → async fetch → store +
  deliver) plus a UI-thread-only pending-request registry on `ShellBase`.

- feat(ui): the room info panel's topic now sizes to its wrapped line count
  (up to 5 lines) instead of a fixed 80px slot; a longer topic is clipped at
  the cap and reveals the full text via a hover tooltip, mirroring the room
  header. `measure_topic_height_` builds/measures the wrapped layout in
  `arrange()`; the panel bubbles `on_show_tooltip` / `on_hide_tooltip` through
  `RoomView` like the header and compose bar.
- feat(ui): room tags — Favourite / Low-priority switches in the room info
  panel, as two stacked rows between the topic and the member list. Two new
  shared widgets land: `tk::SwitchButton` (settings-style label + sliding
  on/off switch, used here) and `tk::ToggleButton` (accent-filled pill toggle,
  available for reuse). The two tags are mutually exclusive in the UI and
  server-side, backed by matrix-sdk's `Room::set_is_favourite` /
  `set_is_low_priority` (new FFI `set_room_favourite` / `set_room_low_priority`);
  tag state rides on `RoomInfo` (`is_favorite` + new `is_low_priority`) so the
  switches re-sync to authoritative server state on every room update. Wired
  through `RoomView` → `ShellBase` and the popout room windows. +4 widget tests.
  The sync watcher's room-list change-detection fingerprint
  (`room_list_fingerprint`) now includes the favourite / low-priority flags, so
  toggling a tag re-sections the room in the list immediately rather than only
  after a restart (account-data updates carry no recency/unread change, which
  the old fingerprint keyed on exclusively). +3 fingerprint tests.
- refactor(sdk): use the high-level `NotificationSettings` API for per-room
  notification mode instead of hand-rolled push-rule requests; keeps the SDK
  ruleset cache consistent and matches Element's mode resolution.
- fix(ui): order the thread list newest-at-bottom to match the message timeline.
  `ThreadListView` now sorts ascending (newest last), opens scrolled to the
  bottom, and paginates older threads on scroll-up with anchored scroll
  (`preserve_top_through` + `on_near_top`), mirroring `MessageListView`. The
  fixed close-button header overlay stays put under any ordering. Thread-list
  ordering / row-click tests updated.
- perf(ui): fetch room-list avatars lazily on first paint. A new
  `RoomListView::on_room_avatar_needed` callback fires from `paint_room` on a
  cache miss (painted — i.e. visible — rows only), wired once in
  `ShellBase::wire_main_app_widget_` to `ensure_room_avatar_`. The eager
  per-shell "fetch every room" loops are removed, so avatars for rooms in
  collapsed or off-screen sections aren't requested until scrolled into view.
- fix(media): back off re-requesting media that fails to fetch. Empty-result
  fetches (network error / 5xx / timeout) are recorded in a per-key
  exponential-backoff cache (`media_fetch_failed_`, 30 s → 30 min), consulted by
  the four `ensure_*` avatar/media paths and cleared on success or cache-wipe.
  Stops a dead-homeserver avatar (e.g. a forgotten DM) from hammering on every
  sync tick. Mirrors the existing `tile_fetch_failed_` pattern, with a TTL so a
  recovered server reloads without a restart.
- fix(build): drop the `u8` prefix on the in-flight-dot label literals. Under
  GCC 16 a `u8"…"` literal is `const char8_t*`, which no longer converts to
  `const char*`, breaking `gtk_label_new(u8"●")`. Plain `"●"` (the source is
  UTF-8) matches the convention documented in `client/src/emoji.cpp`; applied to
  the Qt6 `QLabel` site too.
- feat(ui): show worker-pool queue depth in the in-flight indicator tooltip
  ("fetch: N queued · send: M queued") across all four shells. `WorkerPool` gains
  an atomic `pending_` counter and an `on_change_` callback fired on enqueue/
  dequeue; `ShellBase::init_pool_callbacks_()` re-posts `on_inflight_ui_()` so the
  tooltip stays current, and Qt6/GTK4 force the displayed tooltip to refresh in
  place so hovering users see live values without re-hovering.
- build: add Arch-style security hardening as C++ defaults (new
  `cmake/Hardening.cmake`, gated behind `TESSERACT_ENABLE_HARDENING`, ON):
  `-fstack-protector-strong`, `-fstack-clash-protection`, `-fcf-protection` /
  `-mbranch-protection=standard`, `-Wformat -Werror=format-security`,
  `-D_GLIBCXX_ASSERTIONS`, `-D_FORTIFY_SOURCE=2` (non-Debug), and link-time
  `-Wl,-z,relro -Wl,-z,now`. Each flag is probed so arch-/platform-specific ones
  skip cleanly; the flag set is gated to the Linux toolchain (macOS and MinGW/MSVC
  Windows keep their native hardening).
- fix(build): silence release-build `-Wformat-truncation` / `-Wstringop-overflow`
  warnings exposed by the hardening flags — size the mm:ss duration buffers to the
  worst case, and replace an unbounded span `memcpy` in `svg.cpp` with a vector
  range constructor.
- fix(ui): left-align the space nav-bar title and elide it with an ellipsis,
  constrained to the sidebar width right of the avatar, so a long space name is
  clipped instead of overflowing under the avatar and back button.

#### 2026-06-01

- fix(media): bound every media fetch with a per-request timeout so a stalled
  or endlessly-retrying request can't pin the in-flight indicator (or a worker
  thread). Each `block_on(select!{ … / stop_fut })` gains a `tokio::time::sleep`
  arm — 30 s for thumbnails/avatars, 120 s for full media; on timeout the
  `InFlightGuard` drops and the read-pool thread frees. Also makes the in-flight
  count order-independent: a new `in_flight_count()` FFI getter is re-read on
  every `on_inflight_changed`, so the dot reflects the authoritative Rust atomic
  regardless of cross-thread notification ordering.
- fix(threads): backfill the full thread list on every panel open. Re-arm
  `threads_reached_start_` on open, and continue the pagination chain from
  `paginate_threads_`'s own completion callback so a watcher notification
  arriving mid-pagination can't break the chain. Previously a re-opened panel
  showed only the most recent threads.
- feat(ui): clicking the system-tray icon navigates to the first unread room.
- refactor: deduplicate the tray badge constants and the Linux portal
  notification-ID sanitizer across the four shells (into `tray_icon.h` /
  `linux_portal.h`).
- fix(ui): center emojis vertically in emoji-picker cells on Qt6.
- fix(win32): make all tooltips visible on Windows 11.
- fix(ui): video frames were invisible on Windows during playback.
- fix(ui): compose-bar auto-height on Windows.
- feat(ui): use adjacent zoom-level tiles as placeholders during map zoom.
- i18n: add an autogenerated Spanish translation placeholder.

## v0.1.7 — 2026-06-01

Changes since v0.1.6:

### Summary

- feat(pins): cross-platform Matrix pinned events
- feat(messagelist): hover action pill
- feat(win32): windowless RichEdit compose bar
- feat(win32): route emoji to Noto Color Emoji via `IProvideFontInfo`
- feat(session): restore all open room tabs across restarts
- feat(compose): add `/spoiler [(reason)] <text>` slash command (MSC2010)
- feat(compose): add `/slap <target>` slash command
- feat(compose): `/` in the composer opens a filtered slash-command popup
- feat(threads): cross-platform Matrix threads UI on top of the MSC3440 SDK/FFI infrastructure landed in `9d99d2e`
- feat(encryption): guided encryption-setup overlay
- feat(settings): cache-stats tooltips in About settings
- feat(compose): wheel-event navigation in autocomplete popups
- feat(status): in-flight HTTP request dot indicator
- feat(i18n): extract user-visible strings for translation
- perf(sdk): suspend the DM presence polling loop while the window is hidden
- fix(win32): full HiDPI fix
- fix(win32): honour dark mode in emoji/sticker pickers and Win32RichEditArea
- fix(win32): scale status bar height with screen DPI
- fix(win32): drop `needs_repaint_` flag
- fix(win32): intercept Ctrl+V image paste before `TxSendMessage`
- fix: per-account recovery/verification banner state
- fix: account picker positioned at top of window on macOS + verification state access
- fix: clicking the compose bar card focuses the text input
- fix(account-picker): prefetch all account avatars on open and repaint on arrival
- fix(oauth): drop UI platform suffix from device display name
- fix(timeline): serialize receipt-refresh into the streaming task
- fix(roomview): clear pinned banner state in `clear_room()`
- fix(settings): refresh sidebar strip after self-avatar change
- fix: place caret at end of compose field after accepting a slash command from the popup
- fix(message-cache): invalidate a room's cached row snapshot when a timeline event arrives for it while it's not selected
- fix(ui): composer Home / End and scroll-to-caret on Windows

### Details

- feat(pins): cross-platform Matrix pinned events — a `PinnedBanner` widget sits above
  the message list and cycles through `m.room.pinned_events`; clicking the banner body
  jumps to the pinned message (in-window scroll when loaded, `subscribe_room_at` fallback
  otherwise). A Pin / Unpin action appears in the hover action pill, gated by the user's
  power level (`can_pin_in_room` checks `m.room.power_levels`). Architecture: new
  `sdk/src/client/pins.rs` with `pin_event` / `unpin_event` (state-event read-modify-write)
  and `can_pin_in_room`; `RoomInfo` carries a `pinned_events: Vec<PinnedEvent>` resolved via
  `Room::load_or_fetch_event` (cache → SQLite → `/event/{id}` fallback); pin changes flow
  through the existing `on_rooms_updated` path, so no new FFI callback is needed.
  `MessageListView` gains `set_can_pin` + `set_pinned_event_ids`; all four shells
  (Qt6, GTK4, Win32, macOS) wire two new `RoomView` callbacks. 11 new tests.
- feat(messagelist): hover action pill — collapse the per-button hover strip into a single
  rounded pill of square cells (reply / thread / react / edit / redact / pin) anchored to
  the top-right of each row. Cells share one outer outline separated by 1 px dividers;
  the hovered cell gets a subtle pressed fill. The `+react` chip moves onto the pill for
  rows with no reactions and stays at the end of the reactions strip when reactions exist.
  Redacted and unable-to-decrypt rows suppress the react entry.
- feat(win32): windowless RichEdit compose bar — replaces the windowed RICHEDIT50W child
  HWND with a windowless `ITextServices2`-driven control using Direct2D / DirectWrite
  rendering. Implements `ITextHost2` (~51 pure virtuals); `TxDrawD2D` paints directly into
  the surface's render target so colour emoji (COLR/CPAL via DirectWrite) appear correctly
  in the compose bar. All existing NativeTextArea behaviour preserved (replace_range,
  @-mention pills, clipboard image paste, slash-popup nav, Up-to-edit-last, IME).
- feat(win32): route emoji to Noto Color Emoji via `IProvideFontInfo` — implements the
  `IProvideFontInfo` interface on `Win32RichEditArea` and the main `Win32Canvas` text
  layouts, directing the DirectWrite font fallback chain to use Noto Color Emoji for
  emoji codepoints in all message rows (as already done for Qt6 / GTK4 / macOS).
- feat(session): restore all open room tabs across restarts — the `im.gnomos.tesseract`
  account-data event now carries an `open_rooms` JSON array (all tab room IDs in visual
  order) alongside the existing `last_room` active-tab field. On every room navigation
  the full tab list is serialised and written to the homeserver; on startup each shell
  reads `PrefsData::open_rooms` into `AccountSession` and passes it to a new
  `ShellBase::try_restore_tab_session_()` helper that pre-populates `tabs_` and fires
  `on_tab_state_changed_ui_()` once, reconstructing the full tab bar in a single pass
  with no inter-tab flickering. Account switching clears `tabs_` to prevent the previous
  account's tabs bleeding through. Backward-compatible: old prefs with only `last_room`
  auto-populate `open_rooms = {last_room}` on parse. The prefs serializer was migrated
  from hand-rolled JSON to `nlohmann/json`. 4 new unit tests; wired on all four shells.
- feat(compose): add `/spoiler [(reason)] <text>` slash command (MSC2010) — sends an
  `m.text` event with the spoiler content wrapped in a `data-mx-spoiler` span; the
  plain-text body is prefixed with `(Spoiler)` / `(Spoiler: reason)`. A new
  `markdown_inline_to_html` helper produces the HTML body without block wrapping.
- feat(compose): add `/slap <target>` slash command — classic IRC trout-slap; dispatched
  as an `m.emote` reading "slaps \<target\> around a bit with a large trout".
- fix(win32): full HiDPI fix — `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` means all
  Win32 APIs return physical pixels while D2D draws in DIPs; mixed coordinate spaces
  broke layout, input dispatch, native controls, popups, and pickers at any scaling above
  100%. Root cause fixed at the `host_win32` boundary: mouse coords now converted to DIPs
  before dispatch; `set_rect()` scales logical bounds → physical px for `SetWindowPos`;
  `natural_height()` returns logical px; all popup and picker `SetWindowPos` calls use
  DIP-to-physical helpers.
- fix(win32): honour dark mode in emoji/sticker pickers and Win32RichEditArea — the
  picker popup HWNDs and the windowless RichEdit now respond to the app's theme setting
  and switch between light and dark background / text colours correctly.
- fix(win32): scale status bar height with screen DPI — the status bar HWND height is now
  derived from the logical font height multiplied by the DPI scale factor instead of a
  hardcoded pixel value.
- fix(win32): drop `needs_repaint_` flag — the flag caused `WM_PAINT` to fire
  continuously, starving the animation timer and pegging CPU at idle. Replaced by the
  standard `InvalidateRect` / `ValidateRect` cycle.
- fix(win32): intercept Ctrl+V image paste before `TxSendMessage` — the windowed EDIT
  control was consuming `WM_PASTE` before the app's clipboard-image handler ran.
- fix: per-account recovery/verification banner state — `recovery_banner_dismissed`,
  `recovery_key_chosen`, `verification_banner_dismissed`, and `unverified` are now stored
  in `AccountSession` and saved/restored by every shell's `switch_active_account`, so
  dismissing a banner on one account no longer silences it on another. `EventHandlerBase`
  guards `handle_verification_state_ui_` to only fire for the active account while always
  persisting state. `recover()` returns a human-readable error on
  `SecretStorageError::MissingKeyInfo`.
- fix: account picker positioned at top of window on macOS + verification state access —
  the account picker popover was anchored at the wrong point; now correctly opens at the
  top of the sidebar. Also exposes `unverified_` / `verification_banner_dismissed_` via
  `using` declarations in `MacShell`.
- fix: clicking the compose bar card focuses the text input — a click anywhere on the
  compose bar's surrounding card area now forwards focus to the `NativeTextArea`.
- fix(account-picker): prefetch all account avatars on open and repaint on arrival —
  previously only the active account's avatar was loaded; inactive account rows showed
  blank discs until hovered.
- fix(oauth): drop UI platform suffix from device display name — device names registered
  during OAuth no longer include a platform tag, matching the behaviour of Element and
  other clients.
- fix(timeline): serialize receipt-refresh into the streaming task — the read-receipt
  refresh was racing the timeline streaming task; routing it through the same `select!`
  loop prevents the data race.
- fix(roomview): clear pinned banner state in `clear_room()` — the banner's pinned-events
  list was not reset on room deselect, leaving stale pin data visible briefly on the
  next room open.
- fix(settings): refresh sidebar strip after self-avatar change — changing the account
  avatar in profile settings now immediately updates the avatar shown in the sidebar
  identity strip.
- fix: place caret at end of compose field after accepting a slash command from the popup.
- feat(compose): `/` in the composer opens a filtered slash-command popup —
  arrow keys / Tab accept, Escape dismisses. Initial command set:
  `/me <action>` (m.emote) and `/shrug` (text macro). Wired in all four
  shells (Qt6, GTK4, Win32, macOS) following the same per-shell
  `ShortcodePopup` hosting pattern. New shared widget
  (`SlashCommandPopup`) + stateless engine (`SlashCommandEngine`) +
  central registry (`available_commands()` in `SlashCommands.{h,cpp}`).
- perf(sdk): suspend the DM presence polling loop while the window is hidden — the per-minute `GET /_matrix/client/v3/presence/{userId}/status` fan-out across every DM counterpart produces data that is only visible while the room list is on-screen, so polling while minimized/unfocused is pure waste. `ShellBase::notify_window_active_` now flips `presence_polling_enabled` off on focus loss and back on (with an immediate one-shot kick via new `Client::poll_presence_now`) on focus regain, so contacts don't appear stale for up to a minute after un-minimize. Gated by the same `Settings::send_presence` flag that `handle_send_presence_toggle_` owns — users who disabled presence in the Privacy tab are not silently re-enabled by a focus change. The poll body is refactored into a free `async fn poll_presence_once(...)` shared by the 60 s loop and the on-focus kick; the 403-Forbidden cache moves onto `ClientFfi` so both call sites share it. No platform-shell changes — all four shells already emit the active/inactive signal.
- feat(threads): cross-platform Matrix threads UI on top of the MSC3440 SDK/FFI infrastructure landed in `9d99d2e`. A new RoomHeader threads button toggles a right-side panel (60/40 split with the main message list) that hosts either a `ThreadListView` (all threads in the current room, sourced from `Client::list_room_threads`) or a `ThreadView` (one open thread + its own ComposeBar that routes to `Client::send_thread_message`). `ShellBase` owns a `Closed`/`List`/`Open` state machine; a pure `compute_thread_transition_` static function returns a side-effects descriptor that an applier maps onto `subscribe_thread` / `subscribe_room_threads` calls. `EventHandlerBase` marshals the four `on_thread_*` SDK callbacks + `on_threads_updated` to new `ShellBase::handle_thread_*_ui_` virtuals gated on `(current_room_id_, current_thread_root_)`. In-thread events skip the main `MessageListView` (short-circuit in `handle_message_*_ui_` + defence-in-depth filter in `MessageListView`). `MessageListView` gains thread-preview chips under roots that have replies, plus `set_dimmed` / `set_highlighted_event` used by `RoomView` while a thread is active (scrolls the root into view and dims the surrounding list). `ThreadView` composes a reused `MessageListView` + `ComposeBar` and strips `thread_root_id` from rows before forwarding so the inner filter doesn't drop them. All four shells wire four RoomView callbacks (`on_threads_button_clicked`, `on_thread_open_requested`, `on_thread_close_requested`, `on_thread_send`) to the new ShellBase entry points; a `RoomSwitch` transition fires before every `current_room_id_` assignment so the panel auto-closes on room change. 48 new Catch2 tests across 6 new test files; suite at 535/535.
- fix(message-cache): invalidate a room's cached MessageRowData snapshot when a timeline event arrives for that room while it's not selected — `handle_message_inserted_ui_` / `_updated_ui_` / `_removed_ui_` previously skipped the live-view update for non-current rooms but left any cached snapshot stale, so the next navigation briefly painted the old rows before the live `on_timeline_reset` overwrote them. A new private `invalidate_message_cache_(room_id)` helper drops the entry from both `message_cache_` and `message_cache_lru_`; the three handlers were restructured so the `else` (non-current-room) branch hangs off the room-id comparison alone, not the combined `&& room_view_` guard. 6 new Catch2 tests.
- feat(encryption): guided encryption-setup overlay — `EncryptionSetupOverlay`, a shared widget wired on all four shells, walks new-account users through cross-signing setup: `Intro` → `ChooseMethod` (recovery key or passphrase) → `ShowKey` / `Progress` → `Done`; a `Recover` path goes `Intro` → `EnterKey` → `Progress` → `Done`. Callbacks (`on_enable_recovery`, `on_recover`, `on_request_sas`, `on_close`) drive the underlying SDK calls; `advance_progress` / `report_error` update the progress step once async operations resolve. `ShellBase` gates the overlay on `encryption_setup_shown_` / `encryption_setup_dismissed_` to prevent double-raise and post-dismiss reappearance. 18 new tests.
- feat(settings): cache-stats tooltips in About settings — hovering the "Local cache" row in the About/Storage section now fires `on_show_tooltip` with the entry count and byte total; all value-column cells are laid out with a common x offset so numbers align across rows; the SDK store row produces no tooltip since byte-level tracking is not available there. 3 new tests.
- feat(compose): wheel-event navigation in autocomplete popups — the slash-command, shortcode, and @mention popups now respond to mouse-wheel events, scrolling the suggestion list exactly as Up/Down arrow keys do.
- feat(status): in-flight HTTP request dot indicator — a small coloured dot in the status bar reflects the number of currently in-flight Matrix API requests: green (0–1 requests), amber (2–10), red (>10). Hovering the dot shows a tooltip with the exact count. Wired across Qt6, GTK4, Win32, and macOS (the macOS shell receives its status bar in the same pass; previously it had none).
- fix(ui): composer Home / End and scroll-to-caret on Windows — `Home` now moves the insertion point to the start of the logical line and `End` to the end; after either keystroke the control scrolls to keep the caret visible. Matches existing behaviour on Qt6 and GTK4.
- feat(i18n): extract user-visible strings for translation — all user-facing strings in shared views and shells are now wrapped with the platform translation macro (`tr()` on Qt6, `_()` on GTK4) and the `.ts` / `.pot` templates regenerated. macOS `NSLocalizedString` and Win32 `LoadString` wiring remain deferred.

## v0.1.6 — 2026-05-24

Changes since v0.1.5:

### Summary

- feat(invites): room invitations section with accept/decline/block
- feat(settings): Privacy tab with presence toggle and room key export/import
- feat(settings): storage size display and cache-clear in About tab
- feat(room-list): room-list previews populated from background backfill
- feat(compose): SVG icons and tooltips in ComposeBar
- perf(tk/qt6): pre-shaped `QStaticText` for single-line draws
- perf(room-list): `TextLayout` objects cached in the room-list adapter
- perf(message-list): trigger back-pagination 1 viewport before reaching the top
- perf(list-view): skip `paint_row()` for rows outside the repaint clip
- perf(settings): suppress repaints when hover stays on the same tab
- fix(login): homeserver discovery was not triggered for the pre-populated default URL
- fix(sdk): enter tokio runtime context in blocking FFI methods
- fix(settings): preserve state store in `clear_caches` to keep room list
- fix(room-list): `on_scroll` incorrectly fired on every backfill `set_rooms` call
- fix(room-list): rooms with unknown last-activity timestamp were misclassified as inactive
- fix(receipts): read-receipt chips (avatar + name) were missing after member list fetch
- fix(backfill): `on_rooms_updated` called O(n) times instead of O(1)
- fix(image-packs): HTTP fallback results were not cached
- fix(message-list): scrollbar thumb drag blocked by text-selection hit-test
- fix(tk): single-line text spilled across hard line breaks on all backends
- fix(macos): animated room-list previews stopped advancing without mouse movement
- fix(mingw): cross-compile Win32 from Linux via MinGW-w64

### Details

- feat(invites): room invitations section with accept/decline/block — a new "Invitations" section appears at the top of the room list for pending Matrix room invites (DM and group). Clicking an invite opens an `InviteCard` overlay in the chat panel showing the inviter's avatar, room name, and details, with Accept and Decline buttons; DM invites also offer a Block action (decline + ignore the sender). Accepting a DM invite automatically persists `m.direct` so the room lands in the DM section rather than the general room list. The full stack: `InviteInfo` + `accept/decline/block_invite` FFI (Rust), `on_invites_updated` callback marshalled through `EventHandlerBase`, per-account invite storage and avatar prefetch in `ShellBase`, `InviteCard` widget and `kSecInvites` section (index 0) in `RoomListView`, slot in `MainAppWidget`; wired across all four shells. 478 C++ tests, 150 Rust tests.
- feat(settings): Privacy tab with presence toggle and room key export/import — a new "Privacy" settings tab with two groups. "Presence" has a single checkbox "Send and receive presence status" that controls both outgoing publishing (`PresenceTracker`) and the Rust-side 30 s polling loop (via an `Arc<AtomicBool>` flag on `ClientFfi`); the setting survives app restarts via a new `send_presence` field in `app_settings.json`. "Encryption" has two buttons: "Export room keys…" and "Import room keys…" which orchestrate a passphrase-prompt → file-picker → async SDK call chain; the high-level `Encryption::export_room_keys` / `import_room_keys` APIs from `matrix-sdk` handle the Megolm key file format and encryption. All four shells provide native dialogs: `QInputDialog`/`QFileDialog` (Qt6), `GtkFileChooserNative`/`gtk_dialog_new_with_buttons` (GTK4), in-memory `DLGTEMPLATE`/`OPENFILENAMEW` (Win32), `NSAlert+NSSecureTextField`/`NSSavePanel` (macOS). 476 C++ tests, 150 Rust tests.
- feat(settings): storage size display and cache-clear in About tab — the About settings page now shows a "Storage" section at the bottom-left (natural width, not page-spanning) with "Local cache" and "SDK store" size rows computed asynchronously when settings opens, and a destructive "Clear all caches" button (with confirm dialog) that wipes the media disk cache, `waveforms.db`, and the matrix-sdk event store then refreshes the displayed sizes; in-process state is unaffected — credentials and active sessions survive. Wired on all four shells (Qt6, GTK4, Win32, macOS). 476 C++ tests, 150 Rust tests.
- feat(room-list): room-list previews populated from background backfill — `room.latest_event()` is only updated by the live sync loop, so rooms whose most-recent event arrived solely through back-pagination had blank preview rows. `backfill_room_silent` now extracts a preview directly from the paginated timeline items (`preview_from_timeline_content`) and stores it in a persistent `backfill_previews` cache on `ClientFfi`; `apply_backfill_previews` patches these into every `on_rooms_updated` emission (backfill task and both sites in the room-info watcher) so a sync-triggered room update no longer erases the preview immediately after it appears.
- feat(compose): SVG icons and tooltips in ComposeBar — the emoji, sticker, voice, and voice-stop buttons in `ComposeBar` now use crisp SVG icons (nanosvg + nanosvgrast, vendored headers) instead of Unicode glyph text. A new `tk::rasterize_svg` free-function renders any SVG to an RGBA pixel buffer and `create_image_rgba` (implemented on Qt6, GTK4, and macOS/CoreGraphics) converts it to a backend `tk::Image` for cache and draw. `dispatch_pointer_move` is overridden in `ComposeBar` so pointer-move events reach `on_pointer_move` even when the native text area captures the mouse; this also enables `on_show_tooltip` callbacks that display a centred overlay tooltip (matching the emoji/sticker picker style) on hover for each button.
- fix(login): homeserver discovery was not triggered for the pre-populated default URL — `set_text()` does not fire `on_changed`, so the "New here? Create an account" link and other capability-gated elements were never shown for the initial `matrix.org` value. `hs_changed_()` is now called explicitly in `init_with_field()` and `reset()` (invoked when the user taps Add Account) so probing runs without requiring the user to type. 2 new Catch2 tests.
- fix(sdk): enter tokio runtime context in blocking FFI methods — methods invoked from C++ worker threads (`send_read_receipt`, `mark_room_as_read`, `accept_invite`, `decline_invite`, `block_invite`, `leave_room`, `get_room_members`, `ensure_members`, and `set_topic`) now call `rt.enter()` so any internal matrix-sdk code that spawns futures or uses `tokio::time` resolves against the correct runtime handle; omitting the guard caused panics after logout on several of these paths. 150 Rust tests.
- fix(settings): preserve state store in `clear_caches` to keep room list — the original `clear_caches` implementation deleted the matrix-sdk SQLite store including the state store, which caused the room list to be empty after the app restarted; only the event cache portion should be wiped. The 12-line manual deletion block is removed and replaced with the SDK's own `clear_caches()` which correctly targets just the event store.
- fix(room-list): `on_scroll` incorrectly fired on every backfill `set_rooms` call — the debounce was reset unconditionally, triggering a scroll re-classification even when the set of visible rooms had not changed. `on_scroll` is now only fired when the visible room set differs from the previous call, preventing spurious inactive-room reclassification during backfill.
- fix(room-list): rooms with unknown last-activity timestamp were misclassified as inactive — `classify_room_section` compared a zero `last_activity_ts` against the inactivity threshold and placed rooms whose timestamp had not yet been populated into the Inactive section. The classifier now treats `last_activity_ts == 0` as unknown and keeps those rooms in their normal section; `BackfillPreview` and the room-info watcher both persist the timestamp so it is eventually populated for all rooms.
- fix(receipts): read-receipt chips (avatar + name) were missing after member list fetch — `on_read_receipts_changed` fired before `ensure_members` completed and the member map was empty, so chips showed only bare user IDs. After `fetch_members` resolves, the timeline items are re-emitted via `on_message_updated` so receipt chips rebuild with the now-populated member data.
- fix(backfill): `on_rooms_updated` called O(n) times instead of O(1) — the backfill loop emitted a separate `on_rooms_updated` for every room it processed, queuing hundreds of sequential full room-list rebuilds on large accounts. The loop now batches all updates and emits a single `on_rooms_updated` per backfill cycle.
- fix(image-packs): HTTP fallback results were not cached — the image-pack loader fell back to a plain HTTP fetch when a media source was not in the SDK media cache, but it never stored the result, so every render cycle re-requested the same URL and spammed the server. HTTP fallback bytes are now written to the session HTTP cache on success.
- fix(message-list): scrollbar thumb drag blocked by text-selection hit-test — `MessageListView::on_pointer_down` ran the full message-content hit suite (including `char_index_at`) before the base `ListView` checked for a scrollbar-thumb press; a click on the scrollbar over a text row was claimed by text selection and subsequent drag events went to text selection instead of scrolling. The scrollbar thumb hit-test is now promoted to run first, matching the priority comment already in `ListView::on_pointer_down`.
- fix(tk): single-line text spilled across hard line breaks on all backends — `QPainter::drawText`, DirectWrite, CoreText, and Pango all honour embedded line separators (LF, CR, VT, FF, NEL, U+2028, U+2029) as hard breaks even with word-wrap disabled, so a `wrap=false` layout containing newlines rendered across multiple lines while `measure()` reported only one — silently overflowing fixed-height containers (reply-quote cards, compose reply-preview banner). A new `tk::fold_hard_breaks_utf8()` helper in `canvas.h` is applied in every backend's `build_text()` when `!s.wrap`, replacing separators with spaces so single-line is genuinely one line. The compose-bar reply preview banner also now collapses `\n`/`\r` in `set_reply_to` and bounds both text lines with `max_width` + `TextTrim::Ellipsis`.
- fix(macos): animated room-list previews stopped advancing without mouse movement — the frame-advance timer only ticked when a pointer-move event arrived; the timer is now driven independently of input so GIF/animated WebP previews in the room list play continuously.
- fix(mingw): cross-compile Win32 from Linux via MinGW-w64 — three MinGW header divergences from the MSVC SDK are patched in `canvas_d2d.cpp` (`<cmath>` missing; `IDWriteFactory3::CreateFontFaceReference_` trailing-underscore naming; `IDWriteFactory5::CreateFontSetBuilder` takes `IDWriteFontSetBuilder1**`). `CMakeLists.txt` moves D3D11/DXGI into `tesseract_tk` `PUBLIC` so GNU ld sees them after the archive that needs them, and extends the opus `find_library` call to cover MinGW. Two new CMake presets (`mingw-debug` / `mingw-release`, Linux-only) plus a `mingw-x86_64.cmake` toolchain file with `CMAKE_CROSSCOMPILING_EMULATOR=wine` so `catch_discover_tests` can enumerate tests on the build host.
- perf(tk/qt6): pre-shaped `QStaticText` for single-line draws — `QPainter::drawText()` with a plain string re-shapes glyphs on every call; `QFontMetricsF::elidedText` also ran per frame on elided layouts. `QtTextLayout` now pre-builds a `QStaticText(AggressiveCaching)` in its constructor so `draw()` submits pre-shaped data via `drawStaticText()`; multi-line wrap layouts fall back to `QTextLayout::draw()` which also avoids re-layout after the first `ensure_ql_()` call. Combined with the TextLayout object cache below, this removes all per-frame text work from the ~60% hot draw path.
- perf(room-list): `TextLayout` objects cached in the room-list adapter — previously every `paint_row()` call rebuilt a `TextLayout` for the room name and preview text from scratch; a name/preview keyed cache in the adapter reuses existing layouts across frames, eliminating the dominant allocation hot-spot in the room-list repaint loop.
- perf(message-list): trigger back-pagination 1 viewport before reaching the top — the paginate-back call was fired only when the scroll offset reached the very top of the loaded content, causing a visible stall as new items were fetched; it now fires when the user is within one viewport height of the top so content loads before it is needed.
- perf(list-view): skip `paint_row()` for rows outside the repaint clip — `ListView::paint` now tests each row rect against the `PaintCtx` clip before calling the row painter; rows that do not intersect the dirty region are skipped entirely, reducing per-frame work in long lists during small incremental repaints.
- perf(settings): suppress repaints when hover stays on the same tab — `SideTabView::on_pointer_move` now records the previously highlighted tab and only requests a repaint when the hovered tab actually changes, eliminating continuous repaint traffic while the mouse rests over a settings tab.

## v0.1.5 — 2026-05-23

Changes since v0.1.4:

### Summary

- feat(mentions): `@mention` autocomplete, pills, and `m.mentions`
- feat(timeline): syntax highlighting for fenced code blocks
- feat(dm): open an existing DM from the user profile panel instantly
- feat(login): account registration via OIDC `prompt=create`
- feat(roomlist): group inactive rooms into a fifth section
- feat(notifications): per-room notification settings in `RoomInfoPanel`
- feat(threads): expose Matrix threads (MSC3440) to the C++ level
- refactor(shells): hoist shared shell logic into `ShellBase`
- perf(roomlist): cache space children asynchronously
- perf(anim): stop full-window repaints for animated images
- build(ci): GitHub Actions release packaging
- fix(gtk4): make the GTK4 shell usable at all
- fix(gtk4): decode media off the UI thread to stop a multi-second startup freeze
- fix(session): persist rotated OAuth refresh tokens to the secret store
- fix(qt6): hide native search/composer overlays when the image or video viewer is open
- fix(session): store SDK account data under `data_dir()` instead of `config_dir()`
- fix(macos): unblock the x86_64 build broken by the stale CommandLineTools libc++
- fix(room): hide the compose input overlay after the active room closes
- fix(windows): emoji/sticker pickers follow the main window when it moves
- fix(ui): room-list row centering, macOS picker popups, larger emoji cells

### Details

- fix(gtk4): make the GTK4 shell usable at all — three fixes to a shell that aborted on startup so none of its paths had ever run. (1) System tray: replaced the `libayatana-appindicator3` tray with a pure `org.kde.StatusNotifierItem` + `com.canonical.dbusmenu` implementation over GDBus (`GtkSniTrayIcon`, icon rendered via gdk-pixbuf + cairo); linking appindicator pulled libgtk-3 into the GTK4 process and made `gtk_init()` abort with "GTK 2/3 symbols detected", so the appindicator dependency is dropped entirely. (2) User-strip right-click menu: GTK routes right-clicks through the tk widget tree but only sticker rects were hit-tested, so the Settings / Add-Account / Log-Out / Quit menu never fired — now hit-tests the lower-left user strip like the other shells. (3) Image send: `encode_for_send`'s JPEG compress path failed on any pixbuf with an alpha channel (every PNG), silently resetting the composer; images are now flattened onto opaque white before JPEG encoding.
- fix(gtk4): decode media off the UI thread to stop a multi-second startup freeze — gdk-pixbuf now routes image loading through glycin, which decodes in a sandboxed subprocess and blocks the calling thread; `on_media_bytes_ready_` ran that decode on the UI thread, freezing the window while room avatars loaded at startup. Decode now runs on a worker (`run_async_`) and only the `make_image` + cache store is posted back to the UI thread, with a pre/post cache check to avoid a double-store when concurrent fetches race on the same key.
- refactor(shells): hoist shared shell logic into `ShellBase` — moved the four native shells' duplicated view pointers, Tier-1 event hooks, timeline/message handlers, and provider-lambda factories into `ShellBase` (Qt6 / GTK4 / Win32, with macOS following via the `MacShell` composition). Behaviour-preserving; the shells now differ only in genuinely platform-specific wiring. 475 C++ tests pass.
- perf(roomlist): cache space children asynchronously — `Client::space_children()` calls `rt.block_on()` internally, parking the UI thread on a SQLite read on every `refresh_room_list()` (up to twice per refresh when spaces are present) and producing visible jank on large room lists. A new `ShellBase::update_space_children_cache_()` spawns one worker after each `rooms_` update, fetches all space children off-thread, and posts results back, triggering a re-render only when the cache actually changed.
- perf(anim): stop full-window repaints for animated images — animated GIF/sticker/emoji images drove the 60 Hz frame timer to repaint the whole surface forever, because `anim_cache_` is never evicted and the timer only stopped when it was empty. Two fixes in the shared `AnimImageCache`: (1) a visibility-gated timer — `current_frame()` stamps a last-seen time, `advance()` only advances entries seen within a grace window, and `any_visible()` lets each shell stop the timer once nothing animated is on screen; (2) partial repaints — `PaintCtx` gains an optional `AnimDamageSink` so views report the on-screen rect of each animated image and the Qt6 / macOS hosts invalidate just those regions (`update(QRect)` / `setNeedsDisplayInRect:`). GTK4 drops the wasteful per-frame relayout (no partial invalidation API); Win32's D2D swap-chain present is unchanged.
- fix(session): persist rotated OAuth refresh tokens to the secret store — the synchronous save-session callback still wrote to `accounts/<uid>/session.json`, which after the `SecretStore` migration holds only a sentinel that `load_account()` ignores. On a homeserver that rotates refresh tokens (MAS), an unclean shutdown that aborted the async `TokensRefreshed` watcher left the rotated token only in that dead file, so the next launch restored a stale token and got `invalid_grant` → spurious logout. The synchronous callback now routes through a new `persist_session` FFI to `SessionStore::save_account` (the authoritative store), matching the async and `stop_sync` paths; the unused `atomic_save_session` helper is removed.
- fix(qt6): hide native search/composer overlays when the image or video viewer is open — `any_modal_open_()` gated the native text overlays (room search box, composer textarea) only on `ConfirmDialog` and `RoomView` panels, so the fullscreen image/video viewers left those `QWidget`s and their placeholder text floating above the viewer. The viewers are now included in the check.
- fix(session): store SDK account data under `data_dir()` instead of `config_dir()` — per the XDG spec the matrix-sdk SQLite store and session blobs are state/data, so the per-account `accounts/<uid>/` tree (each with `session.json` + `matrix-store/`) and the `accounts.json` index now live under `~/.local/share/tesseract` on Linux; only `app_settings.json` stays in `~/.config/tesseract`. New `tesseract::data_dir()` ([client/src/paths.cpp](client/src/paths.cpp)) resolves `$XDG_DATA_HOME/tesseract` (or `~/.local/share/tesseract`) on Linux and returns `config_dir()` on Windows/macOS (which have no data/config split). `SessionStore::migrate_legacy_layout()` is reworked into a three-state machine — already-migrated, relocate a multi-account `accounts/` tree left under `config_dir()` by older builds (new `migrate_config_accounts_to_data()`, moves the tree first and `accounts.json` last so the index stays a crash-safe completion sentinel, with rollback), or the existing pre-multi-account single-account migration (now writing into `data_dir()`). No Rust or UI-shell changes — shells call `SessionStore::account_dir`/`sdk_store_dir`/`load_index`, which auto-retarget. 3 new C++ tests (config→data relocation, data-dir no-op, `data_dir()` shape).
- fix(macos): unblock the x86_64 build broken by the stale CommandLineTools libc++ — (1) cxx 1.0.194's `cxx.h` unconditionally includes `<ranges>` under C++20, absent from the CommandLineTools libc++ v8000; fixed by substituting the macOS SDK's modern libc++ via `-nostdinc++` + `-isystem` for CXX and OBJCXX. (2) The newer libc++'s `path& operator=(string_type&&)` made `pending_login_temp_dir_ = {}` ambiguous — replaced six sites with `.clear()`. (3) Added the missing `-framework UniformTypeIdentifiers` link. (4) Captured `send_message`'s `tesseract::Result` (explicit `operator bool`) as `auto`, matching the other shells.
- feat(mentions): `@mention` autocomplete, pills, and `m.mentions` — typing `@` in the composer opens a member-filter popup (with `@room`, keyboard nav, click-to-accept) and inserts an inline mention pill: a native chip on Qt6 (`QTextImageFormat`), GTK4 (`GtkTextChildAnchor`), and macOS (`NSTextAttachment`); Win32 inserts registry-tracked plain `@Name` text (a styled chip needs an EDIT→RichEdit migration, deferred). On send the draft becomes a plain `body` (display names), an HTML `formatted_body` with `matrix.to` mention links, and the intentional-mentions `m.mentions` field — derived in the Rust SDK by scanning the outgoing anchors (`@room` sets `mentions.room` and is rewritten to plain text). Received `matrix.to` user links and a literal `@room` render as themed rounded pills via a new `TextSpan` background field, drawn in `MessageListView` for all four canvas backends; clicking a pill opens the user's profile panel (resolved from room members) instead of a browser. Shared `MentionEngine` / `MentionPopup` / `MentionController` + `client/build_mention_message`; wired into the main window and pop-out on all four shells. Verified on Qt6; GTK4 builds clean; macOS + Win32 written but unverified (no toolchain in the dev env). 144 Rust tests + 465 C++ tests.
- feat(timeline): syntax highlighting for fenced code blocks — messages with a `` ``` ``-fenced block now render with per-token syntax colors that respect light/dark mode. New Rust `highlight_code(code, lang, dark)` free-function FFI uses the `syntect` crate (`default-features = false, features = ["default-fancy"]` — pure-Rust regex, no oniguruma C dep) to tokenize and return per-run RGB from a light (`InspiredGitHub`) or dark (`base16-ocean.dark`) theme; unknown/absent languages return empty so the caller falls back to plain monospace. The C++ wrapper `tesseract::highlight_code` (`client/src/highlight.cpp`) adds a bounded, thread-safe LRU keyed by `(dark, lang, code)` because `html_to_spans` reruns on every measure/paint. `markdown_to_html` now emits `<pre><code class="language-X">` (first info-string token, sanitized to `[a-z0-9+#._-]` so it can't break out of the class attribute) and the shared `html_to_spans` gained a `dark` parameter plus a code-block capture path that reads the `language-X` class, entity-decodes the source, and emits per-token colored `tk::TextSpan`s (new `has_color` / `color` fields). Per-span foreground color is implemented in all four canvas backends: Qt6 (`<span style="color">`), GTK4/Pango (`<span foreground>`), macOS CoreText (`kCTForegroundColorAttributeName`), and Win32 Direct2D (per-range `SetDrawingEffect` brush honored in `CubicEmojiTextRenderer::DrawGlyphRun`). 5 new Rust tests + 11 new C++ tests.
- fix(room): hide the compose input overlay after the active room closes — `clear_room()` dropped `has_room_` so `arrange()` skipped the compose bar, but its cached text-area rect went stale and shells kept overlaying the native input over the brand view. `compose_text_area_rect()` now returns an empty rect when no room is active.
- feat(dm): open an existing DM from the user profile panel instantly — `on_open_dm` now checks the locally cached room list for a direct room whose `dm_counterpart_user_id` matches and navigates synchronously instead of waiting on the async `get_or_create_dm` round-trip; falls back to async creation only when no DM exists. Shared `ShellBase::find_existing_dm` (pure, unit-tested) wired through all four shells. 5 new C++ tests.
- feat(login): account registration via OIDC `prompt=create` — a capability-gated "New here? Create an account" link on the login screen. `oauth::begin` gains a register flag that adds `prompt=create`, and a new `homeserver_supports_registration` probe checks the OAuth server metadata's `prompt_values_supported` for `create`; the shared `LoginView` shows the link only when the resolved homeserver advertises support (generation-guarded probe fired on discovery resolve) and drives the same OAuth browser-loopback routine. No per-shell wiring; legacy `/register` and non-OAuth homeservers are out of scope. (The FFI param is `register_account`, not `register`, since cxx emits it into C++ where `register` is a keyword.)
- feat(roomlist): group inactive rooms into a fifth section — Appearance settings gain a "Room list" group with a "Group inactive rooms" toggle and an inactivity-period selector (1 week–6 months, default 1 month; a local `app_settings.json` preference). When enabled the room list shows a fifth, default-collapsed "Inactive" section holding DMs and Rooms with no activity past the threshold; favorites and spaces are never grouped, the section is hidden when empty, and a room reclassifies out of it automatically when new activity arrives. Classification is a pure, unit-tested `classify_room_section()` helper in the shared `RoomListView`; wired through all four shells.
- feat(notifications): per-room notification settings in `RoomInfoPanel` — a Notifications section with a four-option dropdown (Default / All messages / Mentions / Off) mapped to Matrix per-room push rules (`RuleKind::Override` + `EventMatch` for "off", `RuleKind::Room` for "all"/"mentions", no rule for "default"). New shared `tk::ComboBox` widget — a 32 px dropdown with an overlay popup, lazy `TextLayout` caching, scroll-aware placement via `set_popup_clip()`, and an overridden `contains_world()` so expanded rows outside `bounds_` stay hit-testable. Wired through `RoomView` → `ShellBase::wire_main_app_widget_()` (main window) and `RoomWindowBase::wire_room_view_()` (pop-outs); Rust `client.rs` reads/writes `m.push_rules` via the push-rule API.
- feat(threads): expose Matrix threads (MSC3440) to the C++ level — wires matrix-sdk-ui 0.17 thread support through the cxx bridge and the `client/` layer with no UI changes (the new `IEventHandler` callbacks are default-no-op virtuals). Adds thread metadata on `TimelineEvent`/`Event` (`thread_root_id`, `is_thread_root`, `thread_reply_count`, plus a latest-reply preview on roots); thread-focused timeline subscription (`subscribe_thread` / `paginate_thread_back` / `unsubscribe_thread`, reusing the diff engine via a `TimelineChannel{Room, Thread(root)}` enum + four `on_thread_*` callbacks); a per-room thread list (`ThreadInfo` + `subscribe`/`list`/`paginate`/`unsubscribe`_room_threads wrapping `ThreadListService`, with an `on_threads_updated` ping); and threaded sending (`send_thread_message` / `send_thread_reply` with the `m.thread` relation, plus a `thread_root` param on the five media senders). 130 Rust tests + 416 C++ tests.
- fix(windows): emoji/sticker pickers follow the main window when it moves — the Win32 `WndProc` now handles `WM_MOVING` (drag) and `WM_MOVE` (programmatic/keyboard reposition) and translates visible picker popups by the same delta so they stay anchored to the compose bar.
- fix(ui): room-list row centering, macOS picker popups, larger emoji cells — `RoomListView` now vertically centers the room name in the top half and the preview in the bottom half of each row instead of pinning to fixed padding; the macOS emoji/sticker pickers drop their title/close/utility/HUD window styles to appear as borderless panels matching the other platforms (plus a visibility toggle so the toolbar button dismisses an open picker); and emoji glyph cells move from `FontRole::Title` (14 pt) to a new `FontRole::EmojiPickerCell` (17 pt) across all four canvas backends.
- build(ci): GitHub Actions release packaging — a tag-triggered `package.yml` workflow builds and uploads installers for every platform: Windows NSIS `.exe`, macOS arm64 + x86_64 DMGs, Linux DEB + RPM (CPack) and an AppImage, attached to a GitHub Release on `v*` tags. opus is built from source where no suitable system package exists (Windows MSVC, macOS x86_64 cross-build); the Windows job activates the MSVC environment so the Ninja generator does not fall back to MinGW.

## v0.1.4 — 2026-05-21

Changes since v0.1.3:

### Summary

- feat(notifications): wire the Notifications toggle to the SDK pusher
- feat(settings): "Hide message content in notifications" privacy toggle
- feat(ui): click avatar in `UserProfilePanel` or `RoomInfoPanel` to open full-resolution image in lightbox
- feat(settings): bottom-pinned "About" tab with brand splash
- feat(header): show padlock icon next to room name for encrypted rooms
- feat(ui): hide reaction chip counter when only one person has reacted
- feat(settings): Log Out button at the bottom of the Account settings page
- feat(presence): publish outgoing Matrix presence
- feat(settings): device list in Settings
- feat(ui): DM-counterpart avatar fallback
- feat(client): server capabilities on login
- feat(auth): per-platform secure token storage
- fix(shutdown): drain the `LoginView` homeserver-discovery and `UpConnector` endpoint-scan threads before destruction
- fix(qt6): reset `QMediaPlayer` source between voice/audio clips
- fix(settings): drain `SettingsController` worker threads and the GTK4 `on_logout` callback to prevent a shutdown use-after-free
- fix(timeline): surface undecryptable messages as a single muted line instead of dropping them
- fix(sdk): stop polling presence for users that return 403 Forbidden

### Details

- fix(shutdown): route `LoginView` homeserver-discovery thread and `UpConnector` endpoint-scan through `ShellBase::run_async_` so they are drained before destruction. Two detached `std::thread`s were still live at teardown — `hs_changed_` (300 ms debounce for `discover_homeserver`) and the UP scan thread — causing malloc corruption in `~MessageListView` after `accounts_.clear()` had already freed the client.
- fix(qt6): reset `QMediaPlayer` source between voice/audio clips — `setSourceDevice` short-circuits on an unchanged pointer and replayed the first clip's FFmpeg stream for every subsequent play. Fixed by clearing the source and resetting the `QBuffer`/`QByteArray` before each `play()` call.
- feat(notifications): wire the Notifications toggle to the SDK pusher — `IUpConnector::set_enabled` added so Linux UnifiedPush connectors can remove or re-register their homeserver pusher when the toggle changes; routed through `SettingsController` instead of inline per-shell persistence so all four platforms share the same path. Local OS-notification suppression via `Settings::notifications_enabled` is unchanged.
- feat(settings): "Hide message content in notifications" privacy toggle — when enabled, outgoing notifications title becomes "Tesseract", body becomes "New message", and avatar + image bytes are cleared regardless of the image-preview setting; useful when screensharing. Gate centralised in `ShellBase::apply_notification_redaction_()`. Persisted in `tesseract::Settings`.
- feat(ui): click avatar in `UserProfilePanel` or `RoomInfoPanel` to open full-resolution image in lightbox — fetches original via `fetch_source_bytes`; lightbox image provider falls back to `tk_avatars_` thumbnail for instant display while bytes are in flight; applied to GTK4 / Win32 / macOS shells in the same pass.
- feat(settings): bottom-pinned "About" tab with brand splash — `SideTabView::add_bottom_tab()` anchors a tab group to the sidebar's lower edge, separated from the regular tabs by a thin divider; the About page embeds `BrandView` (app icon, name, version).
- fix(settings): route `SettingsController` worker threads and GTK4 `on_logout` callback through `ShellBase` drain to prevent use-after-free at shutdown; fix `run_async` call-site in GTK4 shell.
- feat(header): show padlock icon next to room name for encrypted rooms — 10×12 px vector padlock drawn to the left of the room name in `RoomHeader` using `text_secondary` colour; name label width reduced so ellipsis truncation still works on long names; condensed mode unaffected.
- feat(ui): hide reaction chip counter when only one person has reacted — chip shows emoji only; count shown from two reactors onward.
- feat(settings): Log Out button at the bottom of the Account settings page — destructive-variant button opens the shared `ConfirmDialog` overlay before firing `on_logout`; `SettingsView` owns its own `ConfirmDialog` instance since it runs on a separate surface.
- fix(timeline): surface undecryptable messages as a single muted line instead of dropping them. The Rust converter in [sdk/src/client.rs](sdk/src/client.rs) now matches `MsgLikeKind::UnableToDecrypt(EncryptedMessage)`, maps the contained `UtdCause` to a single-line padlock message via the new `utd_message_for_cause` helper, and emits a `TimelineEvent { msg_type: "m.utd", body: <reason> }`. The C++ side adds `EventType::Utd` + `UtdEvent` + a `"m.utd"` dispatch arm in `ffi_convert.h`, and `MessageListView` renders the row muted via the existing `paint_wrapped_text` path the redacted tombstone uses. Delete and reaction-add affordances are suppressed on UTD rows; reply still works. Covers all 9 `UtdCause` variants — `SentBeforeWeJoined`, `VerificationViolation`, `UnsignedDevice`, `UnknownDevice`, `HistoricalMessageAndBackupIsDisabled`, `WithheldForUnverifiedOrInsecureDevice`, `WithheldBySender`, `HistoricalMessageAndDeviceIsUnverified`, and the catch-all `Unknown`. New `matrix-sdk-base` direct dep so the converter can name `UtdCause` (matrix-sdk doesn't re-export the crypto types). 1 new Rust test + 1 new C++ test. 118 Rust tests + 412 C++ tests
- fix(sdk): stop polling presence for users that return 403 Forbidden — bridge puppet accounts (e.g. `@_twitterpuppet_*`) often have presence privacy enabled and return `403 Forbidden { kind: Forbidden }` on every `GET /presence/{userId}/status` poll. The polling task now records forbidden user IDs in an `Arc<Mutex<HashSet<OwnedUserId>>>` local to the task and skips them on subsequent ticks. Detected via `HttpError::client_api_error_kind() == Some(ErrorKind::Forbidden)` (matches the existing UIA error-kind pattern used by `delete_devices`); other transient errors (404, 5xx, network) stay retriable. New `is_presence_forbidden` helper + 3 unit tests. 117 Rust tests + 411 C++ tests
- feat(presence): publish outgoing Matrix presence — pairs with the receive-side dots shipped in `d652c95`. New `PresenceTracker` (shared) runs a deadline-driven FSM: Online while engaging with the app, Unavailable after 5 min of no input + no window focus, Offline on logout. Each shell wires (1) `tk::Host::set_on_user_activity` for pointer/wheel events, (2) a focus tap via `changeEvent` (Qt6) / `notify::is-active` (GTK4) / `WM_ACTIVATE` (Win32) / `windowDidBecomeKey,Resign` (macOS), (3) a 30 s `notify_presence_tick_` timer, and (4) `notify_presence_logout_` before `client_->logout()`. Transitions publish via `Client::set_presence(PresenceState)` → new `set_presence` FFI → `ruma::api::client::presence::set_presence::v3::Request` on a detached worker thread (fire-and-forget). 8 PresenceTracker unit tests + 2 Rust set_presence byte-mapping tests. 114 Rust tests + 411 C++ tests
- feat(settings): device list in Settings — new "Sessions" tab lists every device on the account with display name, device id, last-seen IP/time, cross-signing verification badge, and a "This device" marker for the current session. Sign out walks the Matrix UIAA fallback-URL flow: a 401 challenge from `delete_devices` is detected via `as_uiaa_response()`, the row offers "Open in browser" pointing at the spec'd `/_matrix/client/v3/auth/<stage>/fallback/web?session=…` URL (built locally via `build_uia_fallback_url` + `urlencoding_encode_segment`, 4 unit tests), and "I've confirmed" retries with `AuthData::FallbackAcknowledgement{session}`. Verification state is cross-referenced from `client.encryption().get_device(...)`. Rename plumbing (`set_device_display_name` FFI + `Client::set_device_display_name` + `SettingsController::rename_device`) is in place but the per-row inline-edit UI is deferred to a follow-up. New: `DeviceFfi`/`DeleteDeviceBegin` FFI structs, `Client::Device`/`DeviceVerification`/`DeleteDeviceBegin` types, `SettingsController::{load_devices,delete_device,confirm_device_deletion,rename_device}` + per-device in-flight mutex, `DevicesSection` view with DeviceRow chips/action buttons/UIA state machine, `tk::Widget::clear_children` helper. 112 Rust tests + 401 C++ tests
- feat(ui): DM-counterpart avatar fallback — rooms with no avatar of their own now show the other participant's avatar in the room list, room header, room-info panel, and tab strip. Computed in `build_room_infos` (Rust) and piped through a new `dm_avatar_url` field on `RoomInfo` + a `effective_avatar_url()` accessor on the C++ struct; `ensure_room_avatar_` routes the DM fallback through `fetch_media_bytes` (user mxc) instead of the room-avatar endpoint. Functional members (`io.element.functional_members` / MSC4171) are excluded so bridged 1:1s show the puppet's avatar, not the bot's — the same filter also fixes `get_or_create_dm` recognising existing bridged DMs. Counterpart selection prefers `Room::direct_targets()` (m.direct, cheap and in-memory) and falls back to filtering joined members only when `active_members_count - service_members_count == 2`, so large group rooms incur zero extra cost; 380→382 C++ tests, 107/108 Rust tests (the lone failure is a pre-existing image-packs ordering test unrelated to this change)
- feat(client): server capabilities on login — `tesseract::ServerInfo` struct (homeserver URL, spec versions, MSC3030/Jump-to-Date support, `can_change_password`/`can_set_displayname`/`can_set_avatar`, default room version); fetched concurrently via `/_matrix/client/versions` + `/_matrix/client/v3/capabilities` after `RoomListState::Running`; stored in `ShellBase::server_info_` for feature-gating; Settings "Server" tab shows the homeserver URL; 363/363 C++ tests, 108/108 Rust tests
- feat(auth): per-platform secure token storage — `SecretStore` backend using Windows Credential Manager, macOS Keychain, and Linux `libsecret` (plaintext stub fallback when absent); `SessionStore` migrates transparently from legacy plaintext `session.json` on first load (sentinel `{"v":2}` written on success); 357/357 C++ tests

## v0.1.3 — 2026-05-19

Changes since v0.1.2:

### Summary

- feat(download): save file attachments, images, and videos to disk
- feat(voice): MSC3245 voice message send
- feat(pickers): unified async image cache
- feat(ui): the room-list last-message preview shows the first plain line for text and a media summary for image/video/file/voice
- style: standardise formatting across the whole codebase
- refactor(shell): de-duplicated the four native shells into ui/shared/
- fix(tests): update two `ComposeBar` attachment tests to match current floating-preview design
- fix(ui/win32): image/video viewer now closes on Esc immediately

### Details

- feat(download): save file attachments, images, and videos to disk — clicking a file card in the timeline opens a native save dialog and fetches the file from the Matrix media server; a ⬇ button added to `ImageViewerOverlay` and `VideoViewerOverlay` does the same for displayed images and videos; all four platforms (Qt6 `QFileDialog`, GTK4 `GtkFileChooserNative`, Win32 `GetSaveFileNameW`, macOS `NSSavePanel`); bytes fetched off the UI thread via `fetch_media_bytes` / `fetch_source_bytes` and written with `std::ofstream`; 328/328 ctest, 96/96 cargo
- feat(voice): MSC3245 voice message send — mic button in `ComposeBar` starts/stops recording; OGG/Opus encoding in Rust (`audiopus` + `ogg` crates) at 48 kHz mono 32 kbps with MSC1767 waveform sampling (up to 256 samples, normalised [0, 1000]); live waveform strip animates during recording; cancel button; per-platform `tk::AudioCapture` backend (Qt6 `QAudioSource`, GTK4 GStreamer `pulsesrc`, Win32 WASAPI, macOS `AVAudioEngine`); `send_voice` FFI + `Client::send_voice` C++ API; mic button hidden automatically when no capture device is available; voice send wired in all four main shells via new `ShellBase::wire_voice_capture_()` helper; pop-out secondary windows hide the mic button (recording is a singleton interaction owned by the main window); 3 new Rust unit tests + 5 new C++ tests (328/328 ctest, 95/95 cargo)
- fix(tests): update two `ComposeBar` attachment tests to match current floating-preview design — image previews now float above the bar without changing `natural_height()`; only file chips grow it
- fix(ui/win32): image/video viewer now closes on Esc immediately — `SetFocus` to the top-level window on viewer open so its `WM_KEYDOWN` handler receives Esc (previously Esc did nothing until an app deactivate/reactivate restored top-level focus)
- style: standardise formatting across the whole codebase — reconciled STYLE.md's Rust section to idiomatic same-line braces and narrowed Universal Rule §2 (trivial Rust `match` arms stay unbraced); added a repo-root `.clang-format` (Cpp + ObjC sections) encoding the C++/Objective-C rules; reformatted `sdk/` via `cargo fmt` and all C++/ObjC (`client/`, `ui/shared/`, the four shells, `tests/`) via `clang-format` — behaviour-preserving (323/323 ctest, 92/92 cargo)
- feat(pickers): unified async image cache — EmojiPicker/StickerPicker now share the message-list `tk_images_`/`anim_cache_` on all four shells (Qt6 dropped its private per-picker caches), images route through `media_disk_cache_` so custom emoticons/stickers survive an app restart, and decode runs off the UI thread (Qt6 QImageReader / GTK4 GdkPixbuf+cairo / macOS CGImageSource / Win32 WIC) so first paint of a large pack no longer stalls
- feat(ui): room-list last-message preview now uses `formatted_body`'s first plain line for text/notice/emote; renders "`<user>` sent an image/video/file/voice message" for those media kinds; and draws a small inline ~28 px thumbnail for sticker last-messages (SDK exposes `last_message_kind` + `last_message_sticker_url`; `RoomListView` gains a `sticker_provider_` backed by the shells' shared image cache, wired in all four shells)
- refactor(shell): de-duplicated the four native shells into ui/shared/ — ShellBase gains `build_rows_` (+ `prep_row_media_` hook), concrete account-prefs (active-account gate) and image-packs handlers, the `cached_emoticons_` member, and secondary-window dispatch helpers; new shared `tesseract::text::trim` and `ScreenLockState`; `RoomWindowBase::wire_room_view_` absorbs the per-shell secondary-window RoomView wiring behind `surface_repaint_`/`compose_text_area_`/`preview_lookup_` hooks. Behaviour-preserving (323/323 ctest, 92/92 cargo); the UnifiedPush `UpConnectorCore` hoist was deliberately deferred — Qt6/GTK4 endpoint-normalisation are not behaviour-equivalent.

## v0.1.2 — 2026-05-18

Changes since v0.1.1:

### Summary

- feat(messaging): `m.location` / MSC3488 receive
- feat(messaging): optimistic send via SDK local echo
- feat(compose): emoji shortcode expansion
- feat(app): secondary room windows
- feat: light/dark/system theme preference
- feat(notify): image & sticker notification previews with a lock-screen privacy gate
- feat(pickers): emoji + sticker picker shortcode tooltips
- feat(ui): room list redesign
- feat(matrix): `m.notice` renders with muted colour; `m.emote` renders as "* SenderName body" with italic spans
- feat: MSC3030 jump-to-date
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
- feat: overlay scrollbar on GridView (EmojiPicker + StickerPicker)
- feat: MSC4027 custom images in reactions; MSC2448 BlurHash placeholders for media
- feat: clickable inline hyperlinks across all backends
- feat: markdown-to-HTML formatting for sent messages
- Relicense under GPL v3
- fix(linux): Wayland foreground activation via XDG portal
- fix(qt6): dark theme detection on GNOME
- fix(compose): markdown→HTML on send centralised in `Client::send_message/send_reply/send_edit` for all shells + secondary windows
- fix(msc2545): prefer merged stable image-pack event types with unstable fallback
- fix(auth): reject duplicate account sign-in on all four shells
- fix(notify/qt6): use a fresh popup per notification and correctly populate the `image-path` hint
- fix(win32): swallow stale `WM_CHAR('\r')` after Enter submits the compose bar to prevent phantom newline in next reply/edit session
- fix(win32): `NativeTextArea::natural_height()` measures soft-wrapped lines via `DrawTextW(DT_CALCRECT | DT_WORDBREAK …)`
- fix(viewer): image/video overlay backdrop black-on-first-move fixed on all four platforms via transparent Surface/Host propagation
- fix(gtk4): `NativeTextArea` placeholder text via `dim-label` `GtkLabel` overlay
- fix(gtk4): async image fetch for `EmojiPicker` custom emoticon tabs
- fix(map): wire `on_tile_needed` in all four main-window shells and cap the zoom rate at one step per wheel notch
- fix(settings): settings surface receives the current theme immediately on creation
- fix(ui): emoji glyphs centred within their picker grid cells
- fix(message-list): bare URLs in plain-text bodies are now clickable
- fix(reply): pass `event_id` (not `in_reply_to_id`) to reply detail resolver
- fix(sdk): build room timeline on a worker thread to avoid stack-overflow crash on macOS
- fix(win32): compile cleanly under `/std:c++20` on SDK 19041

### Details

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

### Summary

- URL previews + inline hyperlink rendering (OpenGraph card; row-height invalidation on arrival)
- UnifiedPush server pusher for Linux (Qt6 + GTK4): D-Bus connector and endpoint rewrite to `/_matrix/push/v1/notify`
- Typing indicators: send `m.typing` and display incoming
- Read receipts: public `m.read` send; mark rooms read on open (`m.read` + `m.read.private`)
- Inline markdown rendered from `formatted_body`; day separators + virtual timeline items
- Emoticon image loading, picker tab scrolling, compose-bar height fix
- Sticker fixes: saved-pack visibility, aspect ratio, right-click viewer, dedupe-by-URL on save
- Qt6: transparent native text overlays
- Fix use-after-free crash when selecting a room; fix two pre-existing test failures
- Three UI fixes: compose icons inside input, search clear button, play triangle

### Details

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

### Summary

- Initial C++/Rust scaffold; renamed to Tesseract; OAuth/MAS loopback login; CLAUDE.md + Catch2 test framework
- matrix-sdk 0.16.1 → 0.17.0; refresh-token handling; tokio Drop context for client swap
- Step 2 — sliding sync (SyncService + RoomListService) replacing `sync_once`; per-room Timeline map; subscribe/unsubscribe/paginate FFI
- SQLite-backed timeline persistence via EventCache
- Session: restore-or-login, full PersistedSession on token refresh, and UnknownToken / soft_logout handling
- Multi-account support (infrastructure + all four shells); per-account notifier; restore last room on switch
- Prefs stored as Matrix account-data (`im.gnomos.tesseract`)
- `ShellBase` + `EventHandlerBase` refactor extracted from all four shells
- Background backfill (all rooms, limited to visible), cancellable media fetches, and shutdown-stability hardening
- i18n for Qt6 + GTK4; device rename to hostname after OAuth (`device_display_name`)
- Shared `tk::Canvas` / `tk::Widget` / `tk::Host` toolkit across all four platforms (Direct2D / QPainter / Cairo / CoreGraphics)
- Native text overlays; ListView scrollbar drag; `tesseract::Settings`; CoreGraphics + D2D test surfaces
- macOS port: AppKit → Mac Catalyst → native AppKit
- DirectWrite colour-emoji text on Windows; Twemoji fallback
- Message rows: sender identity + avatars, grouping, day separators, redactions, editing, reply-to, reactions, and read receipts
- Media: inline images/stickers, MSC2545 image packs, MSC3245 voice messages, and `m.video` receive + playback
- Text: Markdown→HTML on send; inline markdown render from `formatted_body`; URL previews + inline hyperlinks
- Navigation: spaces drill-in; room search + 500 ms debounce + activity sort; collapsible room-list sections; favorites
- Encryption: device verification (SAS) + key-backup recovery (Step 6); recovery + verification banners
- Notifications: Linux (D-Bus), macOS (UNUserNotificationCenter), Win32 (WinRT toast), system tray, and UnifiedPush
- App icons (Win32 `.ico` / macOS `.icns` / GTK4 / Qt6) generated from a shared SVG
- CPack installers
- MinGW cross-compile support; `WHOLE_ARCHIVE` link for the 3-way FFI cycle; bundled SQLite (rustls, no system OpenSSL)

### Details

#### Core / SDK

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

#### UI toolkit (`tesseract_tk`)

- Shared `tk::Canvas` / `tk::Widget` / `tk::Host` toolkit across all four platforms (Direct2D / QPainter / Cairo / CoreGraphics)
- Native text overlays; ListView scrollbar drag; `tesseract::Settings`; CoreGraphics + D2D test surfaces
- macOS port: AppKit → Mac Catalyst → native AppKit
- DirectWrite colour-emoji text on Windows; Twemoji fallback

#### Features

- Message rows: sender identity + avatars, grouping, day separators, redactions, editing, reply-to + scroll-to-original, reactions + MSC4027 custom-image reactions, read receipts + hover timestamps, typing indicators
- Media: inline images/stickers, MSC2545 image packs (sticker/emoji pickers, encrypted, animated GIF/WebP/APNG), MSC3245 voice messages, `m.video` receive + playback, `m.file` drag-and-drop, clipboard image paste + MSC2530 captions
- Text: Markdown→HTML on send; inline markdown render from `formatted_body`; URL previews + inline hyperlinks
- Navigation: spaces drill-in; room search + 500 ms debounce + activity sort; collapsible room-list sections; favorites
- Encryption: device verification (SAS) + key-backup recovery (Step 6); recovery + verification banners
- Notifications: Linux (Qt6/GTK4 D-Bus), macOS (UNUserNotificationCenter), Win32 (WinRT toast); system tray + minimize-to-tray on all four; UnifiedPush server pusher (Linux)

#### Build & packaging

- App icons (Win32 `.ico` / macOS `.icns` / GTK4 / Qt6) generated from a shared SVG
- CPack installers — NSIS (Windows) + DMG (macOS); Debian + Arch packaging helpers
- MinGW cross-compile support; `WHOLE_ARCHIVE` link for the 3-way FFI cycle; bundled SQLite (rustls, no system OpenSSL)
