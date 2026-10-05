# Screenshot Showcase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the CI screenshot mode capture a richer main view plus four extra scenes (thread, room-info, emoji, settings) on all four shells, driven by one shared director.

**Architecture:** A platform-free `ScreenshotDirector` (in `ui/shared/app/`) steps a table of scenes × {light, dark} through a 5-method `ScreenshotHost` interface. `ShellBase` owns fixture seeding and builds the scene table from shared views; each shell only adapts its existing native PNG grab + timer into a `ScreenshotHost`. The fixture grows more rooms, richer timeline rows, thread replies, members and two new embedded image assets.

**Tech Stack:** C++20, CMake, Catch2 v3, tesseract_tk shared views; Qt6 / GTK4 / Win32 / AppKit shells; GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-02-screenshot-showcase-design.md`

## Global Constraints

- All screenshot-mode code compiled only when `TESSERACT_ENABLE_SCREENSHOT_MODE=ON` (macro `TESSERACT_SCREENSHOT_MODE_ENABLED`) — release builds must not contain the fixture or the director.
- Window size stays 1100×768; settle delay 300 ms (Win32: 500 ms).
- Filenames: `<prefix>-<scene>-<theme>.png`; scene `main` keeps `<prefix>-<theme>.png`. Prefixes `qt6`, `gtk4`, `win`, `mac`. Scenes in order: `main`, `thread`, `room-info`, `emoji`, `settings`. Themes in order: `light`, `dark`.
- CLI stays `--screenshot-dir=<dir>`; no new flags.
- Fixture message bodies are sample data — not wrapped in `tk::tr`. Any *UI* string added must go through `tk::tr`/`tk::trf` and get entries in every `i18n/*.po`.
- Never commit without the user's explicit confirmation (CLAUDE.md). Every "Commit" step below means: stage, show the user, wait for a yes.
- Never launch the app; never build/run the GTK target (the user does that). Verification = Qt6 build + `ctest`. Win32/macOS cannot be built here — say so plainly.
- Shared logic lives in `ui/shared/`; shells contain only native grab/timer/settings-surface wiring.
- Don't run `cargo fmt` on `sdk/`. Searches must be scoped (no unscoped `find /`).

## Review Focus

1. **Settings with no `Client`** — opening settings in screenshot mode must not dereference a null client (Sessions/Account bind code). Expected: settings opens on Appearance, no crash. Pinned in Task 5 Step 4 (Qt live run by the user) and by the null-guard audit in Task 4 Step 6.
2. **Scene leakage** — a panel/picker left open bleeds into the next scene. Expected: each scene's teardown runs before the next scene's setup, and the last scene is torn down before `finish(true)`. Pinned in Task 1 test "teardown precedes next setup".
3. **Failed save mid-run** — expected: process exits non-zero promptly, no further saves, no hang. Pinned in Task 1 test "failed save stops the run".
4. **Missing asset key** — a fixture row references an avatar/media key that `install_assets` never stores → silently blank image. Pinned in Task 2 test "every referenced media key is installed".
5. **Overlapping timers** — Win32 uses one timer id for every `run_after`; a second `run_after` while one is pending would clobber the stored callback. Expected: director never has two pending. Pinned in Task 1 test "never more than one pending run_after".

---

## File Structure

| File | Responsibility |
|---|---|
| `ui/shared/app/ScreenshotDirector.h/.cpp` (new) | `ScreenshotHost` interface, `Scene`, `screenshot_filename()`, `ScreenshotDirector` sequencing. No view/platform includes. |
| `ui/shared/app/ScreenshotFixture.h/.cpp` (modify) | Fixture data (rooms, messages, thread replies, members, presence, typing) + embedded assets; `install_assets()`. |
| `ui/shared/views/RoomView.h/.cpp` (modify) | Add public `close_pickers()`. |
| `ui/shared/views/SettingsView.h/.cpp` (modify) | Add `show_appearance_section()`. |
| `ui/shared/app/ShellBase.h/.cpp` (modify) | `seed_screenshot_fixture_()`, `make_screenshot_scenes_()`, `start_screenshot_director_()`, new virtual `close_app_settings_ui_()`. |
| `ui/{linux-qt,linux-gtk,windows}/src/MainWindow.*`, `ui/macos/src/MainWindowController.mm` (modify) | `ScreenshotHost` adapter; delete old light→dark chains; implement `close_app_settings_ui_()`. |
| `ui/shared/CMakeLists.txt`, `tests/CMakeLists.txt` (modify) | Build wiring. |
| `tests/cpp/test_screenshot_director.cpp`, `tests/cpp/test_screenshot_fixture.cpp` (new) | Tests. |
| `.github/workflows/update-screenshots.yml`, `README.md`, `CHANGES.md` (modify) | CI checks, Feature tour, changelog. |

### Test-build wiring (applies to Tasks 1–2)

The normal dev build has screenshot mode OFF, so `tesseract_tk` does not contain the fixture/director. The tests compile those sources directly when the mode is OFF; when ON they come from `tesseract_tk` (avoids duplicate symbols under WHOLE_ARCHIVE). Add once, in Task 1:

```cmake
# tests/CMakeLists.txt — next to the other target_sources() blocks
target_sources(tesseract_tests PRIVATE
    cpp/test_screenshot_director.cpp
    cpp/test_screenshot_fixture.cpp)
if(NOT TESSERACT_ENABLE_SCREENSHOT_MODE)
    # Screenshot-mode sources aren't in tesseract_tk for normal builds;
    # compile them into the test binary so the director/fixture stay tested.
    target_sources(tesseract_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/ui/shared/app/ScreenshotDirector.cpp
        ${CMAKE_SOURCE_DIR}/ui/shared/app/ScreenshotFixture.cpp)
endif()
```

(Task 1 creates `test_screenshot_fixture.cpp` as an empty-but-valid file containing only `#include <catch2/catch_test_macros.hpp>` so the CMake line is valid from the start; Task 2 fills it.)

---

### Task 1: ScreenshotDirector

**Files:**
- Create: `ui/shared/app/ScreenshotDirector.h`, `ui/shared/app/ScreenshotDirector.cpp`
- Create: `tests/cpp/test_screenshot_director.cpp`, `tests/cpp/test_screenshot_fixture.cpp` (stub)
- Modify: `ui/shared/CMakeLists.txt:360-364`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class tesseract::screenshot::ScreenshotTheme { Light, Dark };`
  - `struct ScreenshotHost { virtual void apply_theme(ScreenshotTheme); virtual void refresh(); virtual bool save_png(const std::string& filename); virtual void run_after(int ms, std::function<void()> fn); virtual void finish(bool ok); }` (all pure virtual)
  - `struct Scene { std::string name; std::function<void()> setup; std::function<void()> teardown; };`
  - `std::string screenshot_filename(std::string_view prefix, std::string_view scene, ScreenshotTheme theme);`
  - `class ScreenshotDirector { ScreenshotDirector(ScreenshotHost&, std::vector<Scene>, std::string prefix, int settle_ms = 300); void start(); };`

- [ ] **Step 1: Write the failing tests**

`tests/cpp/test_screenshot_director.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "app/ScreenshotDirector.h"

#include <functional>
#include <string>
#include <vector>

using tesseract::screenshot::Scene;
using tesseract::screenshot::ScreenshotDirector;
using tesseract::screenshot::ScreenshotHost;
using tesseract::screenshot::ScreenshotTheme;
using tesseract::screenshot::screenshot_filename;

namespace
{

struct FakeHost : ScreenshotHost
{
    std::vector<std::string>* log = nullptr;
    std::vector<std::string> saved;
    std::function<void()> pending;
    int max_pending = 0;
    int finish_calls = 0;
    bool finish_ok = false;
    std::string fail_on; // filename whose save returns false

    void apply_theme(ScreenshotTheme t) override
    {
        log->push_back(t == ScreenshotTheme::Light ? "theme:light"
                                                   : "theme:dark");
    }
    void refresh() override { log->push_back("refresh"); }
    bool save_png(const std::string& f) override
    {
        log->push_back("save:" + f);
        saved.push_back(f);
        return f != fail_on;
    }
    void run_after(int, std::function<void()> fn) override
    {
        if (pending)
            max_pending = 2;
        else if (max_pending == 0)
            max_pending = 1;
        pending = std::move(fn);
    }
    void finish(bool ok) override
    {
        ++finish_calls;
        finish_ok = ok;
        log->push_back(ok ? "finish:ok" : "finish:fail");
    }
    // Drain like an event loop: run pending callbacks one at a time.
    void drain()
    {
        while (pending)
        {
            auto fn = std::move(pending);
            pending = nullptr;
            fn();
        }
    }
};

std::vector<Scene> scenes(std::vector<std::string>& log,
                          std::vector<std::string> names)
{
    std::vector<Scene> out;
    for (auto& n : names)
        out.push_back({n, [&log, n] { log.push_back("setup:" + n); },
                       [&log, n] { log.push_back("teardown:" + n); }});
    return out;
}

} // namespace

TEST_CASE("screenshot_filename keeps legacy names for main",
          "[screenshot]")
{
    CHECK(screenshot_filename("qt6", "main", ScreenshotTheme::Light) ==
          "qt6-light.png");
    CHECK(screenshot_filename("win", "main", ScreenshotTheme::Dark) ==
          "win-dark.png");
    CHECK(screenshot_filename("gtk4", "room-info", ScreenshotTheme::Dark) ==
          "gtk4-room-info-dark.png");
}

TEST_CASE("director saves every scene in light then dark, in order",
          "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    ScreenshotDirector d(host,
                         scenes(log, {"main", "thread", "room-info",
                                      "emoji", "settings"}),
                         "qt6");
    d.start();
    host.drain();

    CHECK(host.saved == std::vector<std::string>{
                            "qt6-light.png", "qt6-dark.png",
                            "qt6-thread-light.png", "qt6-thread-dark.png",
                            "qt6-room-info-light.png", "qt6-room-info-dark.png",
                            "qt6-emoji-light.png", "qt6-emoji-dark.png",
                            "qt6-settings-light.png", "qt6-settings-dark.png"});
    CHECK(host.finish_calls == 1);
    CHECK(host.finish_ok);
}

TEST_CASE("teardown precedes next setup and last teardown precedes finish",
          "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    ScreenshotDirector d(host, scenes(log, {"a", "b"}), "p");
    d.start();
    host.drain();

    CHECK(log == std::vector<std::string>{
                     "setup:a", "theme:light", "refresh", "save:p-a-light.png",
                     "theme:dark", "refresh", "save:p-a-dark.png",
                     "teardown:a",
                     "setup:b", "theme:light", "refresh", "save:p-b-light.png",
                     "theme:dark", "refresh", "save:p-b-dark.png",
                     "teardown:b", "finish:ok"});
}

TEST_CASE("failed save stops the run", "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    host.fail_on = "p-b-light.png";
    ScreenshotDirector d(host, scenes(log, {"a", "b", "c"}), "p");
    d.start();
    host.drain();

    CHECK(host.saved.back() == "p-b-light.png");
    CHECK(host.saved.size() == 3);
    CHECK(host.finish_calls == 1);
    CHECK_FALSE(host.finish_ok);
}

TEST_CASE("never more than one pending run_after", "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    ScreenshotDirector d(host, scenes(log, {"a", "b", "c"}), "p");
    d.start();
    host.drain();
    CHECK(host.max_pending == 1);
}

TEST_CASE("empty scene list finishes ok without saving", "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    ScreenshotDirector d(host, {}, "p");
    d.start();
    host.drain();
    CHECK(host.saved.empty());
    CHECK(host.finish_calls == 1);
    CHECK(host.finish_ok);
}
```

`tests/cpp/test_screenshot_fixture.cpp` (stub until Task 2):

```cpp
#include <catch2/catch_test_macros.hpp>
```

Add the CMake block from "Test-build wiring" above to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build/linux-debug --target tesseract_tests 2>&1 | tail -5`
Expected: FAIL — `app/ScreenshotDirector.h: No such file or directory`.

- [ ] **Step 3: Implement**

`ui/shared/app/ScreenshotDirector.h`:

```cpp
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::screenshot
{

enum class ScreenshotTheme
{
    Light,
    Dark,
};

/// Native side of screenshot capture, implemented by each shell.
struct ScreenshotHost
{
    virtual ~ScreenshotHost() = default;
    virtual void apply_theme(ScreenshotTheme theme) = 0;
    /// Relayout + schedule a repaint after a scene or theme change.
    virtual void refresh() = 0;
    virtual bool save_png(const std::string& filename) = 0;
    /// Run `fn` once on the UI thread after `ms`. The director never has
    /// more than one call pending, so a shell may back this with one timer.
    virtual void run_after(int ms, std::function<void()> fn) = 0;
    /// Called exactly once; quit the app with a matching exit status.
    virtual void finish(bool ok) = 0;
};

struct Scene
{
    std::string name;
    std::function<void()> setup;
    std::function<void()> teardown;
};

/// "main" keeps the pre-scene names (`qt6-light.png`) so README links hold.
std::string screenshot_filename(std::string_view prefix,
                                std::string_view scene,
                                ScreenshotTheme theme);

/// Steps scenes × {light, dark}: setup, then per theme apply/refresh/wait/
/// save, then teardown. Stops at the first failed save.
class ScreenshotDirector
{
public:
    ScreenshotDirector(ScreenshotHost& host, std::vector<Scene> scenes,
                       std::string prefix, int settle_ms = 300);

    void start();

private:
    void step_();

    ScreenshotHost& host_;
    std::vector<Scene> scenes_;
    std::string prefix_;
    int settle_ms_;
    std::size_t index_ = 0; // capture index: scene = index_/2, theme = index_%2
};

} // namespace tesseract::screenshot
```

`ui/shared/app/ScreenshotDirector.cpp`:

```cpp
#include "app/ScreenshotDirector.h"

#include <utility>

namespace tesseract::screenshot
{

std::string screenshot_filename(std::string_view prefix,
                                std::string_view scene,
                                ScreenshotTheme theme)
{
    std::string out{prefix};
    if (scene != "main")
    {
        out += '-';
        out += scene;
    }
    out += theme == ScreenshotTheme::Light ? "-light.png" : "-dark.png";
    return out;
}

ScreenshotDirector::ScreenshotDirector(ScreenshotHost& host,
                                       std::vector<Scene> scenes,
                                       std::string prefix, int settle_ms)
    : host_(host),
      scenes_(std::move(scenes)),
      prefix_(std::move(prefix)),
      settle_ms_(settle_ms)
{
}

void ScreenshotDirector::start()
{
    index_ = 0;
    step_();
}

void ScreenshotDirector::step_()
{
    const std::size_t scene = index_ / 2;
    const bool dark = index_ % 2 != 0;

    if (scene >= scenes_.size())
    {
        host_.finish(true);
        return;
    }

    if (!dark && scenes_[scene].setup)
        scenes_[scene].setup();

    const auto theme = dark ? ScreenshotTheme::Dark : ScreenshotTheme::Light;
    host_.apply_theme(theme);
    host_.refresh();
    host_.run_after(
        settle_ms_,
        [this, scene, dark, theme]
        {
            if (!host_.save_png(
                    screenshot_filename(prefix_, scenes_[scene].name, theme)))
            {
                host_.finish(false);
                return;
            }
            if (dark && scenes_[scene].teardown)
                scenes_[scene].teardown();
            ++index_;
            step_();
        });
}

} // namespace tesseract::screenshot
```

`ui/shared/CMakeLists.txt` screenshot block becomes:

```cmake
if(TESSERACT_ENABLE_SCREENSHOT_MODE)
    list(APPEND _tk_sources
        app/ScreenshotDirector.h
        app/ScreenshotDirector.cpp
        app/ScreenshotFixture.h
        app/ScreenshotFixture.cpp)
endif()
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build/linux-debug --target tesseract_tests && ctest --test-dir build/linux-debug -R screenshot --output-on-failure`
Expected: 6 tests PASS.

- [ ] **Step 5: Commit (after user confirmation)**

```bash
git add ui/shared/app/ScreenshotDirector.h ui/shared/app/ScreenshotDirector.cpp \
        ui/shared/CMakeLists.txt tests/CMakeLists.txt \
        tests/cpp/test_screenshot_director.cpp tests/cpp/test_screenshot_fixture.cpp
git commit -m "feat(screenshots): add shared scene director for screenshot mode"
```

---

### Task 2: Richer fixture + assets

**Files:**
- Modify: `ui/shared/app/ScreenshotFixture.h`, `ui/shared/app/ScreenshotFixture.cpp`
- Modify: `tests/cpp/test_screenshot_fixture.cpp`
- Modify: `README.md` (asset attribution line ~243, only if the new image's source needs credit)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces (in `tesseract::screenshot`):
  ```cpp
  struct Fixture {
      std::string user_id, display_name, avatar_url;
      std::vector<tesseract::RoomInfo> rooms;
      std::string selected_room_id;
      std::vector<views::MessageRowData> messages;
      std::string thread_root_id;                         // "$thread"
      std::vector<views::MessageRowData> thread_messages; // root first, then replies
      std::vector<tesseract::RoomMember> members;
      std::vector<std::pair<std::string, tesseract::PresenceState>> presence;
      std::vector<std::string> typing_names;              // {"Maya"}
  };
  Fixture make_fixture();
  bool install_assets(tk::CanvasFactory& factory, tk::PixmapCache& cache);
  std::vector<std::string> installed_asset_keys();       // for tests
  ```
  `install_avatar_assets` is removed (renamed); Task 5–8 call sites switch to `install_assets`.

- [ ] **Step 1: Write the failing tests**

`tests/cpp/test_screenshot_fixture.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "app/ScreenshotFixture.h"
#include "tk/pixmap_cache.h"
#include "tk_test_surface.h"

#include <algorithm>
#include <set>
#include <string>

using tesseract::screenshot::installed_asset_keys;
using tesseract::screenshot::make_fixture;
using Kind = tesseract::views::MessageRowData::Kind;

namespace
{
bool installed(const std::set<std::string>& keys, const std::string& k)
{
    return k.empty() || keys.contains(k);
}
} // namespace

TEST_CASE("every referenced media key is installed", "[screenshot]")
{
    const auto f = make_fixture();
    const auto list = installed_asset_keys();
    const std::set<std::string> keys(list.begin(), list.end());

    CHECK(installed(keys, f.avatar_url));
    for (const auto& r : f.rooms)
    {
        INFO(r.name);
        CHECK(installed(keys, r.avatar_url));
        CHECK(installed(keys, r.last_message_thumbnail_url));
    }
    auto check_rows = [&](const auto& rows)
    {
        for (const auto& m : rows)
        {
            INFO(m.event_id);
            CHECK(installed(keys, m.sender_avatar_url));
            if (m.kind == Kind::Image)
                CHECK(installed(keys, tesseract::to_string(m.source)));
        }
    };
    check_rows(f.messages);
    check_rows(f.thread_messages);
    for (const auto& mem : f.members)
        CHECK(installed(keys, mem.avatar_url));
}

TEST_CASE("install_assets decodes every asset", "[screenshot]")
{
    auto surface = TestSurface::create(16, 16);
    tk::PixmapCache cache;
    REQUIRE(tesseract::screenshot::install_assets(surface->factory(), cache));
    for (const auto& k : installed_asset_keys())
    {
        INFO(k);
        CHECK(cache.peek(tk::CacheKey::media(k)) != nullptr);
    }
}

TEST_CASE("fixture is internally consistent", "[screenshot]")
{
    const auto f = make_fixture();

    const auto root = std::find_if(
        f.messages.begin(), f.messages.end(),
        [&](const auto& m) { return m.event_id == f.thread_root_id; });
    REQUIRE(root != f.messages.end());
    CHECK(root->is_thread_root);
    REQUIRE_FALSE(f.thread_messages.empty());
    CHECK(f.thread_messages.front().event_id == f.thread_root_id);
    CHECK(root->thread_reply_count == f.thread_messages.size() - 1);

    for (const auto& m : f.messages)
    {
        if (!m.has_reply())
            continue;
        INFO(m.event_id);
        CHECK(std::any_of(f.messages.begin(), f.messages.end(),
                          [&](const auto& o)
                          { return o.event_id == m.in_reply_to_id; }));
    }

    CHECK(std::any_of(f.rooms.begin(), f.rooms.end(),
                      [&](const auto& r) { return r.id == f.selected_room_id; }));
    CHECK(f.rooms.size() == 10);
    CHECK(f.members.size() >= 6);
    CHECK_FALSE(f.typing_names.empty());
}
```

Before writing: confirm the exact names used above — `tk::PixmapCache` default-constructibility and `peek()` (`ui/shared/tk/pixmap_cache.h`), and how a `MediaSourceRef` turns into the provider string (grep `MediaSourceRef` in `client/include/tesseract/types.h` and RoomPane's image provider at `ui/shared/app/RoomPane.cpp:402`). If the conversion isn't `tesseract::to_string`, use whatever `make_row_data()` / the image provider uses and adjust the test line **and** the fixture's source construction to match. If `PixmapCache` needs constructor args, mirror an existing test's construction (`git grep -n "PixmapCache " tests/cpp`).

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build/linux-debug --target tesseract_tests 2>&1 | tail -5`
Expected: FAIL — `installed_asset_keys`, `install_assets`, `thread_messages` not declared.

- [ ] **Step 3: Generate the two new assets**

In the session scratchpad (not the repo), generate deterministic PNGs with Python + Pillow (`python -c "import PIL"`; if missing, `pip install --user pillow` after asking). Script:

```python
# gen_assets.py — deterministic fixture art, CC0 (own work)
from PIL import Image, ImageDraw, ImageFont
import base64, io

def banner():
    w, h = 600, 340
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):  # vertical indigo→violet gradient
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=(int(49 + 70*t), int(46 + 10*t), int(129 + 60*t)))
    for i, (cx, cy, r) in enumerate([(470, 90, 120), (520, 280, 80), (90, 300, 60)]):
        d.ellipse([cx-r, cy-r, cx+r, cy+r], outline=(255, 255, 255, 60), width=6)
    d.rectangle([40, 120, 300, 128], fill=(255, 255, 255))
    d.rectangle([40, 150, 230, 156], fill=(200, 200, 255))
    d.rectangle([40, 172, 260, 178], fill=(200, 200, 255))
    return im

def chip():
    im = Image.new("RGB", (96, 96), (255, 196, 87))
    d = ImageDraw.Draw(im)
    d.ellipse([18, 18, 78, 78], fill=(240, 120, 60))
    return im

for name, im in [("release_banner", banner()), ("sticker_chip", chip())]:
    buf = io.BytesIO()
    im.save(buf, "PNG", optimize=True)
    raw = buf.getvalue()
    print(name, len(raw))
    open(f"{name}.b64", "w").write(base64.b64encode(raw).decode())
```

No text is drawn (font rendering differs across machines → non-deterministic and untranslatable). Record each printed byte length; they go in the `Asset` table.

- [ ] **Step 4: Implement the fixture**

In `ScreenshotFixture.h`: replace the `Fixture` struct with the one in **Interfaces** above (add `#include <utility>`), rename `install_avatar_assets` → `install_assets`, declare `installed_asset_keys()`.

In `ScreenshotFixture.cpp`:

1. New keys next to the existing ones:
   ```cpp
   constexpr char kReleaseBanner[] = "fixture://media/release-banner";
   constexpr char kStickerChip[]  = "fixture://media/sticker-chip";
   ```
   Do **not** add new people with new avatar art; extra members/DMs reuse the four existing people (Alex, Maya, Sam, Robin) plus two members with an empty `avatar_url` (exercises the initials fallback).
2. Embed `kReleaseBannerPng[]` / `kStickerChipPng[]` as `R"b64(...)b64"` like the avatars, and add them to the `assets` array with their byte counts.
3. `installed_asset_keys()` returns the `key` of every entry in that same array — move the array to a file-scope `constexpr std::array kAssets` so both functions share it.
4. Rooms — keep the 4 existing, add (all `last_activity_ts` strictly decreasing so ordering is stable):
   - `!launch:tesseract.test` "Launch Planning" — `highlight_count = 1`, `notification_count = 3`, `unread_count = 3`, last "Alex, can you check the release notes?" from Maya.
   - `!space:tesseract.test` "Tesseract Project" — `is_space = true`, avatar `kTesseractRoomAvatar`.
   - `!standup:tesseract.test` "Daily Standup" — `is_call_room = true`, `has_active_call = true`, `call_members = {"@sam:tesseract.test", "@robin:tesseract.test"}`, `call_intent = "video"`.
   - `!sam:tesseract.test` "Sam Rivera" — `is_direct`, `dm_counterpart_user_id = "@sam:tesseract.test"`, avatar `kSamAvatar`, `last_message_kind = "sticker"`, `last_message_sticker_url = last_message_thumbnail_url = kStickerChip`.
   - `!offtopic:tesseract.test` "Off-topic" — `muted = true`, `unread_count = 5`.
   - `!archive:tesseract.test` "Old Announcements" — `is_low_priority = true`.
5. Timeline for `!tesseract:tesseract.test` (replace the current chatter; keep the `text()` helper and add small helpers `membership()`, `image()`, `voice()`, `file()` in the anonymous namespace following `text()`'s style). Times in minutes after `kBaseTime`:
   - `$day` DaySeparator (0)
   - `$join` Membership: sender/target Sam, `membership_action = Joined` (1)
   - `$plan` Maya text (3): body `"Release checklist for 0.10:\n- Final pass on the thread panel\n- Bump matrix-sdk\n- Refresh screenshots"`, `formatted_body` `"<p>Release checklist for <code>0.10</code>:</p><ul><li>Final pass on the thread panel</li><li>Bump <code>matrix-sdk</code></li><li>Refresh screenshots</li></ul>"`
   - `$banner` Maya Image (6): `source = kReleaseBanner` (constructed the same way the test reads it back), `media_w = 600`, `media_h = 340`, `has_filename_caption = true`, body `"New banner for the release post"`
   - `$love` own text reply to `$banner` (8): body `"Love it — ship it!"`, `in_reply_to_*` filled (sender "Maya", body "New banner for the release post", `in_reply_to_image_source = kReleaseBanner` source); reactions 🎉 (3, by me) and 👍 (2, not me)
   - `$code` Sam text (14): body is the snippet; `formatted_body` `"<p>Fixed the scroll anchor:</p><pre><code>if (anchor.visible())\n    list.scroll_to(anchor.index(), Align::Bottom);</code></pre>"`
   - `$voice` Robin Voice (19): `duration_ms = 14'000`, `audio_mime = "audio/ogg"`, `waveform` = 48 values from a fixed formula `static_cast<uint16_t>(200 + (i * 37) % 700)`
   - `$thread` Robin text thread root (24): `"Should we turn on threads by default for new rooms?"`, `is_thread_root = true`, `thread_reply_count = 4`, latest Maya / `"Yes — the panel is solid now."` / ts 40
   - `$notes` own File (31): `file_name = "release-notes.pdf"`, `file_size = 248'000`, body same
   - `$final` Maya text (42): `"Screenshots look great in dark mode too."`, `is_edited = true`, read receipts Sam + Robin
6. `thread_root_id = "$thread"`; `thread_messages` = a copy of the `$thread` row (as root, first) followed by 4 replies with `thread_root_id = "$thread"`: Maya (26) "I think so — fewer side conversations in the main timeline.", Sam (30) "👍" (emoji-only), own (34) "Agreed. I'll update the default.", Maya (40) "Yes — the panel is solid now." with a ❤️ reaction (1, not me).
7. `members`: Alex (100), Maya (100), Sam (50), Robin (0), `{"@casey:tesseract.test", "Casey", "", 0}`, `{"@drew:tesseract.test", "Drew", "", 0}`. Avatars from `avatar_for_sender`.
8. `presence`: Maya Online, Sam Online, Robin Unavailable, Casey Offline, Drew Offline, Alex Online.
9. `typing_names = {"Maya"}`.

- [ ] **Step 5: Run tests**

Run: `cmake --build build/linux-debug --target tesseract_tests && ctest --test-dir build/linux-debug -R screenshot --output-on-failure`
Expected: all `[screenshot]` tests PASS.

- [ ] **Step 6: Commit (after user confirmation)**

```bash
git add ui/shared/app/ScreenshotFixture.h ui/shared/app/ScreenshotFixture.cpp \
        tests/cpp/test_screenshot_fixture.cpp
git commit -m "feat(screenshots): richer fixture — more rooms, media rows, thread, members"
```

---

### Task 3: Shared view hooks (picker close, Appearance deep-link)

**Files:**
- Modify: `ui/shared/views/RoomView.h` (public section near `show_emoji_picker()` at ~685), `ui/shared/views/RoomView.cpp`
- Modify: `ui/shared/views/SettingsView.h` (next to `show_account_section()` ~320), `ui/shared/views/SettingsView.cpp`
- Test: `tests/cpp/test_screenshot_view_hooks.cpp` (new; add to `tests/CMakeLists.txt` main source list — unconditional, these are not screenshot-gated)

**Interfaces:**
- Produces: `void RoomView::close_pickers();` — public wrapper over private `hide_pickers_()`. `void SettingsView::show_appearance_section();` — selects the Appearance tab (index 3 per `SettingsView.cpp:303-312`).
- Existing, reused by Task 4: `void RoomView::show_emoji_picker();` (public, anchors near compose bar), `bool EmojiPicker::visible()`.

- [ ] **Step 1: Write the failing tests**

Look at `tests/cpp/test_tk_room_view_copy_shortcut.cpp` for how a `RoomView` is constructed in tests and mirror its setup verbatim (host, theme, bounds). Then:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "views/RoomView.h"
#include "views/SettingsView.h"
#include "views/EmojiPicker.h"
#include "tk/side_tab_view.h"
// + whatever test_tk_room_view_copy_shortcut.cpp includes for its harness

TEST_CASE("close_pickers hides an open emoji picker", "[screenshot][room_view]")
{
    // <RoomView setup copied from test_tk_room_view_copy_shortcut.cpp>
    rv.show_emoji_picker();
    REQUIRE(rv.emoji_picker()->visible());
    rv.close_pickers();
    CHECK_FALSE(rv.emoji_picker()->visible());
}

TEST_CASE("show_appearance_section selects the Appearance tab",
          "[screenshot][settings]")
{
    tesseract::views::SettingsView sv;   // match how test_settings_group_accessibility.cpp builds one
    sv.show_appearance_section();
    CHECK(sv.selected_tab_title() == "Appearance");
}
```

`selected_tab_title()` doesn't exist. Check `tk::SideTabView` (`ui/shared/tk/side_tab_view.h`) for a selected-index getter and title accessor and assert through what exists (e.g. `sv.tabs()->selected() == 3` if `SettingsView` exposes its tab view; if not, add a `int selected_tab_index() const` on `SettingsView` returning `tabs_->selected()` and assert `== 3`). Don't hard-code `3` in `show_appearance_section()` without a named constant next to `kAdvancedTabIdx` (`static constexpr int kAppearanceTabIdx = 3;`) and a comment pointing at the `add_tab` order.

- [ ] **Step 2: Run to verify failure** — build fails on `close_pickers` / `show_appearance_section`.

- [ ] **Step 3: Implement**

`RoomView.h`, public, right after `show_emoji_picker();`:

```cpp
    // Close the emoji/sticker picker if open. Public counterpart of the
    // compose-bar toggle, used by screenshot mode to tear its scene down.
    void close_pickers();
```

`RoomView.cpp`:

```cpp
void RoomView::close_pickers()
{
    hide_pickers_();
}
```

`SettingsView.h`, after `show_account_section();`:

```cpp
    // Select the Appearance tab. Used by screenshot mode's settings scene.
    void show_appearance_section();
```

`SettingsView.cpp`, modelled on `show_account_section()`'s body (read it first and mirror it, substituting `kAppearanceTabIdx`).

- [ ] **Step 4: Run tests** — `ctest --test-dir build/linux-debug -R "close_pickers|show_appearance" --output-on-failure` → PASS.

- [ ] **Step 5: Commit (after user confirmation)**

```bash
git add ui/shared/views/RoomView.h ui/shared/views/RoomView.cpp \
        ui/shared/views/SettingsView.h ui/shared/views/SettingsView.cpp \
        tests/cpp/test_screenshot_view_hooks.cpp tests/CMakeLists.txt
git commit -m "feat(views): add RoomView::close_pickers and SettingsView Appearance deep-link"
```

---

### Task 4: ShellBase seeding + scene table

**Files:**
- Modify: `ui/shared/app/ShellBase.h`, `ui/shared/app/ShellBase.cpp`

**Interfaces:**
- Consumes: Task 1 (`Scene`, `ScreenshotHost`, `ScreenshotDirector`), Task 2 (`Fixture`, `install_assets`), Task 3 (`close_pickers`, `show_appearance_section`).
- Produces (all `protected`, inside `#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED`):
  ```cpp
  // Seeds shell state + shared views from the fixture. Returns false when
  // assets fail to decode (caller exits non-zero).
  bool seed_screenshot_fixture_(tk::CanvasFactory& factory);
  // Builds the scene table and starts the director. `host` must outlive it.
  void start_screenshot_director_(screenshot::ScreenshotHost& host,
                                  std::string prefix, int settle_ms = 300);
  ```
  New virtual (always compiled, default no-op, next to `open_app_settings_ui_()` at `ShellBase.h:4100`):
  ```cpp
  // Return from the settings surface to the main content. Counterpart of
  // open_app_settings_ui_(); shells implement it with their existing
  // "show main content" path.
  virtual void close_app_settings_ui_() {}
  ```

- [ ] **Step 1: Add members/declarations**

In `ShellBase.h`, under the macro, include `app/ScreenshotDirector.h` and `app/ScreenshotFixture.h`, and add private-ish protected state:

```cpp
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    screenshot::Fixture screenshot_fixture_;
    std::unique_ptr<screenshot::ScreenshotDirector> screenshot_director_;
    std::vector<screenshot::Scene> make_screenshot_scenes_();
#endif
```

- [ ] **Step 2: Implement `seed_screenshot_fixture_`**

```cpp
bool ShellBase::seed_screenshot_fixture_(tk::CanvasFactory& factory)
{
    if (!screenshot::install_assets(factory, account_manager_.thumbnail_cache()))
        return false;

    screenshot_fixture_ = screenshot::make_fixture();
    auto& f = screenshot_fixture_;
    my_user_id_      = f.user_id;
    my_display_name_ = f.display_name;
    my_avatar_url_   = f.avatar_url;
    rooms_           = f.rooms;
    current_room_id_ = f.selected_room_id;

    main_app_->show_room();
    main_app_->room_list_view()->set_rooms(rooms_);
    main_app_->room_list_view()->set_selected_room(current_room_id_);
    for (const auto& room : rooms_)
        if (room.id == current_room_id_)
        {
            room_view_->set_room(room);
            break;
        }
    room_view_->set_messages(f.messages);
    room_view_->set_typing_text(format_typing_text(f.typing_names));
    return true;
}
```

`format_typing_text` is a file-static in `ShellBase.cpp` (~9662) — the function must be defined below it or forward-declared. Check `room_view_->set_messages` signature (it may take a `room_switch` bool like `ThreadView::set_messages`) and match it. GTK previously called `show_rooms(rooms_)` instead of `room_list_view()->set_rooms` — check what GTK's `show_rooms` adds (e.g. space child counts) and, if it's more than `set_rooms`, keep that extra in GTK's adapter (Task 6) after seeding.

- [ ] **Step 3: Implement `make_screenshot_scenes_`**

```cpp
std::vector<screenshot::Scene> ShellBase::make_screenshot_scenes_()
{
    using views::RoomView;
    const auto& f = screenshot_fixture_;
    std::vector<screenshot::Scene> s;
    s.push_back({"main", {}, {}});
    s.push_back({"thread",
                 [this, &f]
                 {
                     room_view_->set_thread_panel(RoomView::ThreadPanelState::Open,
                                                  f.thread_root_id);
                     room_view_->thread_view()->set_messages(f.thread_messages,
                                                             /*room_switch=*/true);
                 },
                 [this]
                 { room_view_->set_thread_panel(RoomView::ThreadPanelState::Closed, {}); }});
    s.push_back({"room-info",
                 [this, &f]
                 {
                     room_view_->show_room_info();
                     auto* p = room_view_->room_info_panel();
                     p->set_presence_provider(
                         [&f](const std::string& uid)
                         {
                             for (const auto& [id, st] : f.presence)
                                 if (id == uid)
                                     return st;
                             return tesseract::PresenceState::Offline;
                         });
                     p->set_members(f.members);
                 },
                 [this] { room_view_->room_info_panel()->close(); }});
    s.push_back({"emoji",
                 [this] { room_view_->show_emoji_picker(); },
                 [this] { room_view_->close_pickers(); }});
    s.push_back({"settings",
                 [this]
                 {
                     open_app_settings_ui_();
                     if (stats_settings_view_)
                         stats_settings_view_->show_appearance_section();
                 },
                 [this] { close_app_settings_ui_(); }});
    return s;
}
```

Verify against real code before relying on it:
- Read `ThreadPanelController` (`ui/shared/app/ThreadPanelController.cpp`) to see whether the live path feeds the root as the first thread row and whether `set_thread_panel(Open, …)` needs anything else (header root text, `thread_view()->set_root(...)`). Match it.
- `room_info_panel()->set_members` must come *after* `show_room_info()` (`open()` may reset members) — confirm in `RoomInfoPanel::open`.
- Each setup should end with a relayout; the director calls `host.refresh()` right after setup, so no relayout calls are needed here.

- [ ] **Step 4: Implement `start_screenshot_director_`**

```cpp
void ShellBase::start_screenshot_director_(screenshot::ScreenshotHost& host,
                                           std::string prefix, int settle_ms)
{
    screenshot_director_ = std::make_unique<screenshot::ScreenshotDirector>(
        host, make_screenshot_scenes_(), std::move(prefix), settle_ms);
    screenshot_director_->start();
}
```

- [ ] **Step 5: Remove macOS's private seeding**

Delete `MacShell::seed_screenshot_fixture` (`ui/macos/src/MainWindowController.mm:392-411`); Task 8 switches its caller.

- [ ] **Step 6: Null-client audit for the settings scene**

Read each shell's settings-open path (`openSettings` Qt `MainWindow.cpp:~4170`, GTK `open_settings_`, Win32 `open_settings_`, macOS `_openSettings`/`_ensureSettingsView`) and `bind_settings_controller_()` / `wire_settings_view_()`. List every `client_->` / session dereference reached without a null check. For each, add the guard the surrounding code already uses (`if (!client_) return;` or `sess && sess->client`). Note each site in the commit message. Do not add fake-client plumbing.

- [ ] **Step 7: Build** — `cmake --build build/linux-debug --target tesseract_qt tesseract_tests 2>&1 | tail -20` (the Qt target will fail until Task 5 switches it off `install_avatar_assets`; do Task 5 Step 1–2 before declaring this task's build green, or temporarily build only `tesseract_tests`). Note the dev preset may have screenshot mode OFF; configure a second build dir for screenshot-mode compiles:

```bash
cmake --preset linux-debug -B build/linux-debug-shot -DTESSERACT_ENABLE_SCREENSHOT_MODE=ON -DTESSERACT_UI=qt6
cmake --build build/linux-debug-shot --target tesseract_qt
```

(`-DTESSERACT_UI=qt6` keeps GTK out of this build, per the user's rule.)

- [ ] **Step 8: Commit together with Task 5 (after user confirmation)** — ShellBase alone doesn't link into a working shell.

---

### Task 5: Qt6 shell adapter

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h`, `ui/linux-qt/src/MainWindow.cpp:1664-1745`

**Interfaces:**
- Consumes: Task 4 `seed_screenshot_fixture_`, `start_screenshot_director_`, `close_app_settings_ui_`.

- [ ] **Step 1: Adapter + new captureScreenshots**

In `MainWindow.h`, under the macro: `std::unique_ptr<tesseract::screenshot::ScreenshotHost> screenshot_host_;` and `void close_app_settings_ui_() override { showMainContent_(); }` (always compiled, next to `open_app_settings_ui_`). Confirm `showMainContent_()` hides `settingsWidget_` (read it); if not, use whatever the settings "Close" signal handler at `MainWindow.cpp:4183` calls.

Replace the body of `MainWindow::captureScreenshots`:

```cpp
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
namespace
{
class QtScreenshotHost final : public tesseract::screenshot::ScreenshotHost
{
public:
    QtScreenshotHost(MainWindow& w, QString dir) : w_(w), dir_(std::move(dir)) {}
    void apply_theme(tesseract::screenshot::ScreenshotTheme t) override
    {
        w_.apply_screenshot_theme_(t == tesseract::screenshot::ScreenshotTheme::Dark);
    }
    void refresh() override { w_.refresh_for_screenshot_(); }
    bool save_png(const std::string& name) override
    {
        const QString path = QDir(dir_).filePath(QString::fromStdString(name));
        if (w_.grab().save(path, "PNG"))
            return true;
        qCritical("Could not save screenshot: %s", qPrintable(path));
        return false;
    }
    void run_after(int ms, std::function<void()> fn) override
    {
        QTimer::singleShot(ms, &w_, std::move(fn));
    }
    void finish(bool ok) override { qApp->exit(ok ? 0 : 1); }

private:
    MainWindow& w_;
    QString dir_;
};
} // namespace

void MainWindow::apply_screenshot_theme_(bool dark)
{
    apply_theme_ui_(dark ? tk::Theme::dark() : tk::Theme::light());
}

void MainWindow::refresh_for_screenshot_()
{
    mainAppSurface_->relayout();
    mainAppSurface_->update();
    if (settingsWidget_ && settingsWidget_->isVisible())
        settingsWidget_->update();
    repaint();
}

void MainWindow::captureScreenshots(const std::string& output_dir)
{
    if (!seed_screenshot_fixture_(mainAppSurface_->factory()))
    {
        qCritical("Could not load screenshot assets");
        qApp->exit(1);
        return;
    }
    populateUserStrip();
    statusBar()->showMessage(QString::fromStdString(tk::tr("Connected")));
    showMainContent_();
    resize(1100, 768);

    const QString dir = QString::fromStdString(output_dir);
    if (!QDir().mkpath(dir))
    {
        qCritical("Could not create screenshot directory: %s", qPrintable(dir));
        qApp->exit(1);
        return;
    }
    screenshot_host_ = std::make_unique<QtScreenshotHost>(*this, dir);
    start_screenshot_director_(*screenshot_host_, "qt6");
}
#endif
```

Declare `apply_screenshot_theme_(bool)` / `refresh_for_screenshot_()` as public-under-macro members in `MainWindow.h` (the adapter is outside the class). Check whether the settings surface repaints on `apply_theme_ui_`; if the dark settings shot shows a stale light settings surface, call the existing settings theme setter (`settingsWidget_->set_theme(current_theme_)`, seen at `MainWindow.cpp:4176`) inside `apply_screenshot_theme_`.

- [ ] **Step 2: Build screenshot-mode Qt** — `cmake --build build/linux-debug-shot --target tesseract_qt 2>&1 | tail -20` → builds.

- [ ] **Step 3: Full normal build + ctest** — `cmake --build build/linux-debug --target tesseract_qt tesseract_tests && ctest --test-dir build/linux-debug --output-on-failure 2>&1 | tail -15`. If anything fails, report the names to the user and stop (CLAUDE.md: don't revert-and-compare unprompted).

- [ ] **Step 4: Ask the user to run the Qt capture** and look at the 10 PNGs:

```bash
TZ=UTC LANG=C.UTF-8 QT_SCALE_FACTOR=1 \
  XDG_CONFIG_HOME=$(mktemp -d) XDG_DATA_HOME=$(mktemp -d) XDG_CACHE_HOME=$(mktemp -d) \
  ./build/linux-debug-shot/ui/linux-qt/tesseract --screenshot-dir=/tmp/tess-shots
ls /tmp/tess-shots
```

Expected: 10 files, exit status 0. Things to check visually: thread panel populated, members list with presence dots, emoji picker over the composer, settings on Appearance, dark shots fully dark (incl. settings). Fix and iterate based on the user's report.

- [ ] **Step 5: Commit Tasks 4+5 (after user confirmation)**

```bash
git add ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp \
        ui/linux-qt/src/MainWindow.h ui/linux-qt/src/MainWindow.cpp
git commit -m "feat(screenshots): drive Qt6 capture through shared director with 5 scenes"
```

---

### Task 6: GTK4 shell adapter

**Files:** `ui/linux-gtk/src/MainWindow.h`, `ui/linux-gtk/src/MainWindow.cpp:2328-2420`

- [ ] **Step 1: Implement**

`close_app_settings_ui_() override { show_main_content_(); }` next to `open_app_settings_ui_` in `MainWindow.h` (confirm `show_main_content_` hides the settings widget). Adapter in the `.cpp` under the macro:

```cpp
namespace
{
class GtkScreenshotHost final : public tesseract::screenshot::ScreenshotHost
{
public:
    explicit GtkScreenshotHost(MainWindow& w) : w_(w) {}
    void apply_theme(tesseract::screenshot::ScreenshotTheme t) override
    {
        w_.apply_screenshot_theme_(t == tesseract::screenshot::ScreenshotTheme::Dark);
    }
    void refresh() override { w_.refresh_for_screenshot_(); }
    bool save_png(const std::string& name) override
    {
        return w_.save_screenshot_(name.c_str());
    }
    void run_after(int ms, std::function<void()> fn) override
    {
        auto* boxed = new std::function<void()>(std::move(fn));
        g_timeout_add(
            ms,
            +[](gpointer data) -> gboolean
            {
                std::unique_ptr<std::function<void()>> f(
                    static_cast<std::function<void()>*>(data));
                (*f)();
                return G_SOURCE_REMOVE;
            },
            boxed);
    }
    void finish(bool ok) override { w_.finish_screenshots_(ok); }

private:
    MainWindow& w_;
};
} // namespace
```

`finish_screenshots_(bool ok)`: store `ok` in a member the `main.cpp` exit path returns (check how GTK's `main.cpp` derives the exit status from `g_application_run`; if it can't be influenced, call `g_application_quit` and, on failure, `std::exit(1)` after it — mirror what the old code did on failure, which was `g_application_quit` only, and improve to a non-zero exit). `start_screenshot_mode()` keeps the `screenshot_dir_` mkdir, window sizing, `gtk_window_present`, then `seed_screenshot_fixture_(main_app_surface_->factory())`, `populate_user_strip()`, status label, `show_main_content_()`, plus anything extra `show_rooms()` did (see Task 4 Step 2), then `start_screenshot_director_(*screenshot_host_, "gtk4")`. Delete the nested `g_timeout_add` chain. `refresh_for_screenshot_()` = `main_app_surface_->relayout();` plus the settings surface's relayout/queue_draw if visible.

- [ ] **Step 2: Hand the GTK build to the user** — do not build the GTK target yourself. Ask the user to build `tesseract_gtk` in a screenshot-mode dir and run the capture (same command as Task 5 Step 4 with `dbus-run-session` and the GTK binary).

- [ ] **Step 3: Commit (after user confirmation)** — `feat(screenshots): GTK4 capture via shared director`.

---

### Task 7: Win32 shell adapter

**Files:** `ui/windows/src/MainWindow.h:516-517`, `ui/windows/src/MainWindow.cpp:1614-1640, 3755-3810`

- [ ] **Step 1: Implement**

- Replace `kScreenshotLightTimerId` / `kScreenshotDarkTimerId` with one `static constexpr UINT_PTR kScreenshotStepTimerId = 9;` and a member `std::function<void()> screenshot_step_;`.
- `WM_TIMER` branch:
  ```cpp
  if (wParam == kScreenshotStepTimerId)
  {
      KillTimer(hwnd, kScreenshotStepTimerId);
      if (auto fn = std::exchange(screenshot_step_, nullptr))
          fn();
      return 0;
  }
  ```
- Adapter (same shape as Qt/GTK): `run_after` stores `fn` into `screenshot_step_` and `SetTimer(hwnd_, kScreenshotStepTimerId, ms, nullptr)` (safe — the director never has two pending; Review Focus 5). `save_png` → existing `save_screenshot_(utf8_to_wstr(name).c_str())`. `apply_theme` → `apply_theme_ui_(…)`. `refresh` → `RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW)` after the main surface relayout. `finish(ok)` → record exit code (check how `main.cpp`'s message loop returns; `PostQuitMessage(ok ? 0 : 1)` if the window destroy path doesn't already post one, else set a member read by `WM_DESTROY`) then `DestroyWindow(hwnd_)`.
- `close_app_settings_ui_() override { close_settings_(); }`.
- `start_screenshot_mode_()`: seed, user strip, `show_main_content()`, status text, `SetWindowPos` 1100×768, mkdir, then `start_screenshot_director_(*screenshot_host_, "win", 500)`.

- [ ] **Step 2: Tell the user plainly** this cannot be built here (no Windows toolchain); it's verified by the `windows` job of `update-screenshots.yml` or their Windows box.

- [ ] **Step 3: Commit (after user confirmation)** — `feat(screenshots): Win32 capture via shared director (unverified build)`; per CLAUDE.md the user decides whether to commit unverified platform code.

---

### Task 8: macOS shell adapter

**Files:** `ui/macos/src/MainWindowController.mm:226, 1973, 2999-3066`

- [ ] **Step 1: Implement**

- `MacShell`: add `void close_app_settings_ui_() override;` (next to `open_app_settings_ui_` at :226) → `[ctrl_ _showMainContent]`. Expose `seed_screenshot_fixture_` and `start_screenshot_director_` with `using ShellBase::…;` in `MacShell`'s existing public `using` section (that's how ObjC++ reaches protected members, per CLAUDE.md).
- Adapter class in the `.mm` holding a `__weak MainWindowController*`:
  - `apply_theme` → `[c _applyTheme:…]`
  - `refresh` → `c->_mainAppSurface->relayout();` plus `[c _repaintSettingsSurfaceIfVisible]`
  - `save_png` → `[c _saveScreenshotToPath:[directory stringByAppendingPathComponent:@(name.c_str())]]`
  - `run_after` → `dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ fn(); })` — capture `fn` in a `__block std::function<void()>` copy.
  - `finish(ok)` → `ok ? [NSApp terminate:nil] : std::exit(EXIT_FAILURE)`.
- `captureScreenshotsToDirectory:` keeps mkdir, branding/login/settings surface hiding, window size, `makeKeyAndOrderFront`; replaces `install_avatar_assets` + `seed_screenshot_fixture` with `_shell->seed_screenshot_fixture_(_mainAppSurface->factory())`; then `_populateUserStrip`, status label, and `_shell->start_screenshot_director_(*_screenshotHost, "mac")`. Store the adapter in an ivar `std::unique_ptr<…> _screenshotHost`. Delete the nested `dispatch_after` chain.

- [ ] **Step 2: Tell the user plainly** this cannot be built here; verified by the `appkit` CI job or their Mac.

- [ ] **Step 3: Commit (after user confirmation)** — `feat(screenshots): AppKit capture via shared director (unverified build)`.

---

### Task 9: CI, README, changelog

**Files:** `.github/workflows/update-screenshots.yml`, `README.md:27-32`, `CHANGES.md`

- [ ] **Step 1: Workflow**

Linux step: `timeout 30s` → `timeout 60s` (both runs). Replace the four `test -s` lines with:

```bash
for prefix in qt6 gtk4; do
  for theme in light dark; do
    test -s "generated-screenshots/$prefix-$theme.png"
    for scene in thread room-info emoji settings; do
      test -s "generated-screenshots/$prefix-$scene-$theme.png"
    done
  done
done
```

macOS step: `gtimeout 30s` → `gtimeout 60s`; same loop with `prefix=mac`.

Windows step: `Wait-Process -Timeout 30` → `60`; replace the `foreach` list with:

```powershell
$names = foreach ($theme in 'light', 'dark') {
  "win-$theme.png"
  foreach ($scene in 'thread', 'room-info', 'emoji', 'settings') { "win-$scene-$theme.png" }
}
foreach ($name in $names) { ...existing body... }
```

- [ ] **Step 2: README Feature tour** — after the Screenshots table (line ~32):

```markdown
### Feature tour

| | |
|---|---|
| ![Threads](screenshots/qt6-thread-light.png) | ![Room info and members](screenshots/qt6-room-info-light.png) |
| **Threads** — reply in a side panel without leaving the conversation | **Room info** — members, roles and presence at a glance |
| ![Emoji picker](screenshots/qt6-emoji-light.png) | ![Settings](screenshots/qt6-settings-light.png) |
| **Emoji picker** — search, categories and skin tones | **Settings** — themes, accent colours and message layouts |
```

The four PNGs come from the user's first successful CI run (artifact `tesseract-linux-screenshots`) — copy them into `screenshots/` together with the refreshed main shots. Do not generate them locally.

- [ ] **Step 3: CHANGES.md** — one bullet under the current unreleased section, terse per CLAUDE.md:

```markdown
- ci(screenshots): screenshot mode now captures thread, room-info, emoji-picker and settings scenes alongside a richer main view, driven by a shared `ScreenshotDirector`; README gains a Feature tour. Qt6 build + ctest <N>/<N>
```

Fill `<N>` from the actual ctest run; list only platforms actually built/verified (no "unbuilt" notes).

- [ ] **Step 4: STATUS.md** — refresh test count / Last updated if the change warrants it (memory: keep STATUS.md current after major features).

- [ ] **Step 5: Commit (after user confirmation)** — `ci(screenshots): check all scenes; README feature tour`.

---

## Final gate

Run a whole-branch code review (user expects one before merge): `/code-review` at the user's chosen level, then address findings.
