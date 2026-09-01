# Tesseract — Implemented Features

Snapshot of every feature that has landed on `main`. Last updated **2026-08-31** (v0.8.19). 1577 C++ + 615 Rust tests.

> **Markdown tables render as an aligned grid (2026-08-31, v0.8.20).**
> Tables draw as a column-aligned grid — cell borders, a tinted and ruled
> header row, per-column width sizing, and honored column alignment
> (`|:--|`, `|:-:|`, `|--:|`). Wide tables shrink, then wrap, then clip to
> the message width. Text is selectable inside a cell and across cells (a
> rectangular block); copy yields plain text or tab/newline TSV. The HTML sanitizer passes
> `text-align` through on `<td>`/`<th>` (canonicalized, no other CSS) so
> alignment survives from other clients. Linux (Qt6 + GTK4) verified;
> macOS/Windows share the code, unbuilt.

<!-- -->

> **jemalloc is the Linux process allocator (2026-08-30, v0.8.19).**
> The Rust SDK links jemalloc as a `#[global_allocator]`, built
> `unprefixed` so it also interposes `malloc`/`free` for the C++ side
> (Qt/GTK, bundled SQLite, GStreamer), and tuned
> (`background_thread:true`, 5 s decay) to return freed pages to the OS.
> Replaces stock glibc malloc, whose per-arena retention under this app's
> thread count let RSS climb to ~1 GB while the live heap stayed ~266 MB.
> `TESSERACT_ENABLE_JEMALLOC`, default on, Linux only. Build + test suite
> verified; live RSS impact pending measurement.

<!-- -->

> **Room media gallery backed by a persistent index (2026-08-30,
> v0.8.19).** The full-screen room media view no longer re-scans the SDK
> timeline every time it opens. A per-room `room_media` table in
> `app_cache.db` is seeded once (lazily, on first open) from the SDK's own
> event-cache store — no network — and kept current from the same timeline
> diff stream the search index uses. The gallery paints its newest page
> instantly from SQLite and pages older history from the index, only
> falling through to network back-pagination once the index is drained.
> The "Media (N)" badge now counts all synced history. Image + video only.
> Linux-verified; other shells benefit unchanged but unverified.

<!-- -->

> **Decoded-image caches garbage-collected to the visible set (2026-08-30,
> v0.8.19).** Avatars and inline thumbnails are kept decoded only while
> on screen (plus a short grace), reclaimed by a generational
> mark-and-sweep that freezes whenever the user is idle — replacing a
> fixed TTL plus per-row pinning that let the decoded-bitmap heap grow
> unbounded (~200 MB over 80 s in a Qt6 heaptrack). A new in-RAM
> compressed-bytes tier keeps scroll-back re-decode instant. Qt6-verified;
> GTK4/macOS/Windows unverified.

<!-- -->

> **"Clear all caches" actually clears everything now (2026-08-30,
> v0.8.19).** Wipes the account's state, event-cache and media stores,
> closes tabs and pop-out windows, and re-fetches the room list from a
> fresh sync — keeping the crypto store and session, so no re-login or
> re-verification. Refuses while a call or device-verification is in
> progress. Room-list re-fetch after a wipe never worked before (the
> sliding-sync cursor lives in the crypto store, not the state store);
> `SyncService::expire_sessions()` now resets it. User-verified on Linux;
> GTK4/macOS/Windows pending.

<!-- -->

> **Ctrl+Tab / Ctrl+Shift+Tab MRU room switcher (2026-08-27,
> v0.8.18).** Alt-Tab-style recent-room cycling: hold Ctrl, tap
> Tab/Shift+Tab to move through recent rooms, release to jump, Escape to
> cancel. User-verified on Windows, Qt6, GTK4, and macOS.

<!-- -->

> **DPI / HiDPI display-scale awareness across all four platforms
> (2026-08-24, v0.8.18).** The UI now detects live DPI/backing-scale
> changes on all four platforms and rescales accordingly — native text-field
> image captures, avatar/thumbnail fetch size, popups, and fonts all track
> the new scale instead of going stale or blurry. Linux verified against the
> full test suite; Win32/macOS written to the same pattern but unverified.

<!-- -->

> **Read-receipt timestamps and overflow grid popup (2026-08-24,
> v0.8.18).** Read receipts now show a per-reader timestamp on
> hover, and a "+N" pill opens a scrollable grid popup listing every reader
> beyond the inline avatar cluster. Also fixes layout overlap between the
> receipt cluster and nearby hover-toolbar/pending-send/message-body UI.

<!-- -->

> **Idle-TTL eviction for warm room/thread timelines (2026-08-07,
> v0.8.18).** A 30-minute idle timer now evicts any room/thread
> timeline that isn't actually on-screen, even if it's a tab, favorite, or
> pinned pop-out — bounding memory that the warm-subscription LRU previously
> exempted indefinitely.

<!-- -->

> **Custom Windows 11-style title bar (2026-08-26, v0.8.18).**
> A self-drawn extended title bar on `MainWindow` and pop-out `RoomWindow`s
> matching the app's Mica/dark-caption theming, with custom caption buttons,
> Snap Layouts, and right-click system menu all preserved. Title text renders
> through BetterText for proper color-emoji support, and the window title now
> reflects the active room on all four platforms (later unified behind a
> single shared `ShellBase::compose_window_title_()`). User-verified on Qt6,
> GTK4, and macOS.

<!-- -->

> **DirectComposition presentation (2026-08-26, v0.8.18).**
> Windows surfaces now present via DirectComposition instead of a plain DXGI
> swap chain, fixing a visible "stretch" artifact during interactive window
> resize.

<!-- -->

> **Progressive video streaming (2026-08-26, v0.8.18).**
> Fast-start MP4/MOV videos now begin playing while still downloading, on all
> four platforms, with a draggable buffered-range scrub bar and disk caching
> of completed downloads. Also fixes the video/image lightbox not cancelling
> its in-flight fetch on close (audio could keep playing after close), a
> GTK4 pipeline bug that broke all video playback, and a macOS streaming
> loader that stalled forever waiting on content length. User-verified on
> GTK4 and macOS.

<!-- -->

> **In-thread search (2026-08-25, v0.8.18).** Adds find/search
> scoped to a single thread, plus a persistent filter field on the thread
> list, and dims the main timeline whenever the thread-list panel is open.

<!-- -->

> **Room knocking (MSC2403) (2026-08-15, v0.8.18).** Lets a user
> request to join a knock-restricted room, track and cancel a pending
> request, and lets moderators review and accept/deny/deny-and-ban requests —
> wired uniformly across all four platform shells. Also fixes federated
> knocks failing silently due to a missing server routing hint.

<!-- -->

> **Full room-history export (2026-08-16, v0.8.18).** Exports a
> room's complete message history to plain text or HTML (optionally with
> images, optionally zipped), with resumable checkpointing and progress UI.
> A later pass added a time-range selector and a "Stop & save" action, and
> restyled the HTML export to match the live timeline's appearance.
> User-verified on Windows, Qt6, and macOS; GTK4 still unverified.

<!-- -->

> **Optional local crash handler (2026-08-13, v0.8.18).**
> An opt-in, off-by-default crash handler (Settings → Advanced →
> Diagnostics) writes a plain-text native/Rust-panic stack trace to disk,
> with nothing transmitted anywhere.

<!-- -->

> **Per-room compose draft persistence (2026-08-14, v0.8.18).**
> Unsent compose text, a staged attachment, and the caret position now
> persist per room instead of being cleared on every room switch.

<!-- -->

> **Windows D2D rendering reliability: flip-model presentation and
> device-loss/hang recovery (2026-08-20 through 2026-08-24,
> v0.8.18).** Fixes a class of bugs where the Win32 window would
> silently stop repainting with no crash or error, by switching the main
> window to flip-model swap-chain presentation and correctly detecting and
> recovering from device loss/hang.

<!-- -->

> **Use MSC4491 atomically when the homeserver advertises support
> (2026-08-06, v0.8.18).** When a homeserver advertises
> [MSC4491](https://github.com/timedoutuk/matrix-spec-proposals/blob/timedoutuk/create-room-invite-reason/proposals/4491-create-room-invite-reason.md),
> room/DM creation now attaches the invite reason atomically inside the
> `createRoom` call itself, instead of the two-step create-then-invite
> fallback.

<!-- -->

> **Optional invite reasons: room creation, `/invite`, quick-switcher DMs,
> and display on receipt (2026-08-06, v0.8.18).** Room creation,
> `/invite`, and the quick switcher's DM flow now accept an optional invite
> reason, sent as plain text even in encrypted rooms; a received reason also
> renders on the invite card. Implemented via the stable per-invite reason
> field, since no homeserver yet supported MSC4491 at the time.

<!-- -->

> **Native text controls render into the canvas instead of overlaying it
> (2026-08-05, v0.8.18).** `NativeTextField`/`NativeTextArea` now
> render into the canvas via an offscreen image capture instead of floating
> on top of it, so they participate in clipping, opacity, and paint order
> like any other canvas content. Text input is still backed by real native
> controls for IME/selection; only painting moved into the canvas.

<!-- -->

> **Windows: MSIX packaging (Store + direct) alongside NSIS, and taskbar
> shell integration (2026-08-01 – 2026-08-05, v0.8.18).** Adds
> MSIX packaging for both Microsoft Store and direct distribution alongside
> the existing NSIS installer, plus deep taskbar integration: unread/mention
> overlay icons, thumbnail-toolbar controls, upload progress, and Jump Lists
> across all top-level windows.

<!-- -->

> **Linux: MPRIS, GNOME Shell search provider, and KRunner integration;
> macOS: Now Playing and Spotlight search (2026-08-02 – 2026-08-04,
> v0.8.18).** Voice/audio messages now expose MPRIS media
> controls on Linux and Now Playing controls on macOS. Room/contact search
> is also surfaced through GNOME Shell's search provider, a KRunner plugin,
> and macOS Spotlight.

<!-- -->

> **Linux: Flatpak/Flathub manifest and AUR packaging hardening
> (2026-07-31 – 2026-08-03, v0.8.18).** Adds a Flathub-ready
> Flatpak manifest (offline-vendored Rust deps) and hardens the AUR
> `tesseract-matrix`/`tesseract-matrix-git` PKGBUILDs (license, arch, LTO,
> dependency fixes), both validated with real local builds.

<!-- -->

> **Linux: GTK4 rendering fixes; single-instance guard shared with Qt6
> (2026-07-31, v0.8.18).** Fixes several GTK4 rendering bugs —
> stalled sticker/emoji animations, a large avatar-scaling CPU cost, missing
> room-list text ellipsis, a stuck pagination spinner, and a lingering
> placeholder — and adds a shared single-instance guard so launching one
> build while the other is already running gets forwarded instead of opening
> a second instance against the same store.

<!-- -->

> **Windows: hide `main_app_surface_` until session restore completes
> (2026-08-04, v0.8.18).** Fixes the Windows shell briefly showing
> the room-list sidebar alongside the branding/login page before session
> restore completes, matching the other three platforms' behavior.

<!-- -->

> **Per-image scaled-surface LRU cap raised from 4 to 8
> (2026-08-03, v0.8.18).** Raises the per-image pre-scaled-surface
> cache size on the GTK/Cairo and Qt backends, fixing repaint thrashing for
> avatars drawn at many different sizes across the UI.

<!-- -->

> **Floating date badge in the message timeline (2026-07-30, v0.8.17-unreleased).**
> A small pill fixed to the top-center of the timeline shows the date of
> whatever day is currently scrolled to, appearing and disappearing together
> with the scroll-to-bottom pill.

<!-- -->

> **Redundant network fetches on room switch fixed (2026-07-30,
> v0.8.17-unreleased).** Revisiting an already-open room no longer re-fetches
> a fresh page of history or re-runs other per-switch work on every visit —
> each now runs once per warm subscription.

<!-- -->

> **Desktop notification quick-reply, across all four shells (2026-07-30,
> v0.8.17-unreleased).** Desktop notifications now support replying inline
> without opening the app, on Windows (toast), macOS, and Linux (KDE
> inline-reply and the portal's standard reply action). Replies send as
> proper threaded replies. Also fixes a concurrency bug where Linux
> notifications could permanently wedge after the first one.

<!-- -->

> **Launch-at-login support, across all four shells (2026-07-30,
> v0.8.17-unreleased).** A new Settings → General toggle (default off)
> registers the app with each OS's native login-item mechanism; a launch via
> autostart starts hidden to the tray only when a saved session restores
> silently.

<!-- -->

> **`PopupMenu` migrated to a native popup surface (2026-07-29,
> v0.8.17-unreleased).** `PopupMenu` now renders as a genuine OS popup
> window instead of a canvas overlay, fixing it never being able to z-order
> above native text controls. Also fixes a menu not closing on outside
> click, a frozen entrance animation, and a menu left floating after
> alt-tab.

<!-- -->

> **`matrix-sdk` init hardened; new sessions' local store encrypted
> (2026-07-29, v0.8.17-unreleased).** Bounded request retries/timeout and
> auto-enabled key backup alongside cross-signing bootstrap. New logins now
> encrypt the local SQLite store with a per-session key held in the
> platform secret store; existing sessions stay unencrypted since matrix-sdk
> has no in-place migration path.

<!-- -->

> **Two pop-out room window bugs fixed (2026-07-29, v0.8.17-unreleased).**
> Popping a room out no longer leaves it appearing open in the main window
> at the same time, and closing the only open tab now deselects to the
> empty state instead of refusing to close. Also fixes the pop-out compose
> bar's native text field showing through overlays that should cover it.

<!-- -->

> **Right-click context menu on room list rows (2026-07-29,
> v0.8.17-unreleased).** Open in tab / Open in window / Leave room (with
> confirmation), with the open-in items disabled when the room is already
> open in that context.

<!-- -->

> **Image-pack editor scroll/field bugs fixed (2026-07-29, v0.8.17-unreleased).**
> Fixes three bugs in the sticker/emoji image-pack editors: the personal
> pack editor's shortcode field never appeared at all, the room/space
> editor's shortcode field didn't track list scrolling, and beginning an
> edit could visibly jump-scroll the pack list.

<!-- -->

> **Per-class lifetime guards unified onto `tk::EnableWeakSelf<T>`
> (2026-07-29, v0.8.17-unreleased).** Replaces ~16 independent ad hoc
> shared_ptr/weak_ptr lifetime-guard patterns across the codebase with a
> single shared mixin, and fixes three confirmed unguarded async
> use-after-free gaps found along the way.

<!-- -->

> **Room-list rebuild skipped on unchanged presence
> (2026-07-28, v0.8.17-unreleased).** The room list no longer does a full
> rebuild on every presence poll tick when the polled state hasn't actually
> changed.

<!-- -->

> **Flash-highlight on quote/jump scroll (2026-07-28, v0.8.17-unreleased).**
> Reply-quote clicks and other jump-to-message scrolls (search results,
> thread reveal, pinned banners) now flash-highlight the destination row so
> it's clear which message you jumped to.

<!-- -->

> **Fixed the startup splash freezing during account restore
> (2026-07-28, v0.8.17-unreleased).** The animated splash screen could
> freeze solid throughout account restore on all four platforms. Also moves
> a synchronous image-pack cache rebuild and session restore/`start_sync()`
> off the UI thread, and caps each restored account's Tokio runtime at 2
> worker threads instead of defaulting to the core count.

<!-- -->

> **Gallery pagination ported to `RoomPane` for pop-out windows
> (2026-07-28, v0.8.17-unreleased).** Pop-out room windows now share the
> same media-gallery backward-pagination implementation as the main window
> instead of lacking gallery pagination entirely.

<!-- -->

> **macOS: fixed a GIF-strip color-channel swap
> (2026-07-28, v0.8.17-unreleased).** Fixes an intermittent red/blue
> channel swap during GIF-strip playback on macOS, caused by a sample
> buffer aliasing pooled storage.

<!-- -->

> **`RoomPane`: main-window and pop-out room wiring unified
> (2026-07-27/28, v0.8.17-unreleased).** Consolidates near-identical
> per-room display/composer/send-edit-react-pin wiring that was duplicated
> across the main window and pop-out windows on all four shells into a
> shared `RoomPane` class. Fixes several real bugs surfaced by the
> consolidation: pop-out composer popups not auto-dismissing, blocking
> media sends, silent failures on send/edit/topic-save, and a stale
> leave-room handler on Win32, among others.

<!-- -->

> **Client-side video-thumbnail generation hardened
> (2026-07-26, v0.8.16).** Fixes videos with no server-supplied thumbnail
> getting stuck showing only a play-button placeholder, and generated
> thumbnails not persisting to the disk cache. Adds a real Windows
> first-frame generation path and an authenticated Range-GET prefix fetch
> so all four platforms can generate a preview without a full download.

<!-- -->

> **Startup account restore no longer blocks the UI thread
> (2026-07-26, v0.8.16).** Restoring saved accounts at startup no longer
> freezes the window; it now runs off the UI thread with a live
> "Restoring session…" status shown during the process.

<!-- -->

> **`BrandView` hypercube wireframe background
> (2026-07-26, v0.8.16).** Adds a faint, continuously-rotating 4D hypercube
> wireframe behind the icon/name/version stack on the splash/branding
> screen, on all four backends.

<!-- -->

> **MSC4391 in-room bot commands (2026-07-25, v0.8.16).** Bot commands
> declared via `m.bot.command_description` now appear in `/command`
> autocomplete alongside built-in commands, with a guided argument-entry
> flow that sends structured invocations. Qt6/GTK4 fully verified; AppKit's
> pop-out window has parity but its main window doesn't yet share it.

<!-- -->

> **Adaptive narrow-window layout (2026-07-24, v0.8.16).** Below a 600px
> breakpoint, the room-list/room-view split collapses into a single pane
> with a back button (and Escape) to return to the list. Also adds a real
> minimum window width derived from the compose bar's own footprint, and an
> overflow menu for room-header actions that don't fit.

<!-- -->

> **Native context menus now follow the app theme; multi-language pronoun
> tooltip (2026-07-23, v0.8.16).** Native context menus (user-info, message
> Copy, sticker save) now follow the in-app theme selection instead of the
> OS theme, on Qt6 and Windows. The Pronouns row now shows a tooltip listing
> every configured language/pronoun pair. Also hardens Linux shutdown
> (a blocking self-pipe socket, an uninstalled GTK4 shutdown handler) and
> makes blocking SDK sends cancellable so Ctrl+C mid-send doesn't wait out
> the network timeout.

<!-- -->

> **Multi-language pronoun editor; searchable timezone picker
> (2026-07-22, v0.8.16).** The pronouns field becomes a repeatable
> per-language editor matching MSC4247's multi-entry shape, and the
> free-text timezone field is replaced by a searchable picker generated
> from real tzdata. Membership-narration text now resolves the acting
> user's declared pronoun. Also fixes a profile-field save silently
> reverting a sibling field, and gates the room-header call button on
> actual power level.

<!-- -->

> **Combined Join/Create "Add Room" dialog (2026-07-21, v0.8.16).**
> Replaces the separate per-platform Join Room dialog with a shared overlay
> and adds a previously-missing Create Room flow, combined into one tabbed
> `AddRoomView` modal. Also fixes an avatar picker showing initials instead
> of the picked image until reopened, and a few popup-focus/resize
> glitches.

<!-- -->

> **Trackpad momentum (kinetic) scrolling, all four platforms
> (2026-07-20, v0.8.16).** Scrollable views now ease to a stop with
> momentum after a trackpad gesture instead of jumping by discrete wheel
> deltas, on all four platforms. Also includes a visual polish pass:
> stronger active-room highlight, removed row separators, square compose
> button icons, and in-window (non-native-popup) emoji/sticker pickers.

<!-- -->

> **Media-caption editing; livekit build fix; emoji font packaging
> (2026-07-19, v0.8.16).** Editing Image/File/Video captions now preserves
> the original media instead of rebuilding the event as plain text.
> Separately fixes an intermittent livekit build failure and declares an
> emoji font as a runtime dependency on Linux packaging.

<!-- -->

> **"Developer mode" setting; global toast system
> (2026-07-17, v0.8.16).** Adds an off-by-default "Developer mode" setting
> that enables a "Copy event source" message-menu item. Also consolidates
> three near-duplicate toast-notification implementations into one shared
> `tk::Host` mechanism.

<!-- -->

> **MatrixRTC calls always-on (2026-07-17, v0.8.16).** MatrixRTC voice/video
> calls are now a permanent part of every build on all four platforms
> instead of an opt-in CMake flag, removing all the associated `#ifdef`
> gates. Also fixes a link-order bug that broke the Windows/Linux/macOS
> calls-only libraries once they became unconditional.

<!-- -->

> **Tab traversal now follows widget geometry, not insertion order
> (2026-07-17, v0.8.16).** Tab/Shift-Tab order is now computed from actual
> on-screen position (reading order) instead of child-insertion order, so
> grids, pickers, and reordered widgets now tab in visual order.

<!-- -->

> **Fixed unfocused widgets reacting to stray keys; theme-picker gained
> real keyboard access (2026-07-17, v0.8.16).** Several widgets
> (`ComboBox`, `Button`, `CheckButton`, `SwitchButton`, `ListView`,
> `GridView`, `TabBar`) could react to a keypress while unfocused elsewhere
> in the tree; all are now gated on actual focus. The Appearance settings'
> theme picker also gained real keyboard focus and arrow-key cycling.

<!-- -->

> **macOS: fixed the compose box losing focus to any click elsewhere in
> the window (2026-07-17, v0.8.16).** Clicking the room list, user info
> panel, or timeline on macOS silently stole keyboard focus away from the
> compose box before the app's own click handling ever ran. Also refocuses
> the compose box after declining an incoming call banner.

<!-- -->

> **Fixed the recovery-key/passphrase fields becoming unfocusable on every
> repaint (2026-07-17, v0.8.16).** On Qt, the recovery-key/passphrase
> fields during Recover-mode login could become permanently unfocusable
> because a hide-then-reshow on every repaint silently cleared focus.

<!-- -->

> **Linux: shut down gracefully on SIGINT/SIGTERM
> (2026-07-17, v0.8.16).** Ctrl+C previously killed the process outright,
> skipping the destructor that flushes session/token state — which could
> corrupt a just-refreshed OAuth token and wipe the local account on next
> launch. Signals now route through the normal graceful-quit path.

<!-- -->

> **Configured a default media-retention policy on every client build
> (2026-07-17, v0.8.16).** The media cache previously never shrank under
> "Clear all caches" because no retention policy was configured. Now
> defaults to a sensible cache size/per-file/expiry policy on every
> account.

<!-- -->

> **Stickers now support replies (2026-07-16, v0.8.16).** Selecting a
> sticker while the compose bar has an active reply now attaches the reply
> relation like any other message. Also fixes sticker timeline rows never
> rendering their replied-to quote block.

<!-- -->

> **Tab traversal scoped to MainAppWidget-level overlays
> (2026-07-16, v0.8.16).** Fixes Tab/Shift-Tab cycling through background
> widgets while a top-level overlay (image/video viewer, confirm dialog,
> quick switcher, search, forward picker, encryption setup, QR grant) is
> open, instead of staying scoped to the overlay.

<!-- -->

> **Every `tk::Widget` now constructed through a Host-aware factory
> (2026-07-16, v0.8.16).** Widget construction now goes through a factory
> that makes the owning `Host` available from the first line of any
> constructor, replacing hand-threaded `Host*` parameters throughout.
> Fixes a real release-build-only crash caused by undefined behavior in the
> previous approach.

<!-- -->

> **Real keyboard-focus system; native text fields now live directly in
> the widget tree (2026-07-16, v0.8.16).** Adds Tab/Shift-Tab traversal and
> a keyboard-only focus ring, and migrates every native text-entry surface
> across the app from shell-polled overlays to self-positioning widgets.
> Adds a default-focus policy: the compose box is now focused whenever
> nothing else needs attention, replacing ~40 scattered manual refocus call
> sites. Also fixes Tab/Shift-Tab leaking into background controls while a
> room modal is open.

<!-- -->

> **Reaction chips redesigned; mixed text/emoji reaction keys
> (2026-07-14, v0.8.16).** Reaction pills get a smaller, tighter shape of
> their own instead of sharing geometry with other pill UI, and now render
> mixed text/emoji reaction keys (MSC4027) correctly, each run sized and
> aligned appropriately.

<!-- -->

> **`/location` slash command; three-bug GeoClue2 fix along the way
> (2026-07-17, v0.8.16).** `/location` sends a one-shot device location fix
> as an `m.location` event with no confirmation step, on all four
> platforms including pop-outs. Along the way, fixes three real bugs in the
> Linux GeoClue2 backend that had shipped unwired and never worked: wrong
> D-Bus bus, a broken in-flight-request check, and a dangling cancellable.

<!-- -->

> **Windows: fixed a use-after-free in native text field/area destruction
> (2026-07-14, v0.8.15).** A repeatedly opened/closed transient text
> control (search bar, quick switcher, etc.) left a dangling registry entry
> that could cause a use-after-free on the next theme change. Windows-only;
> unverified in this environment pending an on-platform build.

<!-- -->

> **Six unbounded caches found in a memory-usage audit, bounded or pruned
> (2026-07-14, v0.8.15).** A memory audit found and fixed six independent
> caches (Rust media-fetch hints, two SQLite backoff tables, five
> `ShellBase` maps, a receipt map, and animated-image frame decoding on
> three backends) that grew unbounded for the life of a session. Verified
> on Qt6/GTK4; Windows/macOS changes mirror the same pattern but weren't
> build-verified here.

<!-- -->

> **Widget removal hardened against reentrant/self-destroying callbacks
> (2026-07-14, v0.8.15).** Closes two subtle use-after-free hazards where a
> widget's own callback could destroy itself or a sibling mid-invocation:
> `Host`'s tracked widget references are now weak, and actual subtree
> destruction is deferred to the next event-loop turn. Verified on Qt6/GTK4;
> Win32/macOS mirror the same pattern but weren't build-verified here.

<!-- -->

> **Legacy username/password login for non-OIDC homeservers (2026-07-13,
> v0.8.15).** Adds `m.login.password` as a fallback login path for
> self-hosted homeservers with no OIDC/MAS provider, auto-detected from the
> homeserver's advertised login flows, behind a default-on build flag.
> Verified end-to-end against a real self-hosted Synapse with no OIDC/MAS,
> on all platforms.

<!-- -->

> **File drop and drag-hover dispatch through the widget tree
> (2026-07-13, v0.8.15).** File drop and drag-hover now dispatch through
> the normal widget tree instead of requiring a hand-rolled handler per
> drop target, so each target (compose bar, pack editors) claims its own
> drop and paints its own localized highlight. Also fixes the native
> compose text field swallowing file drags as pasted text on Qt6/macOS/GTK4.
> Confirmed working on-platform on all four platforms.

<!-- -->

> **Bigger emoji in the composer and room-list preview (2026-07-14,
> v0.8.15).** Emoji in the compose bar and room-list message preview now
> render at message-body size instead of plain body size, matching the
> timeline, resizing live as-you-type on all four platforms. Also fixes a
> one-keystroke lag on Qt/Win32 and adds missing single-line-ellipsis
> truncation to the rich-text renderer.

<!-- -->

> **macOS: fixed composer inline-emoji resize corrupting glyph layout
> (2026-07-14, v0.8.15).** Fixes a macOS-only bug where the bigger-emoji
> feature above could corrupt a just-typed emoji's glyph layout, leaving it
> invisible until a later edit.

<!-- -->

> **macOS: room-list preview text no longer drifts when it contains emoji
> (2026-07-14, v0.8.15).** Fixes a macOS-only bug where the bigger-emoji
> feature above could make the room-list preview's text baseline drift
> downward whenever the preview contained emoji.

<!-- -->

> **Slash-command popup: full list on no match, plus scrolling
> (2026-07-14, v0.8.15).** The `/command` popup no longer hides itself when
> the typed prefix matches nothing — it falls back to the full command
> list — and now scrolls instead of hard-capping at 8 visible rows.

<!-- -->

> **Room header: topic-link clicks no longer leak onto the room name/avatar
> (2026-07-14, v0.8.15).** Fixes clicking the room name or avatar
> occasionally opening the room topic's link instead, on Qt6 and macOS.

<!-- -->

> **Room Settings → Permissions: aligned combo boxes across groups
> (2026-07-14, v0.8.15).** Combo boxes across the Permissions tab's four
> groups (Default Role, Messages, Membership, Advanced) now align to a
> shared label-column width instead of each group sizing independently.

<!-- -->

> **Login: homeserver field's drawn border no longer lingers in the
> password form (2026-07-14, v0.8.15).** Fixes the homeserver field's
> rounded-rect border staying painted after switching to the
> username/password login form.

<!-- -->

> **Sticker right-click save menu no longer leaks through room overlays
> (2026-07-12, v0.8.14).** Fixes right-clicking over Room Settings/Room
> Info/User Profile popping the sticker-save context menu from stale
> timeline content underneath.

<!-- -->

> **Inline custom-emoji shortcode tooltips in the timeline (2026-07-12,
> v0.8.14).** Hovering a custom MSC2545 emoji in a message timeline now
> shows its `:shortcode:` tooltip, matching the emoji/sticker picker grids.

<!-- -->

> **Personal image-pack editor: drag-drop wired (2026-07-12,
> v0.8.14).** Dropping an image onto the personal image-pack editor in
> Settings now works — the Settings window's own surface previously wasn't
> wired for file drop at all.

<!-- -->

> **Fixed: the Emoji/Sticker picker's shortcode tooltip froze every
> animation in the app on Windows while visible (2026-07-12,
> v0.8.14).** A self-sustaining repaint loop in the shortcode tooltip
> starved the Win32 timer that drives animation frame advance app-wide
> while the tooltip was shown. Windows-specific; unverified live in this
> environment.

<!-- -->

> **Custom MSC2545 emoji now render inline in the timeline on macOS
> (2026-07-12, v0.8.14).** Custom emoji previously rendered fine in the
> composer and pickers but not in the message timeline itself on macOS.
> macOS-only fix; unverified in this environment pending a build check.

<!-- -->

> **Emoji/sticker pickers and the shortcode popup now surface packs from
> any Space the current room belongs to (2026-07-12, v0.8.14).**
> Extends the existing personal/current-room/subscribed-room pack scopes
> with a fourth: packs from every Space (direct or nested) that contains
> the current room. Verified on Qt6/GTK4; Win32/macOS mirror the same
> pattern but weren't build-verified here.

<!-- -->

> **Native-field theming now traverses the widget tree instead of a
> hand-maintained per-shell field list (2026-07-12, v0.8.14).**
> Every native text field now re-themes itself automatically via a new
> `Widget::apply_theme()` tree walk instead of relying on each shell to
> remember to push color updates to every field by hand. Fixed several
> fields that the old manual lists had missed entirely (Qt6's Settings/
> Join-Room dialogs, macOS's join-room dialog permanently stuck in light
> mode). Verified on Qt6/GTK4; Win32/macOS mirror the same pattern but
> weren't build-verified here.

<!-- -->

> **Generic `tk::Host` tooltip system replaces 8 hand-rolled hover/tooltip
> implementations and 4 duplicate per-platform native tooltip codepaths
> (2026-07-12, v0.8.14).** Every tooltip in the app is now driven by one
> shared `tk::Host` tooltip mechanism with a consistent 500ms show-delay,
> replacing 8 independently hand-rolled implementations (two of which,
> macOS and GTK4, weren't even using a real native tooltip API). Verified
> on Qt6/GTK4; Win32/macOS mirror the same pattern but weren't
> build-verified here.

<!-- -->

> **Native text fields no longer go stale on a theme change; forward-picker
> close wired on all four shells (2026-07-12, v0.8.14).** Fixes 11 of Qt6's
> 13 native text fields (including the quick switcher) keeping unreadable
> colors after a theme change, and fixes Escape/outside-click never closing
> the forward-message picker's field on any of the four shells. Verified on
> Qt6/GTK4; Win32/macOS mirror the same pattern but weren't build-verified
> here.

<!-- -->

> **Media lightbox pagination leak + gallery backpressure fixed
> (2026-07-12, v0.8.14).** Fixes several bugs where opening the room media
> viewer left background work running after close or room switch: the
> video lightbox not swallowing wheel input (driving background
> pagination), an unabortable pagination task that could block shutdown,
> and unthrottled pagination in media-sparse rooms outrunning the row
> renderer.

<!-- -->

> **Image pack editor: multi-pack room/space editor + global settings tab,
> fully wired end to end (2026-07-11, v0.8.14).** A new Room Settings
> "Emojis & Stickers" tab edits every MSC2545 sticker/emoji pack in a room
> or space at once (rename, usage toggle, per-image add/remove/shortcode,
> drag-drop and paste), gated behind the room's actual power levels. A
> matching global Settings tab covers the account-wide personal pack and
> the list of subscribed room packs. Pack discovery and reads now combine
> both the stable and legacy unstable MSC2545 event names so partially
> migrated rooms/accounts don't lose images. Qt6 build-verified end to end;
> GTK4 user-verified; Win32/macOS verified by static review only. Also
> fixes several smaller bugs found along the way (animated stickers not
> rendering in editor tiles, shortcode-field/paste edge cases, an i18n-pseudo
> generator bug).

<!-- -->

> **Edited plain-text messages no longer render as a bare `*`
> (2026-07-11, v0.8.14).** Fixes edited messages with no formatting
> sometimes rendering as a literal `*` instead of the edited text.

<!-- -->

> **Windows: clipboard image paste restored in the BetterText composer
> (2026-07-11, v0.8.14).** Fixes Ctrl+V no longer pasting a clipboard image
> into the composer after it moved to the BetterText control.

<!-- -->

> **Fixed a runaway pagination loop in the room media gallery
> (2026-07-10, v0.8.14).** Closing the room media gallery didn't actually
> stop its backward-pagination retries, producing an unbounded pagination
> loop that could delay message send confirmation and even hold up app
> shutdown. Shared code, so all four platforms get the fix.

<!-- -->

> **Pop-out window feature-parity audit (2026-07-10, v0.8.14).** An audit
> found and fixed 13 places where a feature worked in the main window's
> room view but not in pop-out room windows (attachment save dialogs,
> jump-to-message, pin/unpin, edit-last-message, retry/abort send, inline
> autoplay, forward picker, media gallery, and more).

<!-- -->

> **MSC2545 image packs now combine stable + unstable event names
> (2026-07-10, v0.8.14).** Pack loading now reads both the stable and
> legacy unstable MSC2545 event names and merges them, instead of stopping
> at whichever it found first — fixing images silently disappearing on
> partially migrated rooms/accounts.

<!-- -->

> **Linux OS dark/light mode detection fixed on Qt6 and GTK4 (2026-07-10,
> v0.8.14).** Fixes dark mode never being detected via Qt6's D-Bus fallback
> path, and GTK4 not picking up live theme changes on Wayland — both now
> query the same XDG desktop-settings portal.

<!-- -->

> **Windows composer mention pills render as real inline chips
> (2026-07-10, v0.8.14).** @mentions typed in the Windows composer now
> render as a colored inline chip instead of plain `@Name` text, matching
> the other three platforms.

<!-- -->

> **Pinned-events room-list fingerprint fix (2026-07-10, v0.8.14).** Fixes
> the pinned-messages banner and the Pin/Unpin menu label going stale after
> a pin/unpin that didn't also touch some other room field.

<!-- -->

> **Faster local echo under background load (2026-07-09, v0.8.14).** Fixes
> a just-sent message's local echo sometimes taking seconds to appear under
> load, caused by tokio async-worker starvation in the SDK. Moving the
> room-timeline-build work onto tokio's blocking pool frees the async
> workers for the latency-sensitive send/diff tasks. Verified on Qt6 and
> GTK4; Win32/macOS mirror the same pattern pending an on-platform build.

<!-- -->

> **Custom MSC2545 emoji inline, end to end (2026-07-06, v0.8.14).**
> Picking a custom emoji from the picker or shortcode autocomplete now
> inserts a real inline image in the composer and sends it as a proper
> `<img data-mx-emoticon>` tag that renders inline in the timeline, on all
> platforms. Also fixes an XSS where an attacker-controlled shortcode was
> interpolated unescaped into `alt`/`title`.

<!-- -->

> **BetterText: D2D/DirectWrite text backend on Windows (2026-07-09, v0.8.14).**
> Vendors a new from-scratch D2D/DirectWrite text control for Windows native
> text fields, adding inline IME composition, placeholder/password
> rendering, and real inline bitmap rendering — so custom emoji show inline
> in Windows compose fields like everywhere else.

<!-- -->

> **Copy image to clipboard from the lightbox (2026-07-07, v0.8.14).**
> The full-window image viewer gains a "copy" button that copies the
> displayed image to the system clipboard, on all four platforms, with a
> confirming toast. Verified on Qt6.

<!-- -->

> **Room Permissions self-lockout warning (2026-07-06, v0.8.13).**
> The Permissions tab now warns and disables Accept if a staged change
> would lock the current user out of ever editing room permissions again.

<!-- -->

> **Idle-CPU and animation-repaint performance fixes (2026-07-04, v0.8.12-unreleased).**
> Reduces idle CPU usage by disabling an unneeded store-lock lease renewal,
> fixing an animated sticker/GIF forcing a full-UI repaint on every frame
> instead of just its own region, and avoiding re-establishing a hardware
> video decode session every time an already-seen video is revisited.

<!-- -->

> **Voice message auto-advance (2026-07-01, v0.8.12-unreleased).**
> A voice message that finishes playing on its own now automatically
> starts the next voice message from the same sender in the room, if any.

<!-- -->

> **Scroll-position stability during pagination (2026-07-01, v0.8.12-unreleased).**
> Loading more history — backward while scrolled to the top, or forward
> while browsing old messages — no longer shifts what the user is looking
> at. Auto-scroll-to-bottom is now correctly limited to a live message
> arriving while already pinned to the tail.

<!-- -->

> **Room join/leave timeline events (2026-07-02, v0.8.12-unreleased).**
> An opt-in setting (default off) surfaces join/leave/kick/ban/invite/knock
> membership transitions in the message timeline. Consecutive same-action
> events collapse into one summary line, expandable into individual lines
> on click.

<!-- -->

> **Room settings — edit name/topic/avatar (2026-07-02, v0.8.12-unreleased).**
> A wrench icon in the room-info panel opens a full-panel view for staging
> edits to the room's avatar, display name, and topic, gated per-field by
> power level. Nothing is sent until Accept.

<!-- -->

> **Screen-share picker thumbnails + Linux stability (2026-07-03, v0.8.12-unreleased).**
> The screen-share picker now shows real per-source thumbnails instead of
> placeholder tiles. Also fixes a black-tile bug on Linux, a UI freeze on
> stopping a stalled capture, leaked portal sessions, solid-black Windows
> capture of GPU-composited apps, and a macOS thumbnail deadlock.

<!-- -->

> **Location map click-through (2026-07-04, v0.8.12-unreleased).**
> Clicking (not panning) a location message's embedded map opens it on
> openstreetmap.org, centred on the pin.

<!-- -->

> **Media caption linkify (2026-07-01, v0.8.12-unreleased).**
> Captions on image/file/video messages now render through the same
> rich-text pipeline as regular message bodies, so bare URLs in a caption
> are clickable links instead of plain text.

<!-- -->

> **Room-switch viewport auto-backfill (2026-07-01, v0.8.12-unreleased).**
> Switching rooms now proactively fetches more history if the first page
> doesn't fill the viewport, instead of waiting for a manual scroll
> gesture. Benefits both the room timeline and the thread panel.

<!-- -->

> **MatrixRTC voice/video calls (2026-06-25, v0.8.12-unreleased).**
> Native LiveKit-based MatrixRTC calls (MSC4143), interoperating with
> Element X and Element Call. The call overlay supports docked, floating,
> and pop-out-window modes with mute/video/hang-up controls, and incoming
> calls surface a ring notification. Hidden when the server doesn't
> advertise LiveKit support, or for bridged rooms.

<!-- -->

> **`/selfie` slash command (2026-06-25, v0.8.12-unreleased).**
> Typing `/selfie` in the composer opens a full-surface camera overlay with
> a 3-second countdown; the captured still is inserted as a compose-bar
> attachment. Disabled while a call is active.

<!-- -->

> **Audio/video device selection (2026-06-25, v0.8.12-unreleased).**
> Settings → Media gained microphone, speaker, and camera selection
> dropdowns, applied at the next session start.

<!-- -->

> **Bridged-room detection (2026-06-27, v0.8.12-unreleased).**
> Rooms bridged via a third-party network (MSC2346) suppress the call
> button and threads panel and show a Bridged badge in the room-info panel.

<!-- -->

> **Space root view (2026-06-28, v0.8.12-unreleased).**
> Selecting a joined space itself (rather than drilling into a child room)
> now shows a centred summary panel with avatar, name, topic, and
> joined/unjoined child counts.

<!-- -->

> **Phone icon for active calls (2026-06-27, v0.8.12-unreleased).**
> The room list shows a phone icon on any room with an active call.

<!-- -->

> **Room-switch media fetching overhaul (2026-06-30, v0.8.11).**
> Fixes media requests appearing to freeze in rooms that trigger many at
> once, and the in-flight indicator lingering after leaving a media-heavy
> room. The main cause was every room switch eagerly fetching an avatar
> for the entire membership list up front; avatars now fetch on demand
> instead, on all four shells.

<!-- -->

> **macOS thread-reset stack-overflow fix (2026-06-30, v0.8.11).**
> Fixes a macOS-only crash when a thread's timeline reset while the
> message list was mid-layout, caused by a synchronous relayout call
> recursing into itself.

<!-- -->

> **Group unread rooms (2026-06-24, v0.8.9).**
> An optional "Group unread rooms" toggle adds an Unread section above
> Favorites in the room list, collecting every room with a visible unread
> indicator — including rooms nested inside spaces, previously invisible
> at the root list. Rooms leave the section automatically when read.

<!-- -->

> **Colored sender display names (2026-06-24, v0.8.9).**
> Sender names in the message timeline are tinted using a hash of the
> user's Matrix user ID, mapped into an 8-hue palette tuned for contrast in
> both light and dark mode. The color stays stable across display-name
> changes.

<!-- -->

> **Space topic preview in room list (2026-06-24, v0.8.9).**
> Space entries in the Spaces section now show the space's topic as the
> one-line preview instead of a last-message snippet, falling back to
> name-only when the topic is absent.

<!-- -->

> **Forward message (2026-06-19, v0.8.8).**
> A "Forward message" item in the message menu opens a room picker to
> resend the message's content, with all msgtypes preserved, to one or
> more other rooms.

<!-- -->

> **macOS dock badge + dock-click unread navigation (2026-06-17, v0.8.6).**
> The macOS dock icon shows the total notification count as a badge across
> all signed-in accounts. Clicking it raises the window and navigates to
> the highest-priority unread room, matching tray-click behavior on the
> other platforms.

<!-- -->

> **Win32 body font raised 1 pt above the OS default (2026-06-17, v0.8.6).**
> Raises the Windows UI's base font size by 1pt above the OS default, since
> the stock size reads noticeably small next to modern chat clients; every
> font role scales accordingly.

<!-- -->

> **Async space-summary and server-info FFI (2026-06-17, v0.8.5).**
> Space-summary and server-info fetches are now asynchronous instead of
> blocking a C++ worker thread for the full HTTP round-trip, preventing
> concurrent fetches from saturating the worker pool and making the client
> unresponsive.

<!-- -->

> **System font scaling across all four backends (2026-06-15/16, v0.8.5).**
> Tesseract now reads the OS body font size at startup on all four
> platforms and derives every UI font role as an offset from it, so the UI
> scales naturally with the user's OS accessibility font-size setting.

<!-- -->

> **Inline emoji scaling (2026-06-16, v0.8.5).**
> Unicode emoji in message bodies render at ~125% of body font size;
> emoji-only captions beneath media render at 2× body size, matching
> standalone emoji messages.

<!-- -->

> **Automatic update checker (2026-06-16, v0.8.5).**
> A background checker queries GitHub releases at startup and periodically,
> showing an in-app banner when a newer version is available. Controlled
> by a Settings → Privacy toggle.

<!-- -->

> **MSC4133 extended user profiles (2026-06-14, unreleased).**
> Adds Pronouns, Timezone, and Biography fields to account settings and the
> user-profile panel, backed by MSC4133 account data with stable/unstable
> key fallback. Wired on all four shells.

<!-- -->

> **Unjoined space-children section + `RoomPreviewView` (2026-06-14, unreleased).**
> Navigating into a space now shows a collapsible "Not joined" section
> listing child rooms the user hasn't joined; clicking one opens a preview
> panel with room details and a Join button, without changing the active
> room.

<!-- -->

> **Block-level Markdown rendering (2026-06-13, unreleased).**
> Headings, lists (including nested), blockquotes, and tables now render
> visually in the message timeline across all four canvas backends,
> complementing the existing inline styles (bold, italic, code,
> strikethrough, links).

<!-- -->

> **Full-text message search, incl. encrypted rooms (2026-06-11/13).**
> A global search overlay (Ctrl+Shift+F / ⌘⇧F) searches your message
> history, including encrypted rooms, via a local opt-in SQLite FTS5 index
> of decrypted message bodies (off by default, since it stores decrypted
> text at rest). Results show room, sender, and snippet, and clicking jumps
> to the message. Verified on Qt6.

> **In-room find-in-conversation search bar (2026-06-13).**
> Ctrl+F / ⌘F opens a search bar anchored below the room header that
> highlights matching rows in the timeline and navigates between hits,
> auto-paginating older history when needed to find more matches.

> **Shared jump-to-date picker (2026-06-13, unreleased).** Replaces the
> four platforms' separate native date pickers with one shared calendar
> widget for jumping to a specific date in a room's history.

> **Room-switch performance overhaul (2026-06-11, unreleased).** Switching
> rooms is now instant and flicker-free: the old room's rows clear
> immediately, a loading spinner appears only if the load outlasts ~500ms,
> and a still-live timeline is reused instead of rebuilt (bounded by a new
> warm-subscription LRU). Verified on Qt6, GTK4, Win32, and macOS.

> **Multi-window: one window per account.**
> Ctrl+click an account in the picker opens it in its own window; clicking
> an account that already has a window raises it instead of switching in
> place. Each account's SDK event bridge can follow whichever window is
> showing it, and closing a secondary window no longer stops sync for
> shared accounts. Verified on Qt6.

> **Unread-room message prefetch.**
> Rooms that quietly accumulate unread messages (unread but not muted) now
> have their recent messages warmed into the SDK cache ahead of time, so
> opening them renders instantly instead of fetching on click. Default-on.

> **Pre-launch hardening + decomposition (2026-06-09, unreleased).** A
> full-tree code review drove a large correctness/safety/dedup pass and the
> start of a god-object decomposition: shells routed through shared
> `ShellBase` handlers, a multi-window/logout use-after-free and an FFI
> aliasing bug closed, and ~1,250 lines of cross-shell duplication hoisted
> into shared code. No user-facing feature change.

> **Unified Lucide icon set.**
> The composer, message hover-action bar, media viewers, and other UI
> chrome now render from monochrome Lucide SVG icons instead of Unicode
> glyphs or hand-drawn shapes, tinted to the active theme and crisp on
> HiDPI.

<!-- -->

> **`matrix.to` and `matrix:` URI navigation (MSC2312).**
> Clicking a `matrix.to` or `matrix:` link in a message body navigates
> within the app instead of opening the browser: a joined room navigates
> directly, an unknown room opens the join dialog pre-filled, user links
> open the profile panel, and event links scroll to the target event. All
> four platforms register as the OS handler for the `matrix:` URI scheme.

<!-- -->

> **Sticky, collapsible section headers in the room list.**
> Section headers (Favorites, DMs, Rooms, Spaces, Inactive) stick to the
> top of the room list while scrolling their section, and stay fully
> interactive — click to collapse/expand, hover highlight — while pinned.
> Rooms with unread messages render their title semibold.

<!-- -->

> **Auto-scroll the room list to unread rooms.**
> When a room receives new messages, the room list scrolls the most-recent
> unread room into view instead of leaving it hidden below the fold. The
> scroll is minimal — already-visible rooms aren't disturbed — and only
> genuinely new activity re-triggers it. Optional via an Appearance setting,
> default on.

<!-- -->

> **Faster room switching & message bursts.**
> The message list no longer rebuilds and re-shapes every row when messages
> arrive or a room is opened — text layouts are shaped once and cached, and
> a single inserted/updated/removed message re-measures only its
> neighbourhood instead of the whole room. A sync burst now triggers one
> layout pass instead of one per message. Shared code; verified on Qt6.

<!-- -->

> **Pop-out room windows.**
> Ctrl/⌘+click a room tab to pop the room out into its own native window.
> A pop-out is a full room view — timeline, compose, pickers, reactions,
> mentions, animated media, and a room info panel — on all four platforms.
> Verified on Win32; the other shells mirror the same shared logic.

<!-- -->

> **GIF picker (`/gif`).**
> Type `/gif <query>` in the composer to search and send GIFs. Results
> appear in an animated horizontal strip above the compose bar; chosen GIFs
> send as autoplaying video, encrypted like any other media in E2EE rooms.
> Wired on all four shells.

<!-- -->

> **Room navigation history (Alt+Left / Alt+Right; Cmd+[ / Cmd+] on macOS).**
> Back and forward navigation through the session's room visit history,
> like browser navigation. Shortcuts are application-scoped and fire even
> while the compose box holds focus.

<!-- -->

> **Quick switcher (Ctrl/⌘+K).**
> A centered command-palette overlay for jumping between rooms, with a
> "Recent" strip and a full name-filtered room list. Typing a leading `@`
> flips it into user mode to start a DM with anyone, including a
> previously-unseen Matrix ID, resolving and confirming the profile before
> offering the row.

<!-- -->

> **Encryption-setup overlay.**
> A shared overlay wired on all four shells guides new-account users
> through enabling cross-signing, with separate flows for a fresh setup
> (choose recovery key or passphrase) and recovering an existing identity.

<!-- -->

> **In-flight request indicator (animated spinning ring).**
> An animated ring in the status bar shows the number of currently
> in-flight Matrix API requests — green, amber, or red depending on count —
> with a tooltip showing the exact number. Wired on all four platforms;
> macOS gained a status bar in the same pass.

<!-- -->

> **Non-blocking media downloads.**
> Media fetches (avatars, thumbnails, full-size images, sticker/emoji
> images, map tiles, URL previews, voice/audio) now run as async tasks
> instead of blocking calls that each pinned a worker thread, so a slow or
> dead server can no longer starve the media the user is actually waiting
> on. Switching rooms cancels the previous room's still-pending downloads.

<!-- -->

> **Pinned events.**
> A banner above the message list cycles through pinned messages with a
> counter and jump-to-message; a Pin/Unpin action appears in the hover menu,
> gated by the user's power level.

<!-- -->

> **Room tags (favourite / low priority).**
> The room info panel gains Favourite and Low Priority toggle switches,
> mutually exclusive both in the UI and on the server.

<!-- -->

> **Hover action pill.**
> The per-message hover affordances (reply / thread / react / edit)
> consolidate into a single rounded pill anchored to each row, with
> destructive/moderator actions tucked behind an overflow menu to keep the
> pill tidy.

<!-- -->

> **Win32 windowless RichEdit compose bar.**
> The Windows compose bar is now driven by a windowless native rich-edit
> control rendered directly into the surface, with correct color-emoji
> rendering and all prior text-area behavior (mention pills, clipboard
> image paste, slash popups, IME) preserved.

<!-- -->

> **Win32 full HiDPI fix.**
> Fixes a systematic coordinate-space mismatch on Win32 (physical pixels vs.
> D2D's DIPs) affecting pointer dispatch, widget bounds, and popup
> positioning. Emoji/sticker pickers and the rich-edit compose area also
> now honour dark mode.

<!-- -->

> **Tab session restore.**
> The full set of open room tabs is now persisted across restarts, restored
> in a single pass with no inter-tab flickering. Wired in all four shells.

<!-- -->

> **Matrix threads UI.**
> A "threads" button toggles a right-side panel with three states: closed,
> a list of every thread in the room, or one thread's own reply timeline
> with its own compose bar. While a thread is open the main timeline dims
> and in-thread replies are hidden from it. Wired in all four shells.

<!-- -->

> **Privacy settings tab — presence toggle and room key export/import.**
> A new Privacy settings tab lets the user disable outgoing/incoming
> presence, and export or import encryption room keys via a
> passphrase-protected file, with native dialogs on all four platforms.

<!-- -->

> **Storage size display and cache-clear in About settings.**
> The About settings tab shows local cache and SDK store sizes, and a
> destructive "Clear all caches" button that wipes them without touching
> credentials or active sessions. Wired on all four shells.

<!-- -->

> **@mentions.**
> Typing `@` in the composer opens an autocomplete that filters room
> members as you type, with an `@room` entry pinned on top. Selecting one
> inserts an inline pill (plain `@Name` text on Win32 for now); received
> mentions render as clickable pills that open the user's profile.
> Verified on Qt6; GTK4 builds clean; macOS/Win32 written but unverified in
> this environment.

<!-- -->

> **Account registration (OIDC `prompt=create`).**
> The login screen offers a "Create an account" link that reuses the
> existing OAuth flow to reach the homeserver's own signup page, shown only
> when the homeserver advertises registration support.

<!-- -->

> **Group inactive rooms.**
> An Appearance setting adds a default-collapsed "Inactive" section holding
> DMs and rooms with no activity past a configurable threshold (default one
> month); favorites and spaces are never grouped, and a room reclassifies
> out automatically on new activity.

<!-- -->

> **Outgoing Matrix presence.**
> The app now publishes its own presence, not just receives it, via an
> idle-decay state machine: Online while engaged with the app, Unavailable
> after 5 minutes of no input/focus, Offline on logout.

<!-- -->

> **Code-block syntax highlighting and tinted backgrounds.**
> Fenced code blocks in messages now render with syntax-highlighted colors
> and a tinted background panel, across all four canvas backends; inline
> code gets a tight per-run tint. Unknown or absent languages fall back to
> plain monospace.


For build instructions, architectural overview, and the open-roadmap items, see [CLAUDE.md](CLAUDE.md). For tracked open issues / known gaps, see the "Known gaps" section at the bottom of CLAUDE.md.

## Test coverage

| Suite | Count |
| ----- | ----- |
| Rust unit tests (`cargo test -p tesseract-sdk-ffi`) | 461 |
| C++ Catch2 tests via ctest (Qt6 preset) | 1386 |

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
- **Legacy `m.login.password` fallback** — for self-hosted homeservers without an OIDC/MAS provider, gated behind `TESSERACT_ENABLE_LEGACY_LOGIN` (default `ON`). `LoginView` auto-detects support via a homeserver capability probe and shows a "Sign in with password" screen alongside the OAuth button. Session storage is a tagged `SessionEnvelope{OAuth, Native}` so `restore_session`/`export_session`/`logout` share one code path regardless of auth mechanism.
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
- **Hardened `matrix-sdk` init** — bounded request retries/timeout and auto-enabled key backup alongside cross-signing bootstrap (matching Element X Android's client config). New sessions' local SQLite store is encrypted with a randomly generated per-session key, persisted via the platform secret store; sessions created before this shipped remain unencrypted permanently (matrix-sdk has no in-place store-migration API).
- **Descriptive device display name** — reports the actual Linux distro, macOS OS version, or a normalized "Windows 11/10 <edition>" string (via `os_info`) instead of a bare "Windows"/"macOS"/"Linux", sanitized before reuse in the User-Agent's `"(name; os)"` token, the OAuth `device_display_name` param, and `rename_device`; MAS's session list derives its device label from the first token of that parenthetical.

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
- **One-time initial history fill per subscription** — revisiting an already-subscribed room no longer re-runs a 100-event `paginate_back_with_status` fetch on every visit; a one-time `initial_fill_done` flag gates it so the fill runs once per warm subscription and later revisits are free. `reply_details_requested_` clearing and the room-layout account-data `PUT` also moved from every switch to once (the layout write to window close).
- **Right-click context menu on room list rows** — Open in tab / Open in window / Leave room (with confirmation), via the shared `PopupMenu` widget; the open-in items disable when the room is already open in that context.

## Spaces (Step 7)

- `is_space: bool` on `RoomInfo`; spaces shown at the bottom of the room list with `#` prefix on Qt6 / GTK4 (top-row dedicated bar on macOS).
- `space_children(space_id)` FFI returning joined direct children; `space_children_all(space_id)` returning all direct children (joined + unjoined).
- **Stack-based drill-in navigation** — selecting a space replaces the room list with its children; `←` back button + space name label at the top of the sidebar; recursive sub-spaces; auto-pop to "All rooms" when the stack is empty.
- **Space children hidden from the root room list** — they appear only when navigating into the parent.
- **Unjoined space children** — a collapsible "Not joined" section below the joined-rooms list shows every child room the user hasn't joined. Clicking opens `RoomPreviewView` (name, avatar, topic, member count, Join button) without leaving the current room. Summaries fetched concurrently via MSC3266 with generation-based cancellation.

## Shared UI toolkit (`tesseract_tk`)

- **`tk::Canvas`** — abstract 2D backend with four concrete impls (`canvas_d2d`, `canvas_qpainter`, `canvas_cairo`, `canvas_cg`). Color / Rect / Point / Image / TextLayout primitives; rounded-rect, stroke, push/pop clip; circle-cropped image draw; initials disc helper.
- **`tk::Widget`** — measure / arrange / paint + pointer / wheel dispatch with `dispatch_pointer_down` + `world_to_local` capture semantics. Every subclass is constructed exclusively through `tk::create_widget()`/`create_root_widget()` (a Host-aware factory backed by a thread-local pending-`Host*` stack), never directly — constructors are `protected` and friend the factory via `TK_WIDGET_FACTORY_FRIEND`, so `host()` is valid from the first line of any constructor with no manual parameter plumbing.
- **`tk::Host`** — per-platform integration surface (repaint scheduling, post-to-UI, native edit overlays). `request_repaint`, `post_to_ui`, `make_text_field`, `make_text_area`, `make_audio_player`, `make_audio_capture`, `encode_for_send`.
- **Keyboard focus** — real Tab/Shift-Tab traversal (`Host::advance_focus`/`request_focus`/`clear_focus`) with a `:focus-visible`-style ring shown only after keyboard navigation, not a mouse click. Traversal order follows each widget's own `bounds()` in reading order (top-to-bottom rows, left-to-right within a row via a row-overlap comparator), not `add_child()` insertion order, so `Stack`/rect-positioned widgets (grids, pickers) traverse sensibly too. `Host::set_focus_scope()`/`clear_focus_scope()` lets an open modal (Room Settings, an overlay, ...) scope Tab traversal to its own subtree. The compose box is focused by default whenever nothing else needs attention. Native text fields (`tk::TextField`/`tk::TextArea`) participate directly as self-positioning widgets in the tree rather than shell-polled overlays.
- **Native text overlays** — `NativeTextField` (`QLineEdit` / `GtkEntry` / Win32 EDIT / `NSTextField`) and `NativeTextArea` (`QTextEdit` / `GtkTextView` / multi-line EDIT / `NSTextView`) for IME-friendly input. `set_placeholder` is implemented on all four platforms (GTK4 uses a `dim-label` `GtkLabel` overlay child since `GtkTextView` has no native placeholder API).
- **Shared views** — `LoginView`, `RoomListView`, `MessageListView`, `EmojiPicker`, `StickerPicker`, `RecoveryBanner`, `ComposeBar` mounted identically on every platform.
- **`AlertDialog`** — modal overlay widget (not backdrop-dismissible) with a title, body, and up to two configurable action buttons (`open(Options, primary_cb, secondary_cb)` / `close()` / `is_open()`). Used by `LoginView` to surface startup restore errors; available for other blocking error prompts.
- **Drag-and-drop ingest** — `tk::Widget` virtuals (`on_file_drop`/`dispatch_file_drop`, `on_drag_hover`/`dispatch_drag_hover`) mirror the existing pointer-event dispatch shape, so each drop target (`ComposeBar`, `RoomView`, `ImagePackEditorView`, `UserPackEditor`) claims its own drop and paints its own localized hover highlight instead of one whole-surface overlay; image-data MIME types route to the compose bar's image preview, generic files route to the file chip.
- **`PopupMenu`** — renders through `tk::PopupSurfaceHandle` (`Host::make_popup_surface()`, the same primitive `tk::ComboBox`'s dropdown uses) rather than a canvas overlay, so it's a genuine OS popup window that z-orders correctly above everything, including native controls; row drawing/hit-testing lives in a nested `MenuList` widget. Supports separator and disabled items. Dismisses on any outside click — including a click that lands in a native text field and never reaches canvas hit-testing — and on the window losing activation (alt-tab), via `Host::dismiss_active_popup()` (also benefits `ComboBox`/`DatePickerView`). `PopupSurfaceHandle::on_dismiss_requested` outside-click auto-dismiss (Mention/Slash/Shortcode/Gif popups) works on all four shells, not just Qt.

## Messaging

- **Send text / image / file / sticker** — `send_message`, `send_image`, `send_file`, `send_sticker` FFI; matrix-sdk handles E2EE transparently. Text sends use `timeline.send()` local echo so the message appears immediately with a ◷ indicator; transitions to ✓ on delivery, ⚠ + Retry on recoverable failure, ⚠ + ✕ on unrecoverable failure. `retry_send` (re-enables SDK send queue) and `abort_send` (`timeline.redact` for local echoes) exposed through FFI and C++ client API; `RoomPane`'s registration of these handlers is the single source of truth (a redundant overwrite from `wire_main_app_widget_()` was removed), and a retry/abort failure now surfaces as a status toast instead of the button silently doing nothing.
- **MSC2530 captions** — `image_filename` distinct from `body` round-tripped; UI shows the body beneath the image only when the sender supplied an explicit `filename`.
- **Redactions** — `redact_event(room_id, event_id, reason)`; `MsgLikeKind::Redacted` surfaces as `msg_type: "m.redacted"` tombstone placeholder in the timeline.
- **Reactions** — `send_reaction` (toggle) FFI; aggregated reaction chips (24px, fixed 6px corner radius) under each message with sender-name tooltips and a hover-only "+" add button. Reaction keys aren't always emoji (MSC4027 plain-text reactions) — the glyph is segmented into emoji vs. text runs and drawn at different sizes (text at 4/5 the emoji size), vertically centred against the emoji box.
- **Replies (`m.in_reply_to`)** — `in_reply_to_id` / `in_reply_to_sender_name` / `in_reply_to_body` extracted in `timeline_item_to_ffi`; quote block rendered above the message body in `MessageListView`; hover "↩ Reply" button fires `on_reply_requested`; `ComposeBar` grows a reply-preview banner (`kReplyBandH = 44 px`) above the text input with a "×" cancel; `send_reply` FFI sends an `m.text` with `Relation::Reply`; reply relation threaded through image/file/sticker sends via `AttachmentConfig::reply`/`send_sticker_`; click on a quote block scrolls to the original message in-list or fires `on_scroll_to_original` when not loaded; all 4 shells wired.
- **Message editing** — `send_edit` FFI wraps `room.make_edit_event()` + `send_queue().send()`; `is_edited` field in `TimelineEvent` set from `msg_content.is_edited()`; `(edited)` badge appended after the body in `MessageListView`; hover "✏" button on own text messages fires `on_edit_requested`; `ComposeBar` grows an edit-mode banner (`kEditBandH = 44 px`) above the text input with a "×" cancel and `on_send_edit` callback; edit mode and reply mode are mutually exclusive (`set_editing` clears reply state); all 4 shells wired.
- **Location messages (`m.location` / MSC3488) receive** — location events render as interactive 240 px inline maps; OSM tiles fetched from `tile.openstreetmap.org` and composited with a disk cache; pan by drag, zoom by scroll wheel (one notch = one zoom level); attribution overlay; red-circle pin at event coordinates; location description shown as a hover tooltip. `on_tile_needed` wired in all four primary shell `MainWindow` files. Send: `send_location` FFI builds and sends the `m.location` event, triggered either by pasting a recognized Google Maps/OpenStreetMap link (opt-in via Settings) or by the `/location` slash command, which fetches the device's current OS location via `tk::LocationProvider` (CoreLocation/WinRT `Geolocator`/GeoClue2) and sends it immediately with no confirmation step.
- **Read receipts** — `EventTimelineItem::read_receipts()` aggregated via a `collect_read_receipts` helper; `MessageListView` paints up to 5 mini-avatar discs (16 px) with a `+N` overflow pill at the row's bottom-right.
- **Hover-only `HH:MM` timestamp** — paints under the sender avatar when the row is hovered; no always-visible time column.
- **MSC2545 sticker decryption** — encrypted-sticker support via direct `ruma = { features = ["compat-encrypted-stickers"] }`; sticker timeline events emit JSON-encoded `MediaSource` for the encrypted variant.
- **Block-level Markdown rendering** — headings (`#` through `######`), unordered and ordered lists (including nested), blockquotes, and tables render visually in `MessageListView` across all four canvas backends. Headings use `FontRole::UiSemibold`; list items indent with correct bullet / ordinal; blockquotes get an accent left-border stripe; tables use fixed-width columns. Complements the existing inline styles and code-block syntax highlighting.
- **Floating date badge** — a rounded pill fixed to the top-center of the timeline viewport while scrolled away from the live tail, naming the day of whatever row is at the top (reuses the inline day-separator's `format_day_label()`). Modeled on `RoomListView`'s sticky-header pattern; pushes up and blends into the real inline `DaySeparator` row as it scrolls into place; shown regardless of how many distinct days are loaded.
- **Drag-select + copy text** — click-drag (or double/triple-click for word/line) selects plain text across message bodies in `MessageListView`; right-click shows a native "Copy" context menu on all four platforms, and Ctrl+C/Cmd+C at the window level copies the selection too. Starting a real selection now moves OS keyboard focus off the composer (`Host::release_focus_to_canvas()`, fired via `MessageListView::on_selection_started`) so the composer's still-focused native text field doesn't swallow the Ctrl+C first; deselecting (a later click) returns focus to the composer via `on_selection_cleared`.

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
- **Media-viewer chrome as real widgets** — close/save/copy on both lightbox overlays, plus the video overlay's play/pause and speed-pill, are `tk::Button` children (not hand-rolled rects with manual hit-testing), so they get hover/press/keyboard activation for free. Each button supplies its own fixed, backdrop-tuned colors via a new opt-in `tk::Button::FillOverride` (rest/hover/pressed, unset by default for every other button in the app) instead of the theme's normal low-alpha `subtle_hover`/`subtle_pressed`, since the app's light/dark theme palette wasn't designed to read against the overlay's permanently near-black scrim — without the override, hover/press were nearly invisible.

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
- **Click behavior** — a tray-icon click shows a hidden window, raises a visible-but-inactive one, and hides the active one (not a naive visibility toggle), on all four shells. On Wayland, raising an inactive window needs the compositor's consent: Plasma's tray host issues a granted xdg-activation token and delivers it via the `ProvideXdgActivationToken` D-Bus call before `Activate`. Qt6 consumes the token Qt's SNI adaptor places in `XDG_ACTIVATION_TOKEN`; `GtkSniTrayIcon` handles `ProvideXdgActivationToken` itself and feeds the token to `gtk_window_set_startup_id()` before presenting. Without it (X11, non-KDE hosts, or a self-issued token with no fresh input serial) the compositor only flags the window as demanding attention.
- **macOS** — `NSStatusItem` with a template menu-bar icon; `windowShouldClose:` hides the window; Quit calls `[NSApp terminate:nil]`.
- **Unread overlay** — when any signed-in account has rooms with notifications, the tray icon gets a small coloured dot in the bottom-right (accent blue for unread, destructive red for highlights / mentions). Aggregation lives in `ShellBase::compute_tray_unread` over `per_account_rooms_`; the `ITrayIcon::set_unread` hook is implemented per shell (QPainter overlay on Qt6, pre-rendered Cairo PNGs swapped via `app_indicator_set_icon_full` on GTK4, GDI+ ARGB compositing into `CreateIconIndirect` on Win32, `NSImage lockFocus`+`NSBezierPath` on macOS).

## Autostart

- **Launch at login** — Settings → General toggle, default off, backed by a new cross-platform `tesseract::IAutostart` abstraction (mirrors `INotifier`/`IScreenLock`): registry `Run` key on Windows, `SMAppService` on macOS 13+, XDG autostart `.desktop` files on Linux. `is_enabled()` always queries the OS directly so the checkbox self-heals if registration is removed outside the app.
- **Autostart launch behavior** — a launch via the OS autostart mechanism starts hidden to the tray only when a saved session restores silently; otherwise the window force-shows so the user can log in. macOS detects a login-item launch via a best-effort `kAEOpenApplication`/`keyAEPropData` Apple Event check.
- **Shared `parse_launch_args()`** (`client/src/launch_args.cpp`, unit-tested) — replaces each shell's previous ad hoc single-argument `argv` scanning, so `--autostart` and a `matrix:` URI can coexist on the command line.

## Notifications (foreground toasts)

- Cross-platform `tesseract::INotifier` / `Notification` abstraction; per-platform impls created after login.
- Push-rule evaluation via `evaluate_push_rules` in `sdk/src/client.rs`; fires on `VectorDiff::PushBack` (live events only); `is_mention` from `Action::is_highlight()`.
- **Win32** — WinRT `Windows.UI.Notifications.ToastNotificationManager`; `ToastGeneric` XML with sender, optional room name (omitted for DMs), 120-char body preview; `WM_TESSERACT_NOTIFY_CLICK` navigates to the room. AUMID registered in `HKCU\Software\Classes\AppUserModelId\` at startup (required for non-packaged apps); `notify()` wrapped in `try`/`catch(winrt::hresult_error)` for robustness.
- **Qt6** — `QDBusInterface` against `org.freedesktop.Notifications` (legacy D-Bus, used everywhere except Flatpak); always a fresh popup (`replaces_id=0`, never updates a prior one in place); click navigates + raises window.
- **GTK4** — `GDBusConnection` (session bus); same legacy-D-Bus/Flatpak-portal split and always-fresh-popup behavior as Qt6, via a shared `tesseract::linux_notify::NotificationCorrelation` helper (`ui/shared/linux_notification_reply.h`) for the id↔room/event bookkeeping both notifiers need.
- **macOS** — `UNUserNotificationCenter`; `UNUserNotificationCenterDelegate` on `MainWindowController`; in-foreground suppression when the source room is active; click navigates to the room.
- **Image & sticker previews** — `m.image` / `m.sticker` notifications embed the message picture (SDK fetch, 2 MiB cap, E2EE-transparent; a dedicated `m.sticker` push handler — stickers are a distinct event type). Win32 large inline `<image>` + circular avatar `appLogoOverride`; macOS `UNNotificationAttachment`; Linux single image slot. Gated by the `notification_image_previews` setting.
- **Lock-screen privacy gate** — cross-platform `tesseract::IScreenLock` (Win32 WTS, macOS `com.apple.screenIsLocked`, Linux logind `LockedHint`); `ShellBase::notification_image_allowed_()` strips the picture whenever the screen is locked (avatars are not gated).
- **Wayland foreground activation** — Qt6 and GTK4 notifiers use `org.freedesktop.portal.Notification` only inside Flatpak (the sandbox's D-Bus proxy blocks the legacy interface there). Everywhere else, including plain Wayland, they use the legacy `org.freedesktop.Notifications` interface and rely on its KDE/GNOME de-facto `ActivationToken(uint id, string token)` signal (mirroring KDE's own `knotifications`) for the `xdg_activation_v1` token, passed to the compositor before `activateWindow()` / `gtk_window_present()`. This requires sending a `"desktop-entry"` hint on `Notify()` (`QGuiApplication::setDesktopFileName("tesseract-matrix")` in Qt6's `main.cpp`; a literal `"tesseract-matrix-gtk"` in GTK4, matching each shell's own installed `.desktop` basename) so the daemon knows which app to mint the token for. Switched away from routing Wayland through the portal because Plasma's Notification portal backend (implemented in `plasma-workspace`, not `xdg-desktop-portal-kde`) only reached interface v1 as of this writing — no inline-reply support at all on that path, and Wayland+KDE users got no working notifications through it either.
- **Per-room notification settings** — a Notifications section in `RoomInfoPanel` with a four-option dropdown (Default / All messages / Mentions / Off) mapped to Matrix per-room push rules (`RuleKind::Override` + `EventMatch` for "off", `RuleKind::Room` for "all"/"mentions", no rule for "default"); backed by a new shared `tk::ComboBox` widget and wired through both the main window and pop-out room windows; Rust `client.rs` reads/writes `m.push_rules`.
- All platforms suppress the notification when the window is focused and the target room is already open.
- **Quick-reply** — Windows toast `<input>`/`<action>` XML with foreground activation (unpackaged apps can't get true background activation); macOS `UNNotificationCategory` + `UNTextInputNotificationAction` without the `.foreground` option so replying doesn't raise the app; Linux implements both the KDE-only legacy D-Bus "inline-reply" extension (now reachable on Wayland too, since the legacy interface is used everywhere except Flatpak — see Wayland foreground activation above) and the portal's standardized "im.reply-with-text" button purpose (interface v2+, gracefully inert until Plasma implements it; only exercised via Flatpak now). `event_id` is threaded through the notification pipeline so a reply sends as a proper threaded reply (`m.in_reply_to`); `ShellBase::send_notification_reply_` dispatches the send and reports a failure via a follow-up notification.

## Build & packaging

- **Corrosion** fetched at configure time (no global Rust toolchain install requirement beyond `rustup`).
- **`WHOLE_ARCHIVE` link** for the 3-way circular dependency between `tesseract_sdk_bridge_cxx`, `tesseract_client`, and `tesseract_sdk_ffi-static`.
- **Cross-platform CMake presets** — `windows-debug`, `windows-release`, `linux-debug`, `linux-release` (builds GTK4 + Qt6), `macos-appkit-{arm64,x86_64}-{debug,release}`.
- **CPack installer packaging** — NSIS and MSIX (Store + direct sideload) on Windows, DMG on macOS, DEB/RPM/AppImage plus a Flatpak/Flathub manifest and AUR `PKGBUILD`s on Linux (see [PACKAGING.md](PACKAGING.md)).
- **Bundled SQLite** via matrix-sdk's `bundled-sqlite` feature; no system OpenSSL dep (TLS uses rustls).

---

## Maintenance note

Update this file after every major feature lands — append a new bullet (or extend an existing one) in the right category, refresh the test counts in the table at the top, and bump the "Last updated" date.
