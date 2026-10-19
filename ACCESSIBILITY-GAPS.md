# Accessibility mapping gaps

> Kept out of the repo on purpose (local working doc, not committed).

> **2026-10-03:** every item below marked [confirmed open], the spot-check
> queue, and the gaps found by the 2026-10-03 re-audit were closed in the
> gap pass recorded at the top of `docs/ACCESSIBILITY-PLAN.md` (GroupBox:
> won't-fix). What remains is the per-platform screen-reader walkthrough
> at the bottom of this file — none of it has been done yet.

Audit of `a11y` (rebased onto `development` @ `2139c42b`, 2026-09-02) against
the view/widget tree, followed by a 6-commit gap-closing sweep the same day.
Phase 1–3 (foundation + Qt6/GTK4/macOS/Windows bridges) were in place and
wired before this audit; this doc tracked the Phase 4 "widget-by-widget
mapping" backlog that had accumulated as the app grew past the original a11y
commits.

Legend: **[DONE `<commit>`]** · **[confirmed open]** verified still missing ·
**[check]** not re-verified since the original audit.

---

## Base-widget gaps

- **`tk::SearchablePicker`** [DONE `c3867bdc`] — now `Role::ComboBox` mirroring
  `tk::ComboBox` (name/state/default-action). Fixes `TimezonePicker`,
  `LanguagePicker`.
- **`SettingsGroup`** [DONE `c3867bdc`] — `Role::Group` + header as
  `access_name()`. Fixes every Settings page and RoomSettings section.
- **`tk::GroupBox`** [confirmed open, deliberately skipped] — no
  `access_role()`. Skipped by design: it's a decorative leaf with no children
  of its own (the grouped widgets are siblings, not descendants), so a
  `Role::Group` node would be empty — needs an ownership/relation mechanism
  the framework doesn't have. Only `CreateRoomView` uses it; revisit if that
  view is reworked to actually parent its widgets under the box.
- **`tk::ImageView`** (non-`Avatar`) [check] — no role. Fine if only
  decorative; not re-checked this pass.

## Previously fully unmapped views

- **`PopupMenu`** [DONE `971b25d5`] — `WidgetRowAccessibility` on `MenuList`;
  rows are `Role::MenuItem` (separators/disabled → `Role::None`), activation
  routes through the same `on_row_activated` the mouse path uses. Still
  mouse-*and-AT*-only for now — no `on_key_down` (Up/Down/Enter/Escape) was
  added, so sighted keyboard users still can't drive it without a mouse or AT
  client. [confirmed open, smaller item]
- **`tk::ComboBox` / `tk::SearchablePicker` dropdown content** [DONE
  `971b25d5`] — `DropdownList` in both → `WidgetRowAccessibility`, options as
  `Role::ListItem` with `state.selected`.
- **`RoomHeader`** icon buttons [DONE `971b25d5`] — all 6
  (back/calendar/threads/search/call/more) have `set_accessible_name()`. The
  canvas-painted presence/member-count status line is **still unexposed**
  [confirmed open] — `RoomHeader` itself has no `access_role()`/`access_name()`
  override.
- **`QuickSwitcher`** [DONE `14191e92`] — `ListAdapterAccessibility` on the
  adapter; room/user rows named, activation = row-click path.
- **`MessageSearchView`** [DONE `14191e92`] — `ListAdapterAccessibility`;
  "{room}, {sender}: {snippet}, {time}" per result.
- **`RoomMediaView`** [DONE `14191e92`, partial] — `ListAdapterAccessibility`
  at row level (month headers as `StaticText`, thumbnail strips as
  `ListItem`). **Per-thumbnail nodes still missing** [confirmed open] — needs
  the 2-D grid-table model (`GridAdapterAccessibility` only gives a flat
  linear position, not real row/column semantics); same blocker as the
  emoji/sticker pickers.
- **`MruSwitcher`** [DONE `14191e92`] — `WidgetRowAccessibility`; one
  `ListItem` per chip, held selection carries `state.selected`.
- **`CallOverlayWidget`** [DONE `971b25d5`] — all 6 buttons named, updated
  live in `paint()` to track toggle state.
- **`DevicesSection` / `DeviceRow`** [DONE `14191e92`] — `Role::Group`
  summarising name/this-device/verification/last-seen so the action buttons
  have context.

## Partial gaps in already-mapped views

- **`MessageListView`** — flattened row name still [confirmed open] on three
  points: reply context ("in reply to X") isn't mentioned; the `(edited)`
  marker isn't mentioned; a Markdown table reads as raw markdown source, not
  announced as a table. **The interactive-subtree gap is now [DONE
  `3d3b762f`]** for reactions (toggle group), the hover-action pill
  (react/reply/thread/edit/more), and read receipts — via the new
  `AccessNode::activate` + `ListAdapterAccessibility::access_subtree_for_row()`
  mechanism. **Still not covered by the subtree** [confirmed open]: URL-preview
  cards, the "N replies" thread-preview chip, inline hyperlinks, and media
  transport controls (voice/audio/video play buttons) inside a message body.
- **`ComposeBar`** [confirmed open] — icon buttons are named (emoji/sticker/
  voice/remove-attachment), but the reply/edit context banner ("Replying to
  X" + its close button) and the attachment-preview strip are still
  canvas-only, not exposed.
- **`ConfirmDialog` / `AlertDialog`** [DONE `971b25d5`, partial] — now
  `Role::Dialog` with `access_name()` combining title + body. **No
  modal/focus-containment semantics** [confirmed open] — the framework has no
  concept of this; an AT client isn't told the rest of the app is inert while
  a dialog is open.
- **`VerificationBanner`** [DONE `10b803a6`] · **`IncomingCallBanner`** [DONE
  `10b803a6`] — both `Role::Group` naming the canvas text; action buttons were
  already named.

## Spot-check queue

Done this pass: **`UserProfilePanel`**, **`RoomInfoPanel`** [DONE
`2cf3a747`] · **`InviteCard`**, **`RoomPreviewView`** [DONE `10b803a6`,
listed under banners above].

Still queued, not yet looked at: `LoginView` (first-run; compose-real-widgets
shape, likely mostly free — spot-check field labels + error text),
`PinnedBanner`, `KnockStatusCard`, `KnockRequestsPanel`,
`EncryptionSetupOverlay`, `QRGrantView`, `ExportHistoryDialog`,
`DatePickerView` (day grid needs the same grid-cell work as media/emoji),
`ImagePackEditorView`, `RoomSettingsView` + its sections, `AvatarEditControl`,
`SpaceRootView`, `PopoutRoomWidget`, `RoomView` chrome, `MainAppWidget` (root
role?), `BrandView`.

## Build-fix note

`d681d4b9` — the sweep's `win32_accessible.cpp` surfaced a latent Windows
unity-build bug (two `.cpp` files in the same CMake unity target each defined
their own anonymous-namespace `utf8_to_wide()`; MSVC's unity build merges
every file's unnamed namespace into one TU, causing a C2084 redefinition). Not
an accessibility bug per se, but blocked the Windows build until fixed.
User-confirmed: all four platforms build clean as of this fix.

---

## Testing on each platform

Nothing above has been verified against a real screen reader yet — that's the
actual acceptance test (per `docs/ACCESSIBILITY-PLAN.md`), not just "produces
a plausible-looking `AccessNode` tree" in a unit test. Do this per platform,
in this rough order: **main window → Ctrl-K quick switcher → right-click
context menu → Settings → a message with reactions (hover it) → a dialog
(e.g. leave-room confirm)**.

### Linux — Qt6 and GTK4 (test both; separate bridges)

1. Start Orca if it isn't already running: `orca &` (or enable it from
   GNOME Settings → Accessibility → Screen Reader). It talks over AT-SPI to
   both backends automatically — no per-app setup needed.
2. Launch the build under test:
   `./build/linux-debug/ui/linux-qt/tesseract` or
   `./build/linux-debug/ui/linux-gtk/tesseract`.
3. Tab through the main window; Orca should announce each control's role +
   name (room list items, the tab strip, header buttons).
4. Ctrl-K, type a room name, arrow through results, Enter — Orca should read
   each result and announce the switch.
5. Right-click a message → context menu should read as a list of menu items,
   arrow/Enter (or click) should activate one.
6. Open Settings (the gear icon) — each section header should be announced,
   controls inside should read as grouped under it.
7. Hover a message with reactions — the reaction chips and hover-action pill
   should be individually announced when explored (not just the flattened
   message name).
8. Trigger a confirm dialog (e.g. leave a room) — Orca should announce it as
   a dialog with the title/body text, and the buttons should be reachable.
9. **Inspection tool** (optional, more precise than listening): `accerciser`
   — shows the live AT-SPI tree so you can confirm exact roles/names/states
   without relying on speech, and click "Highlight" to visually map a node
   back to a widget on screen.

### Windows — Narrator and NVDA

1. **Narrator** (built-in): `Win+Ctrl+Enter` to toggle it on/off. Or install
   **NVDA** (free, https://www.nvaccess.org/) for a second independent check
   — the two sometimes disagree, and NVDA has a more detailed speech viewer.
2. Launch `tesseract.exe` from the build under test.
3. Same walkthrough as Linux: Tab through the main window, Ctrl-K switcher,
   right-click context menu, Settings, hover a reacted message, a confirm
   dialog.
4. Narrator: `Caps Lock+F1` opens its commands help if you need it mid-test.
   NVDA: `Insert+N` opens its menu; its **Speech Viewer** (Tools menu) shows
   a live transcript of everything it announces, useful for catching a
   silent/unlabeled control you'd otherwise miss by ear.
5. **Inspection tool**: **Accessibility Insights for Windows**
   (https://accessibilityinsights.io/) — its "Live Inspect" mode shows the
   UI Automation tree under the cursor in real time: role, name, and
   supported patterns (Invoke, Toggle, SelectionItem, …), which maps
   directly back to `to_uia_control_type`/`has_action_role` in
   `win32_accessible.cpp`.

### macOS — VoiceOver

1. `Cmd+F5` to toggle VoiceOver (or System Settings → Accessibility →
   VoiceOver). The VoiceOver practice/tutorial is worth a quick run first if
   you haven't driven it before — its interaction model (VO+arrow keys to
   navigate, VO+Space to activate) is less discoverable than Narrator/Orca.
2. Launch `Tesseract.app` from the build under test.
3. Same walkthrough: VO+arrow through the main window, Ctrl-K switcher,
   right-click (or VO+Shift+M) for the context menu, Settings, hover/VO-move
   to a reacted message, a confirm dialog.
4. **Inspection tool**: **Accessibility Inspector** (ships with Xcode —
   Xcode → Open Developer Tool → Accessibility Inspector). Point it at the
   app window to see the live `NSAccessibility` tree (role, label, value)
   without needing VoiceOver's speech running at the same time; its "Audit"
   tab also flags some common issues automatically.

### What to actually look for (all platforms)

- Every interactive control has a **non-empty, meaningful name** — not "Button"
  or silence.
- **Activating** a node from the AT client (not just clicking with a mouse)
  produces the same effect as a click — this is exactly what
  `access_default_action()`/`access_activate_row()`/`invoke_default_action()`
  are supposed to guarantee, so a mismatch here is a real bug, not just a
  missing label.
- **State** is announced and updates live: a toggled reaction reads as
  checked/unchecked, an expanded ComboBox/SearchablePicker reads as expanded,
  a selected tab/switcher row reads as selected.
- Reading a **grouped region** (a SettingsGroup, a dialog, a banner) announces
  the group's name before its controls, so context isn't lost.
- Nothing announces as a completely generic, unlabeled node when it's
  something a sighted user would recognize immediately (an icon-only button,
  a status banner, a device row).
