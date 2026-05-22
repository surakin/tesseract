# Group inactive rooms — design

**Date:** 2026-05-22
**Scope:** Add a "Room list" group to the Appearance settings with a "Group
inactive rooms" toggle and an inactivity-period selector. When enabled, the room
list shows a fifth section, **Inactive**, holding DMs and Rooms with no activity
for the configured period (default 1 month). Most work is in `ui/shared` plus the
`Settings` struct; the four shells each get a small settings-wiring edit (routing
the two new callbacks + seeding), exactly mirroring how they already wire the
theme preference.

## Goal

- A **Room list** group inside the existing **Appearance** settings tab, with:
  - a checkbox **"Group inactive rooms"** (default off),
  - an **"Inactive after:"** combobox (1 week / 2 weeks / 1 month / 3 months /
    6 months; default 1 month), disabled while the checkbox is off.
- When enabled, the room list gains a fifth collapsible section **Inactive**
  containing DMs and Rooms whose last activity is older than the threshold.
- The setting is local (persisted in `app_settings.json`), like `theme_pref`.

## Relevant existing code

- `client/include/tesseract/settings.h` — `tesseract::Settings` singleton
  (local, `load_from_disk`/`save_to_disk` to `app_settings.json`). Home of
  `theme_pref`, notification toggles, `prefetch_full_media`, etc.
- `client/src/settings.cpp` — the JSON load/save serialization.
- `ui/shared/views/settings/AppearanceSection.{h,cpp}` — the Appearance tab
  (`SettingsPage` with a Theme group; `on_theme_changed` callback; `set_selected`
  to seed silently).
- `ui/shared/views/SettingsView.{h,cpp}` — mounts the section widgets; forwards
  section callbacks (e.g. `on_theme_changed`) and exposes silent setters
  (`set_theme_pref`).
- `ui/shared/app/ShellBase.{h,cpp}` — wires `SettingsView` callbacks to
  `Settings::instance()` + `save_to_disk` (`set_theme_preference_`), and feeds
  rooms to the room list. Shared views read `Settings::instance()` directly
  (e.g. fonts in `MessageListView`, `prefetch_full_media` in `ShellBase`).
- `ui/shared/views/RoomListView.{h,cpp}` — the room list. Four mutually-exclusive
  sections: `kSecFavorites=0`, `kSecDMs=1`, `kSecRooms=2`, `kSecSpaces=3`,
  `kNumSections=4`, `kSectionTitles[]`. `set_rooms()` stores `rooms_` and calls
  `rebuild_items()`. `rebuild_items()` clears per-section buckets, classifies each
  room into one section, then builds a flat item list **skipping any empty
  section** (`if (section_rooms_[s].empty()) continue;`). Collapsed state is
  per-section in-memory (`collapsed_[]`).
- `tesseract::RoomInfo.last_activity_ts` — Unix ms of most recent activity, or 0.
- A combobox tk widget already exists (to be located during planning) and is the
  period control.

## Architectural decision — how RoomListView reads the setting

**Chosen: read `Settings::instance()` directly; classification extracted to a pure
helper.** `rebuild_items()` reads `group_inactive_rooms` and
`inactive_room_threshold_days` from the singleton and delegates per-room bucketing
to a free function:

```
int classify_room_section(const RoomInfo& r,
                          bool group_inactive,
                          int threshold_days,
                          uint64_t now_ms);
```

- Consistent with how shared views already read `Settings::instance()`.
- The pure helper is unit-testable without constructing the widget.

Rejected: plumbing the two values into `RoomListView` via setters — adds state
duplication and plumbing for no benefit; the singleton is the house style.

## Design

### 1. Settings model & persistence

Two new fields on `tesseract::Settings` (`settings.h`):

```cpp
// ── Room list ────────────────────────────────────────────────────
// Group rooms with no activity for `inactive_room_threshold_days` into a
// separate "Inactive" room-list section (DMs + Rooms only).
bool group_inactive_rooms = false;
int  inactive_room_threshold_days = 30;
```

Both added to `Settings::load_from_disk` / `save_to_disk` (`settings.cpp`),
following the existing field serialization. Local only (not synced account-data).

### 2. Appearance UI — "Room list" group

`AppearanceSection` gains a second `SettingsPage` group titled **"Room list"**,
rendered below the Theme group:

- Checkbox **"Group inactive rooms"** (reuse the settings checkbox widget used by
  Notifications/Media).
- Combobox **"Inactive after:"** with five entries mapping to day counts:
  1 week → 7, 2 weeks → 14, 1 month → 30, 3 months → 90, 6 months → 180.
  Default selection: 1 month. The combobox is disabled (greyed) when the checkbox
  is off.

New callbacks bubble up the existing chain `AppearanceSection → SettingsView →
ShellBase`:

- `on_group_inactive_changed(bool enabled)`
- `on_inactive_period_changed(int days)`

Silent seeders (set without firing callbacks), mirroring `set_selected` /
`set_theme_pref`:

- `AppearanceSection::set_group_inactive(bool)` / `set_inactive_period(int days)`
- `SettingsView::set_group_inactive_pref(bool)` / `set_inactive_period_pref(int days)`

### 3. Room list classification & the Inactive section

`RoomListView` grows to five sections:

- `kSecInactive = 4`, `kNumSections = 5`.
- `kSectionTitles[]` gains `"Inactive"` at index 4 (rendered last, after Spaces).
- The Inactive section starts **collapsed** by default (declutter): set
  `collapsed_[kSecInactive] = true` at construction. The other four keep their
  current default (expanded).

`rebuild_items()` computes `now_ms` once (system clock) and calls
`classify_room_section` per room:

```
classify_room_section(r, group_inactive, threshold_days, now_ms):
    if r.is_favorite: return kSecFavorites      // favorites never grouped
    if r.is_space:    return kSecSpaces          // spaces never grouped
    if group_inactive and is_inactive(r, threshold_days, now_ms):
        return kSecInactive                       // only DMs + Rooms reach here
    if r.is_direct:   return kSecDMs
    return kSecRooms

is_inactive(r, threshold_days, now_ms):
    // last_activity_ts == 0 (no known activity) counts as inactive.
    return now_ms - r.last_activity_ts > threshold_days * 86'400'000
```

Notes:
- `last_activity_ts == 0` → inactive (literally zero activity). With `now_ms`
  large, `now_ms - 0` exceeds any threshold, so the plain comparison already
  yields inactive — no special case needed.
- Favorites and Spaces are classified before the inactivity check, so they are
  never moved into Inactive.

**Empty Inactive section is hidden.** `rebuild_items()` already skips any section
whose bucket is empty, so the Inactive header appears only when at least one room
is classified inactive — and never when the toggle is off (no room is classified
inactive then).

**Reclassification on new activity is automatic.** An incoming message updates
`RoomInfo.last_activity_ts` and fires `on_rooms_updated` →
`room_list_view()->set_rooms(...)` → `rebuild_items()`, which re-runs
`classify_room_section` and moves the now-active room out of Inactive into its
proper DM/Room section. No special-case code is required. (While the Inactive
section is collapsed, the existing rule that shows unread/mention/selected rooms
inside a collapsed section still applies, so a just-arrived message is visible
even in the moment before the rebuild repositions it.)

**No periodic timer.** Inactivity is wall-clock dependent, but reclassification
happens on every rooms update (frequent) and on every setting change. A room that
crosses the threshold with no other activity rebuckets on the next rooms update —
acceptable staleness; a polling timer is out of scope (YAGNI).

### 4. Wiring

`ShellBase` does not retain the `MainAppWidget`, and each shell already owns and
feeds its own `RoomListView` (`set_rooms` is called per-shell). So persistence and
refresh are per-shell, inline — mirroring the existing self-contained
`on_prefetch_changed` handler (which writes `Settings` + `save_to_disk` in the
lambda) — plus a call to a new shared `RoomListView::refresh()` primitive
(re-runs `rebuild_items()` + `invalidate_data()`).

**Shared UI seeding:** `AppearanceSection` self-seeds its checkbox + combobox from
`Settings::instance()` in its constructor (the pattern `NotificationsSection`
uses), and exposes `set_group_inactive` / `set_inactive_period` setters that
`SettingsView::set_group_inactive_pref` / `set_inactive_period_pref` forward to,
so shells can re-seed on panel re-show (mirroring `set_theme_pref`).

**Shared callbacks:** `AppearanceSection` fires `on_group_inactive_changed(bool)`
and `on_inactive_period_changed(int days)` (the combobox parses its value string
to days); `SettingsView` forwards both.

**Per-shell wiring (unavoidable — this is how every settings toggle works
today).** Each shell routes the two callbacks to a lambda that writes
`Settings::instance()`, calls `save_to_disk(config_dir())`, and refreshes that
shell's room list, then seeds the two controls — parallel to the existing theme /
prefetch lines:

- `ui/linux-qt/src/SettingsWidget.cpp` — the lambda writes Settings + saves and
  emits a new `roomListGroupingChanged()` Qt signal; `MainWindow` (around the
  existing `themeChanged` connect) connects it to
  `mainApp_->room_list_view()->refresh()`. Seed in `SettingsWidget::setInitialValues`
  beside `set_theme_pref`.
- `ui/linux-gtk/src/MainWindow.cpp` and `ui/windows/src/MainWindow.cpp` — the
  `MainWindow` is the shell and owns `room_list_view_`; the lambda writes Settings
  + saves + `room_list_view_->refresh()`. Seed beside the existing `set_theme_pref`.
- `ui/macos/src/MainWindowController.mm` — lambda writes Settings + saves +
  `_roomListView->refresh()`. Seed beside the existing `set_theme_pref`.

The room list and settings view themselves stay shared; no `ShellBase` change.

## Testing

- **`tests/cpp/test_room_list_sections.cpp` (new):** unit-test the pure
  `classify_room_section`:
  - favorite past threshold → `kSecFavorites` (never grouped);
  - space past threshold → `kSecSpaces` (never grouped);
  - DM past threshold → `kSecInactive`; DM within threshold → `kSecDMs`;
  - Room past threshold → `kSecInactive`; Room within threshold → `kSecRooms`;
  - `last_activity_ts == 0` → `kSecInactive`;
  - `group_inactive == false` → never `kSecInactive` (DM/Room go to their normal
    section regardless of age);
  - boundary: activity exactly `threshold_days` old is **not** inactive (strict
    `>`), one ms older is.
- **`tests/cpp/test_settings.cpp` (extend):** round-trip `group_inactive_rooms`
  and `inactive_room_threshold_days` through `save_to_disk` / `load_from_disk`.
- **Manual smoke (Qt6):** enable the toggle → Inactive section appears with aged
  rooms; change the period → reclassification; send a message to an inactive room
  → it jumps back to Rooms/DMs; disable the toggle → Inactive section disappears.

## Out of scope

- Syncing the preference across devices (account-data) — local only, like theme.
- A custom/free-form duration beyond the five presets.
- A periodic re-evaluation timer.
- Per-section persisted collapse state.

## Files touched (anticipated)

- `client/include/tesseract/settings.h` — two new fields.
- `client/src/settings.cpp` — serialize/deserialize the two fields.
- `ui/shared/views/settings/AppearanceSection.{h,cpp}` — Room list group
  (checkbox + combobox), callbacks, silent seeders.
- `ui/shared/views/SettingsView.{h,cpp}` — forward callbacks + silent seeders.
- `ui/shared/views/RoomListView.{h,cpp}` — fifth section, `classify_room_section`
  helper, `now_ms`, default-collapsed Inactive, `refresh()`.
- Per-shell settings wiring (route 2 callbacks → write Settings + save + refresh
  that shell's room list; seed 2 controls — mirroring the theme/prefetch wiring):
  `ui/linux-qt/src/SettingsWidget.{h,cpp}` (+ its `MainWindow.cpp` signal/slot for
  the refresh), `ui/linux-gtk/src/MainWindow.cpp`, `ui/windows/src/MainWindow.cpp`,
  `ui/macos/src/MainWindowController.mm`.
- `tests/cpp/test_room_list_sections.cpp` (new) registered in
  `tests/CMakeLists.txt`, `tests/cpp/test_settings.cpp` (extend).
