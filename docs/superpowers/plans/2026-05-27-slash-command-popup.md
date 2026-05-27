# Slash-Command Popup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Typing `/` at the start of an empty composer pops up a filtered list of slash commands (initially `/me`, `/shrug`); arrow keys + Enter/Tab autocomplete; Escape dismisses.

**Architecture:** Mirror the existing `ShortcodePopup` infrastructure. A new stateless `SlashCommandEngine` exposes prefix matching + lookup. A new `SlashCommandPopup` widget renders rows (`/name <args> — description`). Each of the four shells (Qt6, GTK4, Win32, macOS) hosts the popup in its own native overlay surface — the same per-shell pattern already used for `ShortcodePopup`. The command registry already lives in `ui/shared/app/SlashCommands.{h,cpp}` from the `/me` slice; we extend it with a descriptor table and one new command (`/shrug`) so the popup demos with multiple items.

**Tech Stack:** C++17, `tk::Widget` for rendering, Catch2 for unit tests. New Rust SDK work is **not** required — `/shrug` is a pure text-substitution command routed through the existing `Client::send_message`.

**Out of scope (explicit non-goals):**
- Slash commands beyond `/me` (existing) and `/shrug` (new). The "Text + room-state" scope (`/topic`, `/nick`, `/invite`, `/join`, `/part`, `/upgraderoom`, `/converttodm`, `/notice`, `/plain`, `/html`, `/tableflip`, `/unflip`, `/lenny`, `/rainbow`) is a follow-up plan once the popup ships.
- The popup in reply/edit/thread composers. Only the top-level composer activates the popup for this slice.
- Help command (`/?`) — covered in the follow-up.
- Per-room command filtering (e.g. hiding `/kick` when you're not a moderator). All commands are shown to all users for now.

**File Structure:**
- `ui/shared/views/SlashCommandEngine.h` / `.cpp` — stateless helper: `find_prefix(text, cursor)`, `lookup(prefix, max_results)`. Mirrors `ShortcodeEngine`.
- `ui/shared/views/SlashCommandPopup.h` / `.cpp` — `tk::Widget` rendering rows with selection. Mirrors `ShortcodePopup`.
- `ui/shared/app/SlashCommands.h` / `.cpp` — **extend existing file** with `SlashCommandDescriptor`, `available_commands()`, and an entry for `/shrug`.
- `tests/cpp/test_slash_command_engine.cpp` — unit tests for engine.
- `ui/shared/CMakeLists.txt`, `tests/CMakeLists.txt` — register new files.
- `ui/linux-qt/src/MainWindow.{h,cpp}`, `ui/linux-gtk/src/MainWindow.{h,cpp}`, `ui/windows/src/MainWindow.{h,cpp}`, `ui/macos/src/MainWindowController.{h,mm}` — per-shell glue.

**Reference patterns to copy:**
- `ui/shared/views/ShortcodePopup.{h,cpp}` for the widget shape.
- `ui/shared/views/ShortcodeEngine.{h,cpp}` for the engine shape.
- `show_shortcode_popup_()` / `hide_shortcode_popup_()` in each shell's `MainWindow.{h,cpp}` for the per-shell hosting / keyboard interception / positioning glue.

---

## Task 1: Command registry

**Files:**
- Modify: `ui/shared/app/SlashCommands.h` (add descriptor + accessor)
- Modify: `ui/shared/app/SlashCommands.cpp` (define the table + wire `/shrug`)

- [ ] **Step 1: Write the failing test**

Add to `tests/cpp/test_slash_commands.cpp` (create if missing, otherwise append):

```cpp
#include "app/SlashCommands.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("available_commands lists me and shrug", "[slash]")
{
    const auto& cmds = tesseract::available_commands();
    REQUIRE(cmds.size() >= 2);

    auto by_name = [&](const std::string& n) {
        for (const auto& c : cmds) if (c.name == n) return &c;
        return (const tesseract::SlashCommandDescriptor*) nullptr;
    };

    const auto* me = by_name("me");
    REQUIRE(me != nullptr);
    REQUIRE(me->args_hint == "<action>");

    const auto* shrug = by_name("shrug");
    REQUIRE(shrug != nullptr);
    REQUIRE(shrug->args_hint.empty()); // /shrug takes no args
}
```

Register the new test file in `tests/CMakeLists.txt` if it isn't already (look for the `add_executable(tesseract_tests …)` source list).

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build/linux-qt6-debug && \
  ctest --test-dir build/linux-qt6-debug -R "available_commands" --output-on-failure
```

Expected: FAIL with "SlashCommandDescriptor not declared" / "available_commands not declared".

- [ ] **Step 3: Add the descriptor + accessor to the header**

Edit `ui/shared/app/SlashCommands.h`. Add **above** the `dispatch_compose_send` declaration:

```cpp
#include <vector>

namespace tesseract
{

// Metadata describing one slash command for display in the popup.
// The actual dispatch logic lives in dispatch_compose_send below.
struct SlashCommandDescriptor
{
    std::string name;         // canonical name without leading slash, e.g. "me"
    std::string args_hint;    // e.g. "<action>" or "" for argless commands
    std::string description;  // one-line user-facing description
};

// Returns the canonical command list. Stable order — the popup uses this
// vector's order when nothing has been typed yet. Lifetime: static.
const std::vector<SlashCommandDescriptor>& available_commands();

}  // namespace tesseract (re-enter for the existing declaration below)
```

(Place this *inside* the existing `namespace tesseract` block; do not re-open it.)

- [ ] **Step 4: Define the table + add /shrug dispatch**

Edit `ui/shared/app/SlashCommands.cpp`. Above the `dispatch_compose_send` function, add:

```cpp
const std::vector<SlashCommandDescriptor>& available_commands()
{
    static const std::vector<SlashCommandDescriptor> kCommands = {
        {"me",    "<action>", "Send an action message"},
        {"shrug", "",         "Append ¯\\_(ツ)_/¯"},
    };
    return kCommands;
}
```

Then in `dispatch_compose_send`, add a branch for `/shrug` **above** the existing `/me ` branch:

```cpp
    // `/shrug` (no args) — append the shrug emoticon to whatever the user
    // typed in front of the slash. With no leading text it sends just the
    // emoticon. The shrug is sent as plain text via the normal send_message
    // path so it threads / replies correctly.
    if (body == "/shrug" || body.compare(0, 7, "/shrug ") == 0)
    {
        std::string suffix = body.size() > 7 ? body.substr(7) : "";
        std::string text = "\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF";
        if (!suffix.empty()) text = suffix + " " + text;
        return client.send_message(room_id, text, "");
    }
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cmake --build build/linux-qt6-debug && \
  ctest --test-dir build/linux-qt6-debug -R "available_commands" --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Add a unit test for /shrug dispatch**

Append to `tests/cpp/test_slash_commands.cpp`:

```cpp
TEST_CASE("/shrug body parses correctly", "[slash][shrug]")
{
    // We can't exercise dispatch_compose_send without a live Client, so
    // assert the parsing by checking the descriptor + that a literal
    // "/shrug" string starts with the registered prefix.
    REQUIRE(std::string("/shrug").compare(0, 6, "/shrug") == 0);
    REQUIRE(std::string("/shrugfoo").compare(0, 7, "/shrug ") != 0);
}
```

Run and confirm PASS.

- [ ] **Step 7: Commit**

```bash
git add ui/shared/app/SlashCommands.h ui/shared/app/SlashCommands.cpp \
        tests/cpp/test_slash_commands.cpp tests/CMakeLists.txt
git commit -m "feat(compose): slash command registry + /shrug"
```

---

## Task 2: SlashCommandEngine

**Files:**
- Create: `ui/shared/views/SlashCommandEngine.h`
- Create: `ui/shared/views/SlashCommandEngine.cpp`
- Create: `tests/cpp/test_slash_command_engine.cpp`
- Modify: `ui/shared/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `tests/cpp/test_slash_command_engine.cpp`:

```cpp
#include "views/SlashCommandEngine.h"
#include <catch2/catch_test_macros.hpp>

using tesseract::views::SlashCommandEngine;

TEST_CASE("find_prefix returns query on empty-line slash", "[slash][engine]")
{
    SlashCommandEngine e;
    auto m = e.find_prefix("/", 1);
    REQUIRE(m.has_value());
    REQUIRE(m->prefix.empty());
    REQUIRE(m->start == 0);
    REQUIRE(m->end == 1);
}

TEST_CASE("find_prefix returns query while typing", "[slash][engine]")
{
    SlashCommandEngine e;
    auto m = e.find_prefix("/me", 3);
    REQUIRE(m.has_value());
    REQUIRE(m->prefix == "me");
}

TEST_CASE("find_prefix stops at space (args entered)", "[slash][engine]")
{
    SlashCommandEngine e;
    auto m = e.find_prefix("/me hello", 9);
    REQUIRE(!m.has_value());
}

TEST_CASE("find_prefix rejects mid-message slash", "[slash][engine]")
{
    SlashCommandEngine e;
    auto m = e.find_prefix("hi /me", 6);
    REQUIRE(!m.has_value());
}

TEST_CASE("find_prefix rejects non-letter chars after slash", "[slash][engine]")
{
    SlashCommandEngine e;
    REQUIRE(!e.find_prefix("/9", 2).has_value());
    REQUIRE(!e.find_prefix("/!", 2).has_value());
}

TEST_CASE("lookup ranks exact then prefix matches", "[slash][engine]")
{
    SlashCommandEngine e;
    auto results = e.lookup("m", 8);
    REQUIRE(!results.empty());
    REQUIRE(results.front().name == "me");
}

TEST_CASE("lookup returns full list for empty prefix", "[slash][engine]")
{
    SlashCommandEngine e;
    auto results = e.lookup("", 8);
    REQUIRE(results.size() >= 2);  // at least me + shrug
}

TEST_CASE("lookup returns empty for non-matching prefix", "[slash][engine]")
{
    SlashCommandEngine e;
    auto results = e.lookup("zzzz", 8);
    REQUIRE(results.empty());
}
```

Register the test source in `tests/CMakeLists.txt`.

- [ ] **Step 2: Run tests to verify they fail to compile**

```bash
cmake --build build/linux-qt6-debug 2>&1 | grep -E "SlashCommandEngine.h|error:" | head -5
```

Expected: `'views/SlashCommandEngine.h' file not found`.

- [ ] **Step 3: Create the header**

Create `ui/shared/views/SlashCommandEngine.h`:

```cpp
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views
{

struct SlashCommandMatch
{
    int start;          ///< UTF-8 byte offset of the leading '/'.
    int end;            ///< UTF-8 byte offset one past the last typed char.
    std::string prefix; ///< Characters typed after '/' (no slash).
};

struct SlashCommandSuggestion
{
    std::string name;         ///< Canonical name without leading slash.
    std::string args_hint;    ///< e.g. "<action>".
    std::string description;  ///< One-line user-facing description.
};

/// Stateless helper — all inputs passed per call; no members.
class SlashCommandEngine
{
public:
    /// Returns a match when `text` is a slash-command prefix at the start
    /// of the composer: `^/[A-Za-z_]*$` with the cursor at end-of-text.
    /// Returns nullopt for empty text, mid-message slashes, slashes
    /// followed by a space (args entered), or non-letter chars after '/'.
    std::optional<SlashCommandMatch>
    find_prefix(std::string_view text, int cursor_byte_pos) const;

    /// Filter the canonical command list (from
    /// `tesseract::available_commands()`) by name prefix. Exact match
    /// first, then prefix matches in registry order. Returns at most
    /// `max_results` suggestions. Empty prefix returns the full list.
    std::vector<SlashCommandSuggestion>
    lookup(std::string_view prefix, int max_results = 8) const;
};

} // namespace tesseract::views
```

- [ ] **Step 4: Create the implementation**

Create `ui/shared/views/SlashCommandEngine.cpp`:

```cpp
#include "views/SlashCommandEngine.h"
#include "app/SlashCommands.h"

namespace tesseract::views
{

static bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

std::optional<SlashCommandMatch>
SlashCommandEngine::find_prefix(std::string_view text, int cursor_byte_pos) const
{
    // Activate only when the slash is at position 0 (start of composer)
    // and the cursor is at end-of-text. Anything else is a literal slash
    // in a message — pass through.
    if (text.empty() || text[0] != '/') return std::nullopt;
    if (cursor_byte_pos != (int)text.size()) return std::nullopt;

    // All chars after the leading '/' must be name chars (no spaces,
    // no digits, no punctuation). The first space terminates the popup
    // because the user has moved on to typing args.
    for (std::size_t i = 1; i < text.size(); ++i)
    {
        if (!is_name_char(text[i])) return std::nullopt;
    }
    SlashCommandMatch m;
    m.start = 0;
    m.end = (int)text.size();
    m.prefix = std::string(text.substr(1));
    return m;
}

std::vector<SlashCommandSuggestion>
SlashCommandEngine::lookup(std::string_view prefix, int max_results) const
{
    std::vector<SlashCommandSuggestion> out;
    const auto& all = ::tesseract::available_commands();

    // Pass 1: exact match (only one possible).
    for (const auto& c : all)
    {
        if (c.name == prefix)
        {
            out.push_back({c.name, c.args_hint, c.description});
            break;
        }
    }
    // Pass 2: prefix matches in registry order, skipping the exact one.
    for (const auto& c : all)
    {
        if ((int)out.size() >= max_results) break;
        if (c.name == prefix) continue;  // already added
        if (c.name.size() < prefix.size()) continue;
        if (c.name.compare(0, prefix.size(), prefix) != 0) continue;
        out.push_back({c.name, c.args_hint, c.description});
    }
    return out;
}

} // namespace tesseract::views
```

- [ ] **Step 5: Register the new files**

Edit `ui/shared/CMakeLists.txt` to add (next to the `views/Shortcode*` entries):

```cmake
    views/SlashCommandEngine.h
    views/SlashCommandEngine.cpp
```

- [ ] **Step 6: Build + run all engine tests**

```bash
cmake --build build/linux-qt6-debug && \
  ctest --test-dir build/linux-qt6-debug -R "\\[slash\\]" --output-on-failure
```

Expected: all engine tests PASS.

- [ ] **Step 7: Commit**

```bash
git add ui/shared/views/SlashCommandEngine.h ui/shared/views/SlashCommandEngine.cpp \
        tests/cpp/test_slash_command_engine.cpp \
        ui/shared/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(compose): SlashCommandEngine — prefix detection + filter"
```

---

## Task 3: SlashCommandPopup widget

**Files:**
- Create: `ui/shared/views/SlashCommandPopup.h`
- Create: `ui/shared/views/SlashCommandPopup.cpp`
- Modify: `ui/shared/CMakeLists.txt`

Copy `ShortcodePopup.{h,cpp}` verbatim, then strip emoticon/image-provider logic and swap `ShortcodeSuggestion` → `SlashCommandSuggestion`. Row format: `/name <args_hint>` on one line (or two columns), description grayed below or to the right.

- [ ] **Step 1: Create the header**

Create `ui/shared/views/SlashCommandPopup.h`:

```cpp
#pragma once
#include "tk/widget.h"
#include "views/SlashCommandEngine.h"
#include <algorithm>
#include <functional>
#include <vector>

namespace tesseract::views
{

class SlashCommandPopup : public tk::Widget
{
public:
    static constexpr float kRowHeight = 44.0f; // taller than shortcode: shows two lines
    static constexpr float kWidth     = 320.0f;
    static constexpr int   kMaxRows   = 8;

    void set_suggestions(std::vector<SlashCommandSuggestion> suggestions);
    void set_selected_index(int index);
    int  selected_index() const { return selected_index_; }
    int  visible_rows() const
    {
        return std::min((int)suggestions_.size(), kMaxRows);
    }
    // Public accessor used by shell keyboard handlers — they consume
    // Up/Down/Enter themselves, then need to fire on_accepted with the
    // suggestion that lives at the current selected_index_.
    const SlashCommandSuggestion& suggestion_at(int i) const
    {
        return suggestions_[i];
    }

    std::function<void(SlashCommandSuggestion)> on_accepted;
    std::function<void()>                       on_dismissed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void     arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;

private:
    std::vector<SlashCommandSuggestion> suggestions_;
    int selected_index_ = -1;
    int hovered_index_  = -1;
    int pressed_index_  = -1;

    int row_at(float y) const
    {
        int r = (int)(y / kRowHeight);
        return (r >= 0 && r < visible_rows()) ? r : -1;
    }
};

} // namespace tesseract::views
```

- [ ] **Step 2: Create the implementation**

Open `ui/shared/views/ShortcodePopup.cpp` and read it. Create `ui/shared/views/SlashCommandPopup.cpp` mirroring its structure:

- `measure()`: returns `{kWidth, kRowHeight * visible_rows()}`.
- `arrange()`: sets `bounds_ = bounds`.
- `paint()`: for each visible row, paint a background (highlighted when index == selected_index_), then paint `/name args_hint` in the theme foreground color and `description` in a muted color, both via `tk::TextLayout`. Reuse the row layout from ShortcodePopup but make `args_hint` and `description` fields visible together — `/me <action>` on the left, `Send an action message` on the right (or under).
- `on_pointer_down/up`: pressed_index_ tracking, fire `on_accepted(suggestions_[r])` on release inside same row.
- `on_pointer_move`: update hovered_index_, schedule_repaint.
- `on_pointer_leave`: clear hovered_index_.

This is mechanical — read `ShortcodePopup.cpp` and translate. Avoid the image-provider / glyph rendering branches (we have neither).

- [ ] **Step 3: Register the new files**

Edit `ui/shared/CMakeLists.txt`, add:

```cmake
    views/SlashCommandPopup.h
    views/SlashCommandPopup.cpp
```

- [ ] **Step 4: Build to make sure it compiles**

```bash
cmake --build build/linux-qt6-debug 2>&1 | tail -10
```

Expected: clean build. (Widget tests are hard without a real Canvas; visual verification happens in Task 4+ when wired into a shell.)

- [ ] **Step 5: Commit**

```bash
git add ui/shared/views/SlashCommandPopup.h ui/shared/views/SlashCommandPopup.cpp \
        ui/shared/CMakeLists.txt
git commit -m "feat(compose): SlashCommandPopup widget"
```

---

## Task 4: Qt6 shell wiring

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h` (add engine + popup surface)
- Modify: `ui/linux-qt/src/MainWindow.cpp` (show/hide/text-change/keyboard)

Pattern to follow exactly: every `shortcode_popup_*` / `shortcode_engine_` mention in [`ui/linux-qt/src/MainWindow.{h,cpp}`]. Add a parallel `slash_popup_*` / `slash_engine_` set of members and methods. Same surface lifecycle, same `compose_text_area_changed` hook, same arrow-key/Enter/Escape filter on the native text area's key handler.

**Before starting, verify two NativeTextArea APIs exist in the Qt6 shell** — the shortcode popup uses them, so they almost certainly do:

1. **A way to read the cursor's screen position** for positioning the popup. Look at how `show_shortcode_popup_` computes its caret coordinates — the slash popup uses the exact same calculation.
2. **A way to programmatically set the composer text and move the cursor.** Used by the acceptance flow when `args_hint` is non-empty (we set the text to `/me ` and place the cursor after). If `tk::NativeTextArea` only exposes `set_text(const std::string&)`, that's enough — Qt's `QTextEdit::moveCursor(End)` will park the cursor at the end of the newly-set text automatically.

If neither of these is reachable through `tk::NativeTextArea`, the platform shell can talk to its concrete native widget directly (the shell owns the `QPlainTextEdit` / `QTextEdit` / `NSTextView` / `GtkTextView` / `EDIT` HWND).

- [ ] **Step 1: Add members to MainWindow.h**

In the same `private:` section as `shortcode_engine_` / `shortcode_popup_widget_`, add:

```cpp
    tesseract::views::SlashCommandEngine  slash_engine_;
    QWidget*                              slash_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface>     slash_popup_surface_ = nullptr;
    tesseract::views::SlashCommandPopup*  slash_popup_widget_  = nullptr;

    void show_slash_popup_(
        const std::vector<tesseract::views::SlashCommandSuggestion>& items,
        int caret_global_x, int caret_global_y);
    void hide_slash_popup_();
    bool slash_popup_visible_() const
    {
        return slash_popup_frame_ && slash_popup_frame_->isVisible();
    }
```

Add the matching `#include "views/SlashCommandEngine.h"` and `#include "views/SlashCommandPopup.h"` next to the existing shortcode includes.

- [ ] **Step 2: Implement show_slash_popup_/hide_slash_popup_**

Open the existing `MainWindow::show_shortcode_popup_(...)` / `hide_shortcode_popup_(...)` definitions in `MainWindow.cpp`. Copy them verbatim into the same file as `show_slash_popup_` / `hide_slash_popup_`, swapping every `shortcode_*` symbol for `slash_*` and `ShortcodeSuggestion` → `SlashCommandSuggestion`. Behaviour stays identical: borderless `Qt::Popup` `QWidget` positioned at the caret's screen coords, mounting a `tk::qt6::Surface` that hosts the widget.

In the `on_accepted` lambda for the slash popup, instead of calling the shortcode acceptance (text replacement of `:abc` → `:abc:`), do:

```cpp
slash_popup_widget_->on_accepted =
    [this](tesseract::views::SlashCommandSuggestion s)
{
    if (!roomTextArea_) return;
    hide_slash_popup_();
    if (s.args_hint.empty())
    {
        // No args needed — autocomplete to `/name` and send.
        std::string body = "/" + s.name;
        tesseract::dispatch_compose_send(*client_, current_room_id_, body, "");
        roomTextArea_->set_text("");
        mainApp_->room_view()->clear_compose_text();
    }
    else
    {
        // Needs args — autocomplete to `/name ` and keep the composer open.
        std::string body = "/" + s.name + " ";
        roomTextArea_->set_text(body);
        roomTextArea_->set_cursor_position((int)body.size());
        // Trigger a no-op text-changed so the popup hides (text now ends in
        // a space — `find_prefix` returns nullopt).
    }
};
slash_popup_widget_->on_dismissed = [this]
{
    hide_slash_popup_();
};
```

- [ ] **Step 3: Wire the text-changed hook**

Find the existing `roomTextArea_->on_changed = ...` lambda in MainWindow.cpp. **Before** the existing shortcode-popup branch, add:

```cpp
{
    auto m = slash_engine_.find_prefix(current_text, cursor_pos);
    if (m.has_value())
    {
        auto items = slash_engine_.lookup(m->prefix);
        if (items.empty())
        {
            hide_slash_popup_();
        }
        else
        {
            // caret_x, caret_y in screen coords come from roomTextArea_
            auto [cx, cy] = roomTextArea_->cursor_screen_pos();
            show_slash_popup_(items, cx, cy);
        }
        return; // don't fall through to shortcode popup
    }
    if (slash_popup_visible_()) hide_slash_popup_();
}
```

(`cursor_screen_pos()` already exists for the shortcode popup; reuse it. If it doesn't, look at how the existing shortcode popup positions itself and use the same calculation.)

- [ ] **Step 4: Keyboard interception**

Find the existing `roomTextArea_->on_key` (or the equivalent — search for `Key_Up` / `Key_Down` near the shortcode popup). Add a slash-popup branch with the same priority as the shortcode-popup branch:

```cpp
if (slash_popup_visible_())
{
    switch (key)
    {
        case Qt::Key_Up:
            slash_popup_widget_->set_selected_index(
                std::max(0, slash_popup_widget_->selected_index() - 1));
            return true;
        case Qt::Key_Down:
            slash_popup_widget_->set_selected_index(
                std::min(slash_popup_widget_->visible_rows() - 1,
                         slash_popup_widget_->selected_index() + 1));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
        {
            int i = std::max(0, slash_popup_widget_->selected_index());
            if (i < slash_popup_widget_->visible_rows() &&
                slash_popup_widget_->on_accepted)
            {
                slash_popup_widget_->on_accepted(
                    slash_popup_widget_->suggestion_at(i));
            }
            return true;
        }
        case Qt::Key_Escape:
            hide_slash_popup_();
            return true;
    }
}
```

`suggestion_at(i)` was added to `SlashCommandPopup` in Task 3 specifically for this call site.

- [ ] **Step 5: Build + run app**

```bash
cmake --build build/linux-qt6-debug && \
  ctest --test-dir build/linux-qt6-debug --output-on-failure
```

All tests still pass. Then manually:

```bash
./build/linux-qt6-debug/ui/linux-qt/tesseract
```

In a room, type `/` — popup should appear with `me` and `shrug` rows. Arrow keys change selection. Enter on `me` autocompletes to `/me `. Enter on `shrug` sends the shrug emoji. Escape dismisses.

- [ ] **Step 6: Commit**

```bash
git add ui/linux-qt/src/MainWindow.h ui/linux-qt/src/MainWindow.cpp
git commit -m "feat(compose-qt6): slash-command popup"
```

---

## Task 5: GTK4 shell wiring

Same as Task 4 but for `ui/linux-gtk/src/MainWindow.{h,cpp}`. The popup uses a `GtkPopover` (or whatever surface the existing GTK shortcode popup uses — find by grepping `shortcode_popup_` in the file).

- [ ] **Step 1: Mirror Task 4** — add `slash_engine_`, `slash_popup_*` members; copy `show/hide_shortcode_popup_` definitions, rename, swap types.
- [ ] **Step 2: Wire the text-changed hook and keyboard interception** with the same logic as Task 4 (in the GTK shell's text-area `connect("changed")` handler and its `connect("key-pressed")` handler).
- [ ] **Step 3: Build + manual smoke test on GTK4.**

```bash
cmake --preset linux-gtk-debug && cmake --build build/linux-gtk-debug
./build/linux-gtk-debug/ui/linux-gtk/tesseract
```

- [ ] **Step 4: Commit.**

```bash
git add ui/linux-gtk/src/MainWindow.h ui/linux-gtk/src/MainWindow.cpp
git commit -m "feat(compose-gtk4): slash-command popup"
```

---

## Task 6: Win32 shell wiring

Same as Task 4 but for `ui/windows/src/MainWindow.{h,cpp}`. The popup uses a borderless HWND (look at `shortcode_popup_hwnd_`).

- [ ] **Step 1: Mirror Task 4** — add `slash_engine_`, `slash_popup_hwnd_`, `slash_popup_surface_`, `slash_popup_widget_`; copy show/hide methods.
- [ ] **Step 2: Wire text-change + keyboard. Win32 keyboard hooks live in the same WM_KEYDOWN handler as the shortcode popup; add a slash-popup branch before the shortcode-popup branch.**
- [ ] **Step 3: Cross-compile and smoke test under Wine if available (per CLAUDE.md, the mingw preset uses `CMAKE_CROSSCOMPILING_EMULATOR=wine`):**

```bash
cmake --preset mingw-debug && cmake --build build/mingw-debug
```

- [ ] **Step 4: Commit.**

```bash
git add ui/windows/src/MainWindow.h ui/windows/src/MainWindow.cpp
git commit -m "feat(compose-win32): slash-command popup"
```

---

## Task 7: macOS shell wiring

Same as Task 4 but for `ui/macos/src/MainWindowController.{h,mm}`. The popup uses an `NSPanel` (look at existing `shortcode_popup_` in MainWindowController.mm).

- [ ] **Step 1: Mirror Task 4** — Obj-C++ syntax: members go in the `@implementation` ivar block; methods are `-(void)showSlashPopup:...` etc.
- [ ] **Step 2: Wire text-change + keyboard. The text-area change observer and `NSResponder` key handlers live in the same area as the shortcode popup wiring.**
- [ ] **Step 3: Build + smoke test:**

```bash
cmake --preset macos-appkit-arm64-debug && cmake --build build/macos-appkit-arm64-debug
open build/macos-appkit-arm64-debug/ui/macos/Tesseract.app
```

- [ ] **Step 4: Commit.**

```bash
git add ui/macos/src/MainWindowController.h ui/macos/src/MainWindowController.mm
git commit -m "feat(compose-macos): slash-command popup"
```

---

## Task 8: Final integration test + docs

- [ ] **Step 1: Full test sweep**

```bash
cargo test -p tesseract-sdk-ffi
cmake --build build/linux-qt6-debug
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

All Rust + C++ tests pass.

- [ ] **Step 2: Update docs**

Edit `CHANGES.md`, prepend to the Unreleased section:

```markdown
- feat(compose): `/` in the composer opens a filtered slash-command popup —
  arrow keys / Enter accept, Escape dismisses. Initial command set:
  `/me <action>` (m.emote) and `/shrug` (text macro). Wired in all four
  shells (Qt6, GTK4, Win32, macOS) following the same per-shell
  `ShortcodePopup` hosting pattern.
```

Edit `TODO.md`: tick off the `- [ ] ComposeBar: / slash-command hint popup` item.

Edit `STATUS.md` if test counts changed.

- [ ] **Step 3: Commit docs**

```bash
git add CHANGES.md TODO.md STATUS.md
git commit -m "docs: slash-command popup in the composer"
```

---

## Verification

End-to-end:

1. Run the app: `./build/linux-qt6-debug/ui/linux-qt/tesseract`
2. Open any room, click into the composer.
3. Type `/` — popup appears showing `me` and `shrug`.
4. Type `m` — popup narrows to `me`. Type backspace — popup widens back.
5. Press Down to navigate. Selection highlight tracks.
6. Press Enter on `me` — composer text becomes `/me ` (with trailing space and cursor after); popup hides.
7. Type `dances` and press Enter — message sends as `m.emote` (verify in another client; appears as "* @you dances").
8. Type `/shrug` — popup appears with `shrug` selected. Press Enter — message sends as `¯\_(ツ)_/¯` (plain text via `m.text`).
9. Type `/zzz` — popup appears empty (or hides); no autocomplete suggestions.
10. Type `/`, press Escape — popup hides; the `/` stays in the composer as literal text.
11. Type `hi /me` — no popup (mid-message slash is literal).
12. Switch rooms with the popup open — popup hides cleanly.

All twelve must pass before declaring the feature done.
