# Tesseract — Implemented Features

Snapshot of every feature that has landed on `master`. Last updated **2026-06-10** (v0.8.0).

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

> **In-flight request indicator.**
> A small coloured dot in the status bar shows the number of currently
> in-flight Matrix API requests: green for 0–1, amber for 2–10, red for
> more than 10. A tooltip on the dot shows the exact count. Wired on Qt6,
> GTK4, Win32, and macOS. The macOS shell receives its status bar in the same
> pass (it previously had none), giving all four platforms parity for the
> sync-state label and in-flight dot. Every extra-sync media fetch is bounded
> by a per-request timeout (30 s thumbnails/avatars, 120 s full media) so a
> stalled or endlessly-retrying request can't pin the dot or a worker thread,
> and the displayed count is re-read from the authoritative Rust atomic
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
| Rust unit tests (`cargo test -p tesseract-sdk-ffi`) | 208 |
| C++ Catch2 tests via ctest (Qt6 preset) | 742 |

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
- **Tombstoned (upgraded) rooms hidden** from the room list.
- **Runtime offline banner** — when sync loses connectivity (`sync_offline` / `sync_error`), a 32 px amber "No internet connection — reconnecting…" strip appears at the top of the chat panel; it auto-hides when `RoomListState` returns to `Running`. `ShellBase::offline_` tracks the flag; `EventHandlerBase` wires both transitions; `MainAppWidget::set_offline(bool)` drives the banner. All four shells benefit with no per-shell changes.
- **Graceful shutdown** — `Drop` on `ClientFfi` calls `stop_sync()`.
- **Non-blocking FFI lock (room-switch freeze fix)** — the C++ `Client` no longer serialises every FFI call behind one coarse `std::mutex` held across blocking `block_on`s. The read + dispatch bridge methods are now `&ClientFfi` (interior-mutable Rust state: `thread_lists` / `thread_timelines` moved behind `parking_lot::RwLock`), guarded by a `std::shared_mutex` taken in shared mode; only ~15 genuine writers (`start_sync`, `restore_session`, `logout`, …) take the exclusive lock. The UI thread's cheap room-switch reads (`list_room_threads`, `subscribe_room_threads`) now run concurrently with a worker mid-`subscribe_room` timeline build instead of freezing behind it.
- **Low-power CPU optimisations** — the sync worker no longer fans out into matrix-sdk SQLite queries on every notable update. The room-info watcher coalesces `RoomInfoNotableUpdate` bursts in a 150 ms window and folds their reasons, skipping the image-pack/prefs rebuild when only read-receipt / latest-event / recency bits are set. `sync_room_subscriptions` is diff-aware — a re-selection of the already-open room or a thread toggle that lands in an already-subscribed room is a no-op. The presence polling loop reads a cached DM-counterpart set (refreshed from `RoomInfo.dm_counterpart_user_id` after every room-list rebuild) instead of walking every joined room with a `dm_other_user` lookup per tick, the tick interval is raised from 30 s to 60 s, and the loop is suspended entirely while the window is hidden/minimized/unfocused (re-enabled with an immediate one-shot kick on focus regain via `Client::poll_presence_now`). On low-end laptops these collapse a previously dominant `chunk_large_query_over` hotspot.

## Spaces (Step 7)

- `is_space: bool` on `RoomInfo`; spaces shown at the bottom of the room list with `#` prefix on Qt6 / GTK4 (top-row dedicated bar on macOS).
- `space_children(space_id)` FFI returning joined direct children.
- **Stack-based drill-in navigation** — selecting a space replaces the room list with its children; `←` back button + space name label at the top of the sidebar; recursive sub-spaces; auto-pop to "All rooms" when the stack is empty.
- **Space children hidden from the root room list** — they appear only when navigating into the parent.

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

## Media

- **`fetch_media_bytes(mxc)`** / **`fetch_source_bytes(source_json)`** — synchronous wrappers around matrix-sdk's media cache; the latter handles plain mxc + encrypted `EncryptedFile` transparently.
- **Avatars** — sender avatars (24 px per row) + room avatars (36 px); circular crop via `draw_circle_image`; initials-disc fallback when bytes aren't yet cached. Rooms without a custom avatar fall back to the *other participant's* avatar in 1:1 chats (`RoomInfo::dm_avatar_url`, populated in Rust by inspecting `m.direct` first and then filtering joined members by `service_members` per MSC4171); render sites read via the inline `effective_avatar_url()` accessor and `ShellBase::ensure_room_avatar_` routes the DM-fallback fetch through `fetch_media_bytes` so the cache key naturally dedupes with the user's avatar elsewhere.
- **Lazy room-list avatars** — room-list avatars are requested only when a row is first painted (`RoomListView::on_room_avatar_needed` fires from `paint_row` on a cache miss, wired to `ensure_room_avatar_` in `ShellBase::wire_main_app_widget_`), so rooms in collapsed or off-screen sections fetch nothing until scrolled into view. The former per-shell "fetch every room" loops are gone.
- **Bounded fetches** — every media download runs under a per-request timeout (30 s thumbnails/avatars, 120 s full files), so a stalled or endlessly-retrying request can't hang a read-pool worker thread or pin the in-flight indicator.
- **Failed-fetch backoff** — a fetch that returns empty (network error / 5xx / timeout) is recorded in a per-key exponential-backoff cache (30 s → 30 min); the `ensure_*` avatar/media paths skip a key still in cooldown, so an unreachable avatar (e.g. a forgotten DM on a dead homeserver) stops being re-requested on every sync tick. Cleared on success and on cache-wipe.
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

- **`sdk/src/image_packs.rs` aggregator** — user pack (`im.ponies.user_emotes` / `m.image_pack`), enabled-rooms list (`im.ponies.emote_rooms` / `m.image_pack.rooms`), per-room state events (`im.ponies.room_emotes` / `m.room.image_pack`). 16 unit tests.
- **Spec-correct usage semantics** — missing/empty `usage` → both sticker + emoticon allowed; per-image `usage` overrides pack-level.
- **FFI surface** — `list_image_packs`, `list_pack_images`, `list_favorite_stickers`, `send_sticker`, `save_sticker_to_user_pack`, `user_pack_has_sticker`, `toggle_favorite_sticker`.
- **`IEventHandler::on_image_packs_updated`** — fires whenever the cache is rebuilt; pickers refresh in place.

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
- **Cross-platform CMake presets** — `windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-qt6-debug`, `linux-qt6-release`, `macos-appkit-{arm64,x86_64}-{debug,release}`.
- **CPack installer packaging** — NSIS on Windows, DMG on macOS (see [PACKAGING.md](PACKAGING.md)).
- **Bundled SQLite** via matrix-sdk's `bundled-sqlite` feature; no system OpenSSL dep (TLS uses rustls).

---

## Maintenance note

Update this file after every major feature lands — append a new bullet (or extend an existing one) in the right category, refresh the test counts in the table at the top, and bump the "Last updated" date.
