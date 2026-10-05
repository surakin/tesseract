# Screenshot showcase — design

Date: 2026-10-02
Status: approved; updated after plan research (see plan)

## Goal

The CI screenshot fixture (`ui/shared/app/ScreenshotFixture.*`) currently
captures one main-view shot per platform × theme, showing four plain rooms and
five mostly-plain text messages. Make the screenshots showcase more of the app:

1. **Richer main shot** — pack more features into the existing main view.
2. **Extra scenes** — capture four additional scenes per platform × theme:
   thread panel, room info / members, emoji picker, settings.

The README keeps its 4-platform main-shot grid and gains a "Feature tour"
section showing each extra scene once (Qt6, light).

## Non-goals

- Location messages (need network OSM tiles), video (GStreamer thumbnail
  generation is non-deterministic), URL preview cards (timeline is full).
- New CLI flags: `--screenshot-dir` and the `TESSERACT_ENABLE_SCREENSHOT_MODE`
  build gate stay as they are.
- Running a per-scene process (rejected: 5× launches/Xvfb sessions).

## 1. Fixture content (main shot)

Window size stays 1100×768. The timeline is bottom-anchored (~8–9 rows
visible); older rows may scroll off the top.

### Room list (4 → 10 entries)

Existing: Tesseract (favorite, encrypted, selected), Design (2 unread),
Maya Chen (DM), Matrix Community (quiet unread). Added:

- a space (`is_space = true`)
- a room with a mention (`highlight_count > 0`) → red mention badge
- a muted room (`muted = true`)
- a low-priority room (`is_low_priority = true`)
- a call room with an active call (`is_call_room`, `has_active_call`,
  `call_members`)
- a second DM whose last message is an image/sticker
  (`last_message_kind`, `last_message_thumbnail_url`) → room-list thumbnail chip

### Timeline (top → bottom)

The conversation is rewritten as a believable project chat (no meta
"screenshot workflow" text).

1. `Membership` — "Sam joined the room".
2. `Text` with `formatted_body` — bulleted list + inline `code`.
3. `Image` — bundled ~600×340 PNG (release-banner style), with an MSC2530
   caption (`has_filename_caption`), served from `fixture://media/...`.
4. Own `Text` reply quoting the image (`in_reply_to_image_source`), with a
   🎉 and 👍 reaction row.
5. `Text` with a fenced code block (`<pre><code>`).
6. `Voice` — fixed `waveform` + `duration_ms` (renders from data, no fetch).
7. `Text` thread root (Robin) with the "4 replies" chip — `$thread`, reused by
   the thread scene.
8. `File` — `release-notes.pdf`, 248 KB.
9. `Text` — final edited message with read receipts.

Composer: typing indicator via `RoomView::set_typing_text` ("Maya is typing…",
built with `tk::trf` like the real path).

### Additional fixture data (for scenes)

- `thread_messages`: 4 replies to `$thread` (one emoji-only, one with a
  reaction), each with `thread_root_id = "$thread"`.
- `members`: ~6 `tesseract::RoomMember` entries with power levels
  (Admin / Moderator / default) and a fixed presence map (online / away /
  offline) served by a presence provider.
- No "Frequently used" emoji row: `EmojiPicker::refresh_frequents()` reads
  from the client, which screenshot mode doesn't have, so the emoji scene shows
  the normal category view. No fake-client plumbing is added for it.

Fixture message bodies are sample data, not UI strings — not wrapped in
`tk::tr`, matching the current fixture.

### Assets

New embedded base64 assets alongside the avatars, installed the same way
(`cache.store(tk::CacheKey::media(key), ...)`):

- timeline image (~30–60 KB PNG, CC0, generated deterministically)
- one small thumbnail/sticker for the second DM's room-list chip

`install_avatar_assets` is renamed `install_assets` (it now installs media,
not just avatars). README attribution is extended if the new asset has a
source other than this repo.

## 2. Scenes

Each scene is captured in light and dark. Filenames:
`<prefix>-<scene>-<theme>.png`, except `main`, which keeps today's names
(`<prefix>-<theme>.png`) so the README grid and existing links are unchanged.
Prefixes: `qt6`, `gtk4`, `win`, `mac`.

| Scene       | Setup                                                                                          | Teardown                          |
|-------------|------------------------------------------------------------------------------------------------|-----------------------------------|
| `main`      | none beyond seeding                                                                            | —                                 |
| `thread`    | `room_view->set_thread_panel(Open, "$thread")`; feed `thread_messages` to `thread_view()`      | `set_thread_panel(Closed, {})`    |
| `room-info` | `room_info_panel()->set_members(members)`, presence provider; `room_view->show_room_info()`   | hide room info panel              |
| `emoji`     | existing public `RoomView::show_emoji_picker()` (already anchors near the compose bar) | new public `RoomView::close_pickers()` |
| `settings`  | `ShellBase::open_app_settings_ui_()`; new `SettingsView::show_appearance_section()` | new virtual `ShellBase::close_app_settings_ui_()` |

New public API:

- `RoomView::close_pickers()` — public wrapper over private `hide_pickers_()`.
- `SettingsView::show_appearance_section()` — sibling of the existing
  `show_account_section()`, using a named `kAppearanceTabIdx`.
- `ShellBase::close_app_settings_ui_()` — virtual, default no-op; counterpart
  of `open_app_settings_ui_()`, implemented by each shell with its existing
  "show main content" path.

### Risk: settings without a client

Screenshot mode has no `tesseract::Client`. The shell's settings bind path
(`bind_settings_controller_()`, Sessions/Account sections) may call into the
client or `SettingsController` when the settings surface is first shown. Verify
on Qt6 first; if it crashes, guard the offending bind steps for screenshot mode
rather than adding fake client plumbing.

## 3. Architecture

### `ScreenshotDirector` (`ui/shared/app/ScreenshotDirector.{h,cpp}`)

Compiled only when `TESSERACT_SCREENSHOT_MODE_ENABLED`.

```cpp
namespace tesseract::screenshot
{
enum class ScreenshotTheme { Light, Dark };

struct ScreenshotHost
{
    virtual ~ScreenshotHost() = default;
    virtual void apply_theme(ScreenshotTheme theme) = 0;
    virtual void refresh() = 0;   // relayout + repaint after a change
    virtual bool save_png(const std::string& filename) = 0;
    virtual void run_after(int ms, std::function<void()> fn) = 0;
    virtual void finish(bool ok) = 0;
};

struct Scene
{
    std::string name;
    std::function<void()> setup;
    std::function<void()> teardown;
};

class ScreenshotDirector
{
public:
    ScreenshotDirector(ScreenshotHost& host, std::vector<Scene> scenes,
                       std::string prefix, int settle_ms = 300);
    void start();
};
}
```

Sequencing: for each scene → setup; for each theme (light, dark) →
`apply_theme`, `refresh`, `run_after(settle_ms)`, `save_png`; then teardown.
A failed `save_png` calls `finish(false)` immediately; after the last scene's
teardown, `finish(true)`. The director never has more than one `run_after`
pending, so a shell may back it with a single timer.

The director knows nothing about views: it takes the scene table as data.
`ShellBase::make_screenshot_scenes_()` builds the real table from shared views
(see §2), which keeps the director unit-testable with fake scenes.

### Fixture seeding and scenes move to `ShellBase`

macOS's `MacShell::seed_screenshot_fixture` becomes a protected
`ShellBase::seed_screenshot_fixture_(tk::CanvasFactory&)` used by all four
shells (it also installs the assets). `ShellBase::start_screenshot_director_(host,
prefix, settle_ms)` builds the scene table and starts the director. It
assigns `my_user_id_`, `my_display_name_`, `my_avatar_url_`, `rooms_`,
`current_room_id_` and seeds room list / room view / messages / typing text.
Shells keep only genuinely native steps: user strip, status bar text,
showing the main surface, window size, `ScreenshotHost` adapter.

### Per-shell adapter

Each shell keeps its existing native `save_screenshot_` (Qt `grab()`, GTK
paintable → texture, Win32 `PrintWindow` + GDI+, macOS
`_saveScreenshotToPath:`) and timer primitive (`QTimer::singleShot`,
`g_timeout_add`, `SetTimer` / `WM_TIMER`, `dispatch_after`), wrapped in a
small `ScreenshotHost` implementation, and implements
`close_app_settings_ui_()`. The existing nested light→dark timer chains are
deleted. Win32 passes `settle_ms = 500` (its current value).

Win32's `WM_TIMER`-id based chain needs a single generic timer id that
invokes a stored `std::function`; the plan details this.

## 4. CI workflow (`.github/workflows/update-screenshots.yml`)

- Capture timeouts 30 s → 60 s (10 captures × ~300–500 ms + startup).
- Replace the per-file `test -s` lines with a loop over
  `main thread room-info emoji settings` × `light dark` per platform
  (bash for Linux/macOS, PowerShell for Windows), mapping `main` to the legacy
  filename.
- Artifact upload globs unchanged (`generated-screenshots/*.png`).

## 5. README

- Main grid unchanged.
- New "Feature tour" section after "Screenshots" with four captioned images:
  `screenshots/qt6-thread-light.png`, `qt6-room-info-light.png`,
  `qt6-emoji-light.png`, `qt6-settings-light.png`.
- PNGs are checked in from the user's first CI run (Claude does not run the
  app to generate them).

## 6. Testing & verification

Catch2 (`tests/cpp/`). When screenshot mode is OFF (normal dev builds),
`tests/CMakeLists.txt` compiles `ScreenshotDirector.cpp` and
`ScreenshotFixture.cpp` straight into `tesseract_tests`; when ON they come from
`tesseract_tk` (avoids duplicate symbols). Either way the tests run in the
regular ctest.

- Director sequencing with a fake `ScreenshotHost` and fake scenes: exactly
  the 10 expected filenames in order, themes alternating light/dark, each
  scene's teardown runs before the next setup, `finish(true)` once at the end,
  `finish(false)` and no further saves after a failed save, never more than one
  pending `run_after`.
- View hooks: `close_pickers()` hides an open picker;
  `show_appearance_section()` selects the Appearance tab.
- Fixture consistency: every `sender_avatar_url`, room avatar, and media key
  referenced by the fixture is installed by `install_assets`; `$thread` exists
  and has `thread_reply_count == thread_messages.size() - 1` (root is first); every
  `in_reply_to_id` resolves.

Verification: Qt6 build + full ctest on Linux; the user runs the Qt6 capture
locally and the CI workflow for GTK4/Win32/macOS.

## 7. Docs

- CHANGES.md: one terse bullet.
- No new translatable strings expected ("Connected" and the typing template
  already exist); if any are added, update every `.po` under `i18n/`.
