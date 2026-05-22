# Group Inactive Rooms — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an Appearance → "Room list" settings group with a "Group inactive rooms" toggle + period selector that, when on, shows a fifth "Inactive" room-list section for DMs/Rooms with no activity past the configured period (default 1 month).

**Architecture:** Two local `tesseract::Settings` fields; classification extracted to a pure `classify_room_section()` used by `RoomListView::rebuild_items()`; UI is a checkbox + `tk::ComboBox` in the shared `AppearanceSection`; persistence + an immediate `RoomListView::refresh()` are wired per-shell (mirroring the existing theme/prefetch wiring).

**Tech Stack:** C++17, the in-tree `tk` toolkit (`tk::CheckButton`, `tk::ComboBox`), Catch2/ctest. Primary build/test target: `linux-qt6-debug`. `linux-gtk-debug` also builds locally; Windows/macOS shells are written by analogy and verified in CI / on those platforms.

**Design spec:** `docs/superpowers/specs/2026-05-22-group-inactive-rooms-design.md`

---

## Background the engineer needs

- `tesseract::Settings` (`client/include/tesseract/settings.h`) is a local singleton persisted to `app_settings.json` via `load_from_disk`/`save_to_disk` (`client/src/settings.cpp`). Shared views read it directly via `Settings::instance()`.
- `client/src/settings.cpp` uses hand-rolled flat-JSON helpers `extract_string` / `extract_bool`. There is **no** int extractor yet.
- `RoomListView` (`ui/shared/views/RoomListView.{h,cpp}`) has four mutually-exclusive sections classified inline in `rebuild_items()`; `kSecFavorites=0, kSecDMs=1, kSecRooms=2, kSecSpaces=3, kNumSections=4`, `kSectionTitles[]`. `set_rooms()` stores `rooms_` then calls `rebuild_items()`. `rebuild_items()` skips empty sections (`if (section_rooms_[s].empty()) continue;`). `collapsed_[kNumSections] = {}` is in-memory only.
- `tesseract::RoomInfo.last_activity_ts` is Unix ms of last activity, or 0.
- Settings UI: `AppearanceSection` (`ui/shared/views/settings/AppearanceSection.{h,cpp}`) is a `SettingsPage`; its ctor does `add_group("Theme")` then adds a `ThemePicker`. `NotificationsSection` shows the pattern for a `tk::CheckButton` inside a group and self-seeding from `Settings::instance()` in the ctor. `SettingsGroup::add_widget<W>(unique_ptr<W>)` returns a borrowed pointer.
- `tk::CheckButton` (`ui/shared/tk/controls.h`): ctor `CheckButton(std::string label, bool checked=false)`; `set_checked(bool)`, `checked()`, `set_enabled(bool)`, `std::function<void(bool)> on_change`.
- `tk::ComboBox` (`ui/shared/tk/combobox.h`): `struct Option { std::string label; std::string value; }`; `set_options(std::vector<Option>)`, `set_selected_value(const std::string&)`, `selected_value()`, `std::function<void(std::string value)> on_changed`, `set_popup_clip(Rect)` (constrain dropdown popup to a world-space rect — call from the parent's `arrange`). See usage in `ui/shared/views/RoomInfoPanel.cpp` (`set_options({...})`, `on_changed`, `set_popup_clip(panel_rect_)`).
- `SettingsView` (`ui/shared/views/SettingsView.{h,cpp}`) owns the section widgets; it forwards section callbacks (e.g. `on_theme_changed`) and exposes silent seeders (e.g. `set_theme_pref`). `SettingsView.cpp` ~line 35 builds `AppearanceSection` and forwards `appearance->on_theme_changed`.
- Per-shell settings wiring anchors (mirror these exactly):
  - **linux-qt:** `ui/linux-qt/src/SettingsWidget.cpp:28` (`on_theme_changed` → `emit themeChanged`), `:51` (`on_prefetch_changed` self-contained lambda: writes Settings + `save_to_disk`), `:83` (`set_theme_pref` seed in `setInitialValues`). Signals declared in `ui/linux-qt/src/SettingsWidget.h:46`. `ui/linux-qt/src/MainWindow.cpp:3716` connects `SettingsWidget::themeChanged` → `set_theme_preference_`; `mainApp_->room_list_view()` is the room list.
  - **linux-gtk:** `ui/linux-gtk/src/MainWindow.cpp:1891` (`settings_widget_->on_theme_changed` → `set_theme_preference_(pref)`), `:4677` (`set_theme_pref` seed). `MainWindow` is the shell; `room_list_view_` is the room list.
  - **windows:** `ui/windows/src/MainWindow.cpp:2569` (`settings_view_->on_theme_changed` → `set_theme_preference_`), `:2592` (`on_prefetch_changed` self-contained), `:3106` (`set_theme_pref` seed). `room_list_view_` is the room list.
  - **macos:** `ui/macos/src/MainWindowController.mm:3112` (`_settingsView->on_theme_changed` → `s->_shell->set_theme_preference_`), `:4276` (`set_theme_pref` seed). `_roomListView` is the room list.
- `tesseract::config_dir()` (`tesseract/paths.h`) gives the settings dir; `Settings::instance().save_to_disk(tesseract::config_dir())` is the persistence call used everywhere.

### Build & test commands

```bash
cmake --preset linux-qt6-debug && cmake --build build/linux-qt6-debug
ctest --test-dir build/linux-qt6-debug --output-on-failure
# GTK shell (also builds on this Linux host):
cmake --preset linux-gtk-debug && cmake --build build/linux-gtk-debug
```

Run the qt6 build + ctest after every shared/qt task. The Windows and macOS shells cannot be compiled on this Linux host — Tasks 7–8 are written by faithful analogy to the theme/prefetch wiring and verified by review + platform CI.

---

## Task 1: Settings fields + serialization

**Files:**
- Modify: `client/include/tesseract/settings.h`
- Modify: `client/src/settings.cpp`
- Test: `tests/cpp/test_settings.cpp`

- [ ] **Step 1: Add a failing round-trip test.** In `tests/cpp/test_settings.cpp`, find the existing save/load round-trip test (it writes a temp dir, sets fields, `save_to_disk`, mutates, `load_from_disk`, asserts). Add a new `TEST_CASE` (use a unique temp dir like the existing tests):

```cpp
TEST_CASE("Settings persist group_inactive_rooms + threshold", "[settings]")
{
    auto dir = std::filesystem::temp_directory_path() /
               "tess_settings_inactive_test";
    std::filesystem::remove_all(dir);

    auto& s = tesseract::Settings::instance();
    s.group_inactive_rooms = true;
    s.inactive_room_threshold_days = 90;
    s.save_to_disk(dir);

    s.group_inactive_rooms = false;
    s.inactive_room_threshold_days = 30;
    s.load_from_disk(dir);

    CHECK(s.group_inactive_rooms == true);
    CHECK(s.inactive_room_threshold_days == 90);

    std::filesystem::remove_all(dir);
}
```

- [ ] **Step 2: Build to verify it fails.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | head -20`
Expected: compile error — `Settings` has no member `group_inactive_rooms`.

- [ ] **Step 3: Add the fields.** In `client/include/tesseract/settings.h`, after the `prefetch_full_media` block (~line 101) add:

```cpp
    // ── Room list ─────────────────────────────────────────────────────
    // Group rooms with no activity for `inactive_room_threshold_days` into a
    // separate "Inactive" room-list section (DMs + Rooms only). Default off.
    bool group_inactive_rooms = false;
    int inactive_room_threshold_days = 30;
```

- [ ] **Step 4: Add an int extractor.** In `client/src/settings.cpp`, after `extract_bool` add:

```cpp
// Extractor for a bare JSON integer by key. Returns `default_value` when the
// key is absent or no digits follow.
static int extract_int(const std::string& json, const std::string& key,
                       int default_value)
{
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos)
    {
        return default_value;
    }
    pos += needle.size();
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    {
        ++pos;
    }
    std::size_t end = pos;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9')
    {
        ++end;
    }
    if (end == pos)
    {
        return default_value;
    }
    return std::stoi(json.substr(pos, end - pos));
}
```

- [ ] **Step 5: Parse the fields in `load_from_disk`.** In `client/src/settings.cpp`, after the `prefetch_full_media = extract_bool(...)` line add:

```cpp
    group_inactive_rooms = extract_bool(json, "group_inactive_rooms", false);
    inactive_room_threshold_days =
        extract_int(json, "inactive_room_threshold_days", 30);
```

- [ ] **Step 6: Write the fields in `save_to_disk`.** In `client/src/settings.cpp`, extend the `f << ...` JSON object: change the final `<< ",\"prefetch_full_media\":" << (prefetch_full_media ? "true" : "false") << "}";` so the `"}"` moves to after the two new keys:

```cpp
      << ",\"prefetch_full_media\":"
      << (prefetch_full_media ? "true" : "false")
      << ",\"group_inactive_rooms\":"
      << (group_inactive_rooms ? "true" : "false")
      << ",\"inactive_room_threshold_days\":" << inactive_room_threshold_days
      << "}";
```

- [ ] **Step 7: Build + run the test.**

Run: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure -R settings`
Expected: PASS, including the new case.

- [ ] **Step 8: Commit.**

```bash
git add client/include/tesseract/settings.h client/src/settings.cpp tests/cpp/test_settings.cpp
git commit -m "feat(settings): add group_inactive_rooms + inactive_room_threshold_days"
```

---

## Task 2: RoomListView Inactive section + classify helper + refresh

**Files:**
- Modify: `ui/shared/views/RoomListView.h`
- Modify: `ui/shared/views/RoomListView.cpp`
- Create: `tests/cpp/test_room_list_sections.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing classifier test.** Create `tests/cpp/test_room_list_sections.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "tesseract/types.h"
#include "views/RoomListView.h"

using tesseract::RoomInfo;
using namespace tesseract::views;

namespace
{
constexpr uint64_t kNow = 1'000'000'000'000ULL; // fixed "now" in ms
constexpr uint64_t kDayMs = 86'400'000ULL;

RoomInfo room(bool fav, bool dm, bool space, uint64_t last_ts)
{
    RoomInfo r;
    r.is_favorite = fav;
    r.is_direct = dm;
    r.is_space = space;
    r.last_activity_ts = last_ts;
    return r;
}
} // namespace

TEST_CASE("classify: favorites/spaces never grouped", "[roomlist][inactive]")
{
    // Aged favorite stays in Favorites even with grouping on.
    auto fav = room(true, false, false, kNow - 60 * kDayMs);
    CHECK(classify_room_section(fav, true, 30, kNow) ==
          RoomListView::kSecFavorites);
    auto space = room(false, false, true, kNow - 60 * kDayMs);
    CHECK(classify_room_section(space, true, 30, kNow) ==
          RoomListView::kSecSpaces);
}

TEST_CASE("classify: DMs and Rooms group when inactive", "[roomlist][inactive]")
{
    auto dm_old = room(false, true, false, kNow - 60 * kDayMs);
    auto dm_new = room(false, true, false, kNow - 1 * kDayMs);
    CHECK(classify_room_section(dm_old, true, 30, kNow) ==
          RoomListView::kSecInactive);
    CHECK(classify_room_section(dm_new, true, 30, kNow) ==
          RoomListView::kSecDMs);

    auto room_old = room(false, false, false, kNow - 60 * kDayMs);
    auto room_new = room(false, false, false, kNow - 1 * kDayMs);
    CHECK(classify_room_section(room_old, true, 30, kNow) ==
          RoomListView::kSecInactive);
    CHECK(classify_room_section(room_new, true, 30, kNow) ==
          RoomListView::kSecRooms);
}

TEST_CASE("classify: zero last_activity_ts is inactive", "[roomlist][inactive]")
{
    auto r = room(false, false, false, 0);
    CHECK(classify_room_section(r, true, 30, kNow) ==
          RoomListView::kSecInactive);
}

TEST_CASE("classify: grouping off keeps normal sections", "[roomlist][inactive]")
{
    auto dm_old = room(false, true, false, kNow - 60 * kDayMs);
    CHECK(classify_room_section(dm_old, false, 30, kNow) ==
          RoomListView::kSecDMs);
}

TEST_CASE("classify: threshold boundary is strict", "[roomlist][inactive]")
{
    // Exactly threshold old → still active; one ms older → inactive.
    auto at = room(false, false, false, kNow - 30 * kDayMs);
    auto past = room(false, false, false, kNow - 30 * kDayMs - 1);
    CHECK(classify_room_section(at, true, 30, kNow) == RoomListView::kSecRooms);
    CHECK(classify_room_section(past, true, 30, kNow) ==
          RoomListView::kSecInactive);
}
```

- [ ] **Step 2: Register the test.** In `tests/CMakeLists.txt`, add `cpp/test_room_list_sections.cpp` to the test sources list (alongside the other `cpp/test_*.cpp` entries, e.g. after `cpp/test_tk_lists.cpp`).

- [ ] **Step 3: Build to verify it fails.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | head -25`
Expected: compile error — `classify_room_section` / `kSecInactive` not declared.

- [ ] **Step 4: Add section constants + free function decl to the header.** In `ui/shared/views/RoomListView.h`, the section constants are currently in the `private:` block. Make them **public** and add the Inactive section. Move this block into a `public:` section of `RoomListView` (e.g. right after the existing public methods), replacing the old private copy:

```cpp
public:
    // Section indices (matches array positions throughout the class).
    static constexpr int kSecFavorites = 0;
    static constexpr int kSecDMs = 1;
    static constexpr int kSecRooms = 2;
    static constexpr int kSecSpaces = 3;
    static constexpr int kSecInactive = 4;
    static constexpr int kNumSections = 5;

    static constexpr const char* kSectionTitles[kNumSections] = {
        "Favorites", "Direct Messages", "Rooms", "Spaces", "Inactive"};

    // Re-run section classification (e.g. after a settings change) and repaint.
    void refresh();
```

Then, after the closing `};` of the `RoomListView` class (still inside
`namespace tesseract::views`), declare the pure classifier:

```cpp
// Pure room→section classifier. Favorites and Spaces are never grouped; when
// `group_inactive` is true, DMs and Rooms whose `last_activity_ts` is more than
// `threshold_days` before `now_ms` go to kSecInactive. `last_activity_ts == 0`
// (no known activity) counts as inactive.
int classify_room_section(const tesseract::RoomInfo& r, bool group_inactive,
                          int threshold_days, std::uint64_t now_ms);
```

Update the `Item.section` comment `// which section (0-3)` → `(0-4)`.

- [ ] **Step 5: Define the classifier + use it in `rebuild_items`.** In `ui/shared/views/RoomListView.cpp`, add the free function (file scope, inside `namespace tesseract::views`), near the top after the existing anonymous-namespace helpers:

```cpp
int classify_room_section(const tesseract::RoomInfo& r, bool group_inactive,
                          int threshold_days, std::uint64_t now_ms)
{
    if (r.is_favorite)
    {
        return RoomListView::kSecFavorites;
    }
    if (r.is_space)
    {
        return RoomListView::kSecSpaces;
    }
    if (group_inactive)
    {
        std::uint64_t threshold_ms =
            static_cast<std::uint64_t>(threshold_days) * 86'400'000ULL;
        // last_activity_ts == 0 → now_ms - 0 > threshold ⇒ inactive.
        if (now_ms - r.last_activity_ts > threshold_ms)
        {
            return RoomListView::kSecInactive;
        }
    }
    if (r.is_direct)
    {
        return RoomListView::kSecDMs;
    }
    return RoomListView::kSecRooms;
}
```

Then replace the inline `if (r.is_favorite) {...} else if ...` classification block in `rebuild_items()` (the `int sec; ... section_rooms_[sec].push_back(&r);` region) with a call to the helper. Compute `now_ms` once before the room loop and read the settings:

```cpp
    const auto& settings = tesseract::Settings::instance();
    const bool group_inactive = settings.group_inactive_rooms;
    const int threshold_days = settings.inactive_room_threshold_days;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
```

and inside the loop, after the search-filter `continue;`:

```cpp
        int sec = classify_room_section(r, group_inactive, threshold_days, now_ms);
        section_rooms_[sec].push_back(&r);
```

Add includes at the top of `RoomListView.cpp` if missing: `#include "tesseract/settings.h"` and `#include <chrono>`.

- [ ] **Step 6: Default-collapse the Inactive section + add `refresh()`.** In `RoomListView::RoomListView()` ctor body (after the existing list setup), add:

```cpp
    collapsed_[kSecInactive] = true; // Inactive starts collapsed to declutter.
```

And define `refresh()` in `RoomListView.cpp`:

```cpp
void RoomListView::refresh()
{
    rebuild_items();
    if (list_)
    {
        list_->invalidate_data();
    }
    set_selected_room(selected_room_id_cache_);
}
```

- [ ] **Step 7: Build + run the tests.**

Run: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure -R "roomlist|lists"`
Expected: PASS (the 5 new classifier cases + existing list tests).

- [ ] **Step 8: Commit.**

```bash
git add ui/shared/views/RoomListView.h ui/shared/views/RoomListView.cpp tests/cpp/test_room_list_sections.cpp tests/CMakeLists.txt
git commit -m "feat(roomlist): Inactive section via classify_room_section + refresh()"
```

---

## Task 3: AppearanceSection "Room list" group (checkbox + combobox)

**Files:**
- Modify: `ui/shared/views/settings/AppearanceSection.h`
- Modify: `ui/shared/views/settings/AppearanceSection.cpp`

- [ ] **Step 1: Add the API to the header.** In `ui/shared/views/settings/AppearanceSection.h`, add includes `#include "tk/controls.h"` and `#include "tk/combobox.h"` (for `tk::CheckButton` / `tk::ComboBox`), then inside the class add the callbacks, silent setters, and member pointers + an `arrange` override:

```cpp
    // ----- Room list group -----
    // Silently update the controls without firing the callbacks below.
    void set_group_inactive(bool enabled);
    void set_inactive_period(int days);

    // Fired when the user toggles grouping / changes the period.
    std::function<void(bool)> on_group_inactive_changed;
    std::function<void(int)>  on_inactive_period_changed;

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
```

and in the private members:

```cpp
    tk::CheckButton* group_inactive_cb_ = nullptr;
    tk::ComboBox*    period_combo_ = nullptr;
```

- [ ] **Step 2: Build the group in the ctor.** In `ui/shared/views/settings/AppearanceSection.cpp`, at the end of `AppearanceSection::AppearanceSection()` (after the Theme group), add a second group that self-seeds from `Settings::instance()`:

```cpp
    {
        const auto& s = tesseract::Settings::instance();
        auto* rl_group = add_group("Room list");

        auto cb = std::make_unique<tk::CheckButton>(
            "Group inactive rooms", s.group_inactive_rooms);
        group_inactive_cb_ = rl_group->add_widget(std::move(cb));
        group_inactive_cb_->on_change = [this](bool v)
        {
            if (on_group_inactive_changed) on_group_inactive_changed(v);
        };

        auto combo = std::make_unique<tk::ComboBox>();
        combo->set_options({
            {"1 week", "7"},
            {"2 weeks", "14"},
            {"1 month", "30"},
            {"3 months", "90"},
            {"6 months", "180"},
        });
        combo->set_selected_value(std::to_string(s.inactive_room_threshold_days));
        period_combo_ = rl_group->add_widget(std::move(combo));
        period_combo_->on_changed = [this](std::string value)
        {
            if (on_inactive_period_changed)
            {
                on_inactive_period_changed(std::stoi(value));
            }
        };
    }
```

**Note on enable/disable:** `tk::ComboBox` (per `combobox.h`) has **no** `set_enabled`. Do **not** call a non-existent method. Instead, leave the combobox always interactive; the period only takes effect when grouping is on (the shell ignores period changes when the toggle is off is unnecessary — the value is still persisted and used once grouping is enabled). Remove the `period_combo_->set_enabled...` placeholder line above; the checkbox `on_change` body is simply `if (on_group_inactive_changed) on_group_inactive_changed(v);`. (If a disabled-look is desired later, add `set_enabled` to `tk::ComboBox` in a separate change — out of scope here.)

- [ ] **Step 3: Add the silent setters + arrange override.** In `AppearanceSection.cpp`:

```cpp
void AppearanceSection::set_group_inactive(bool enabled)
{
    if (group_inactive_cb_) group_inactive_cb_->set_checked(enabled);
}

void AppearanceSection::set_inactive_period(int days)
{
    if (period_combo_) period_combo_->set_selected_value(std::to_string(days));
}

void AppearanceSection::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    SettingsPage::arrange(ctx, bounds);
    // Constrain the combobox dropdown popup to the page bounds so it does not
    // paint outside the settings panel (mirrors RoomInfoPanel).
    if (period_combo_)
    {
        period_combo_->set_popup_clip(bounds);
    }
}
```

Add `#include <string>` if not already present.

- [ ] **Step 4: Build.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 5: Commit.**

```bash
git add ui/shared/views/settings/AppearanceSection.h ui/shared/views/settings/AppearanceSection.cpp
git commit -m "feat(settings-ui): Room list group (group-inactive toggle + period combobox)"
```

---

## Task 4: SettingsView forwards the callbacks + seeders

**Files:**
- Modify: `ui/shared/views/SettingsView.h`
- Modify: `ui/shared/views/SettingsView.cpp`

- [ ] **Step 1: Add the public API.** In `ui/shared/views/SettingsView.h`, near `on_theme_changed` / `set_theme_pref`, add:

```cpp
    // Room list grouping (forwarded from AppearanceSection).
    std::function<void(bool)> on_group_inactive_changed;
    std::function<void(int)>  on_inactive_period_changed;
    void set_group_inactive_pref(bool enabled);
    void set_inactive_period_pref(int days);
```

- [ ] **Step 2: Forward from AppearanceSection.** In `ui/shared/views/SettingsView.cpp`, where the appearance section is built (right after `appearance->on_theme_changed = ...`), add:

```cpp
    appearance->on_group_inactive_changed = [this](bool v)
    {
        if (on_group_inactive_changed) on_group_inactive_changed(v);
    };
    appearance->on_inactive_period_changed = [this](int days)
    {
        if (on_inactive_period_changed) on_inactive_period_changed(days);
    };
```

- [ ] **Step 3: Implement the seeders.** In `ui/shared/views/SettingsView.cpp` (near `set_theme_pref`):

```cpp
void SettingsView::set_group_inactive_pref(bool enabled)
{
    if (appearance_) appearance_->set_group_inactive(enabled);
}

void SettingsView::set_inactive_period_pref(int days)
{
    if (appearance_) appearance_->set_inactive_period(days);
}
```

- [ ] **Step 4: Build.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 5: Commit.**

```bash
git add ui/shared/views/SettingsView.h ui/shared/views/SettingsView.cpp
git commit -m "feat(settings): SettingsView forwards room-list grouping callbacks"
```

---

## Task 5: linux-qt wiring (primary target)

**Files:**
- Modify: `ui/linux-qt/src/SettingsWidget.h`
- Modify: `ui/linux-qt/src/SettingsWidget.cpp`
- Modify: `ui/linux-qt/src/MainWindow.cpp`

- [ ] **Step 1: Add a refresh signal.** In `ui/linux-qt/src/SettingsWidget.h`, beside the existing `void themeChanged(...)` signal (~line 46) add:

```cpp
    void roomListGroupingChanged();
```

- [ ] **Step 2: Wire the callbacks (self-contained persist + emit refresh).** In `ui/linux-qt/src/SettingsWidget.cpp`, beside the existing `on_prefetch_changed` self-contained lambda (~line 51), add:

```cpp
    settings_view_->on_group_inactive_changed = [this](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.group_inactive_rooms = enabled;
        s.save_to_disk(tesseract::config_dir());
        emit roomListGroupingChanged();
    };
    settings_view_->on_inactive_period_changed = [this](int days)
    {
        auto& s = tesseract::Settings::instance();
        s.inactive_room_threshold_days = days;
        s.save_to_disk(tesseract::config_dir());
        emit roomListGroupingChanged();
    };
```

- [ ] **Step 3: Seed the controls.** In `ui/linux-qt/src/SettingsWidget.cpp`, in `setInitialValues` beside `settings_view_->set_theme_pref(theme_pref);` (~line 83) add:

```cpp
    settings_view_->set_group_inactive_pref(
        tesseract::Settings::instance().group_inactive_rooms);
    settings_view_->set_inactive_period_pref(
        tesseract::Settings::instance().inactive_room_threshold_days);
```

- [ ] **Step 4: Connect the refresh in MainWindow.** In `ui/linux-qt/src/MainWindow.cpp`, beside the existing `connect(settingsWidget_, &SettingsWidget::themeChanged, ...)` (~line 3716), add:

```cpp
        connect(settingsWidget_, &SettingsWidget::roomListGroupingChanged, this,
                [this]
                {
                    if (mainApp_ && mainApp_->room_list_view())
                    {
                        mainApp_->room_list_view()->refresh();
                    }
                });
```

- [ ] **Step 5: Build + full ctest + manual smoke.**

Run: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure`
Expected: clean build, 100% tests pass.
Manual: launch `./build/linux-qt6-debug/ui/linux-qt/tesseract`, open Settings → Appearance → Room list, toggle "Group inactive rooms" → an "Inactive" section appears (collapsed) holding aged DMs/Rooms; change the period → reclassification; send a message to an inactive room → it leaves Inactive; toggle off → the section disappears.

- [ ] **Step 6: Commit.**

```bash
git add ui/linux-qt/src/SettingsWidget.h ui/linux-qt/src/SettingsWidget.cpp ui/linux-qt/src/MainWindow.cpp
git commit -m "feat(linux-qt): wire group-inactive-rooms settings + room-list refresh"
```

---

## Task 6: linux-gtk wiring

GTK uses a `gtk4::SettingsWidget` wrapper (`ui/linux-gtk/src/SettingsWidget.{h,cpp}`)
that re-exposes the inner `SettingsView`'s callbacks as its own members (its own
`on_theme_changed` forwarded from `settings_view_->on_theme_changed`, plus seeding
pass-throughs). The wrapper must forward the two new callbacks and expose two
seeders before `MainWindow` can use them.

**Files:**
- Modify: `ui/linux-gtk/src/SettingsWidget.h`
- Modify: `ui/linux-gtk/src/SettingsWidget.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`

- [ ] **Step 1: Forward callbacks + add seeders in the wrapper.** In `ui/linux-gtk/src/SettingsWidget.h`, beside the existing `on_theme_changed` member add:

```cpp
    std::function<void(bool)> on_group_inactive_changed;
    std::function<void(int)>  on_inactive_period_changed;
    void set_group_inactive_pref(bool enabled);
    void set_inactive_period_pref(int days);
```

In `ui/linux-gtk/src/SettingsWidget.cpp`, where the ctor wires
`settings_view_->on_theme_changed`, add the parallel forwarding and define the
seeders (match the actual inner `SettingsView` member name used by this wrapper):

```cpp
    settings_view_->on_group_inactive_changed = [this](bool v)
    {
        if (on_group_inactive_changed) on_group_inactive_changed(v);
    };
    settings_view_->on_inactive_period_changed = [this](int days)
    {
        if (on_inactive_period_changed) on_inactive_period_changed(days);
    };
```

```cpp
void SettingsWidget::set_group_inactive_pref(bool enabled)
{
    settings_view_->set_group_inactive_pref(enabled);
}
void SettingsWidget::set_inactive_period_pref(int days)
{
    settings_view_->set_inactive_period_pref(days);
}
```

- [ ] **Step 2: Wire + seed in MainWindow.** In `ui/linux-gtk/src/MainWindow.cpp`, beside `settings_widget_->on_theme_changed = [...]{ set_theme_preference_(pref); };` (~line 1891) add (the GTK `MainWindow` is the shell and owns `room_list_view_`):

```cpp
        settings_widget_->on_group_inactive_changed = [this](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.group_inactive_rooms = enabled;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_widget_->on_inactive_period_changed = [this](int days)
        {
            auto& s = tesseract::Settings::instance();
            s.inactive_room_threshold_days = days;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
```

Then seed the controls after the wrapper is created/before it is shown:

```cpp
        settings_widget_->set_group_inactive_pref(
            tesseract::Settings::instance().group_inactive_rooms);
        settings_widget_->set_inactive_period_pref(
            tesseract::Settings::instance().inactive_room_threshold_days);
```

(The GTK theme seeding around line 4677 is passed via the wrapper's init args; the
two `set_*_pref` calls go wherever the wrapper is populated/shown. Confirm
`room_list_view_` is the member used elsewhere, e.g. `room_list_view_->set_rooms(...)`
~line 3441.)

- [ ] **Step 3: Build (GTK preset builds on this host).**

Run: `cmake --preset linux-gtk-debug && cmake --build build/linux-gtk-debug 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 4: Commit.**

```bash
git add ui/linux-gtk/src/SettingsWidget.h ui/linux-gtk/src/SettingsWidget.cpp ui/linux-gtk/src/MainWindow.cpp
git commit -m "feat(linux-gtk): wire group-inactive-rooms settings + room-list refresh"
```

---

## Task 7: windows wiring (no local build — write by analogy)

**Files:**
- Modify: `ui/windows/src/MainWindow.cpp`

- [ ] **Step 1: Wire the callbacks.** In `ui/windows/src/MainWindow.cpp`, beside the existing `settings_view_->on_theme_changed` (~line 2569) and `on_prefetch_changed` (~line 2592, self-contained) add:

```cpp
        settings_view_->on_group_inactive_changed = [this](bool enabled)
        {
            auto& s = tesseract::Settings::instance();
            s.group_inactive_rooms = enabled;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
        settings_view_->on_inactive_period_changed = [this](int days)
        {
            auto& s = tesseract::Settings::instance();
            s.inactive_room_threshold_days = days;
            s.save_to_disk(tesseract::config_dir());
            if (room_list_view_) room_list_view_->refresh();
        };
```

- [ ] **Step 2: Seed the controls.** Beside the existing `set_theme_pref` seeding (~line 3106) add:

```cpp
    settings_view_->set_group_inactive_pref(
        tesseract::Settings::instance().group_inactive_rooms);
    settings_view_->set_inactive_period_pref(
        tesseract::Settings::instance().inactive_room_threshold_days);
```

- [ ] **Step 3: Verify by inspection.** This shell cannot be compiled on the Linux host. Confirm: the two callbacks mirror the theme/prefetch idiom exactly; `room_list_view_` is the same member used elsewhere in this file (e.g. `room_list_view_->set_rooms(...)` ~line 3987); `tesseract/settings.h` and `tesseract/paths.h` (`config_dir()`) are already included (they are, given `on_prefetch_changed` uses them). Build verification happens in Windows CI.

- [ ] **Step 4: Commit.**

```bash
git add ui/windows/src/MainWindow.cpp
git commit -m "feat(windows): wire group-inactive-rooms settings + room-list refresh"
```

---

## Task 8: macos wiring (no local build — write by analogy)

**Files:**
- Modify: `ui/macos/src/MainWindowController.mm`

- [ ] **Step 1: Wire the callbacks.** In `ui/macos/src/MainWindowController.mm`, beside the existing `_settingsView->on_theme_changed = [...]{ s->_shell->set_theme_preference_(pref); };` (~line 3112) and `on_prefetch_changed` (~line 3153) add (use the same weak-self capture idiom as the surrounding handlers — shown here as `ws`/`s` matching line 3153):

The adjacent `on_prefetch_changed` (line 3153) captures a weak self `ws` and
derefs as `MainWindowController* s = ws; if (!s) return;`. Mirror it exactly:

```cpp
        _settingsView->on_group_inactive_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().group_inactive_rooms = enabled;
            tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
            if (s->_roomListView) s->_roomListView->refresh();
        };
        _settingsView->on_inactive_period_changed = [ws](int days)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().inactive_room_threshold_days = days;
            tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
            if (s->_roomListView) s->_roomListView->refresh();
        };
```

- [ ] **Step 2: Seed the controls.** Beside the existing `_settingsView->set_theme_pref(...)` (~line 4276) add:

```cpp
    _settingsView->set_group_inactive_pref(
        tesseract::Settings::instance().group_inactive_rooms);
    _settingsView->set_inactive_period_pref(
        tesseract::Settings::instance().inactive_room_threshold_days);
```

- [ ] **Step 3: Verify by inspection.** Cannot be compiled on the Linux host. Confirm: handler idiom matches the adjacent theme/prefetch handlers; `_roomListView` is the room list member used elsewhere (e.g. `_roomListView->set_rooms(...)` ~line 5334); `tesseract/settings.h` + `config_dir()` are in scope. Build verification happens in macOS CI.

- [ ] **Step 4: Commit.**

```bash
git add ui/macos/src/MainWindowController.mm
git commit -m "feat(macos): wire group-inactive-rooms settings + room-list refresh"
```

---

## Final verification

- [ ] **Step 1: qt6 build + full ctest.** `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure` → all pass.
- [ ] **Step 2: GTK build.** `cmake --build build/linux-gtk-debug 2>&1 | tail -3` → clean.
- [ ] **Step 3: Manual smoke (qt6)** per Task 5 Step 5.
- [ ] **Step 4: Update STATUS.md** — add the room-list grouping feature, refresh Rust/C++ test counts + Last updated date (this is a user-facing feature). Commit.

> Windows/macOS shells are verified by review + platform CI, not on this Linux host. Per repo policy (CLAUDE.md): do not push or merge until the user confirms it works.

---

## Notes on API confirmation during implementation

- `tk::ComboBox` has no `set_enabled` (Task 3 Step 2) — the period combobox stays always-interactive; do not call a non-existent method.
- For each non-qt shell, match the EXACT accessor/capture idiom of the adjacent theme/prefetch handler (the `SettingsView` object name and, on macOS, the weak-self pattern). The new logic is identical everywhere: write the two `Settings` fields, `save_to_disk(config_dir())`, refresh that shell's room list.
- `AppearanceSection::arrange` popup-clip rect (Task 3 Step 3): if the dropdown clips wrong in practice, adjust the rect to the scroll viewport the section sits in (mirror whatever `RoomInfoPanel` passes to `set_popup_clip`).
