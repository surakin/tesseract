# Shell De-duplication Hoist Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hoist eight behavior-preserving duplications out of the four native shells into `ui/shared/` (`ShellBase` / `RoomWindowBase` / new shared helpers), removing ~650+ duplicated lines.

**Architecture:** Each task extracts a common algorithm into shared code and leaves a tiny per-shell virtual hook for the one genuine platform primitive (repaint / native widget / native picker refresh). No behavior changes; verified by the existing suite (ctest 323/323, cargo 92/92) after every task. Known per-shell divergences are reconciled to the documented canonical form.

**Tech Stack:** C++20, `tesseract_tk`, Catch2; Qt6 + GTK4 build-verified on Linux; Win32 (CMake-hard-gated) + macOS (no Xcode) are **parity-review-only on this host** — implement by mirroring the verified Qt6/GTK4 result; they compile elsewhere.

---

## Ground rules (every task)

- **`origin/main` moves constantly — line numbers drift.** LOCATE every edit site by **content/grep**, not by the line numbers cited here. The cited lines are from the 2026-05-18 audit and are indicative only.
- **Behavior-preserving.** Do not change observable behavior except the explicitly-listed divergence reconciliations.
- **Repo formatting is enforced** (`.clang-format`, `cargo fmt`). Run `clang-format -i` on every changed C++/ObjC file before committing.
- **macOS C++↔ObjC boundary stays.** `MacShell` (C++) often does the *secondary-window* part in C++ then hands the *primary* part to an ObjC `MainWindowController` method via `[c handle…:]` (often `.release()`-ing `unique_ptr<Event>` into `Event*`). Hoist only the parts that are clean; keep that boundary.
- **Per-task verification (run from the worktree):**
  ```bash
  cmake --build build/linux-qt6-debug 2>&1 | grep -E "error:" | head
  cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -1
  ctest --test-dir build/linux-qt6-debug 2>&1 | grep -E "% tests passed|failed out of"
  cmake --build build/linux-gtk-debug --target tesseract 2>&1 | grep -E "error:" | head
  cd sdk && cargo test -p tesseract-sdk-ffi 2>&1 | grep -E "test result:" | tail -1 && cd ..
  ```
  Expected: zero `error:`; **ctest 323/323**; GTK4 clean; **cargo 92/92**. Win32/macOS edits: confirm only their TUs changed and the Linux suite is unaffected (those TUs aren't compiled on Linux).
- Clangd/IDE diagnostics in this repo are KNOWN false positives — only `cmake`/`ctest`/`cargo` are authoritative.

## File inventory

| File | Touched by tasks |
|---|---|
| `ui/shared/app/ShellBase.h` / `.cpp` | 1,2,3,4 |
| `ui/shared/app/RoomWindowBase.h` / `.cpp` | 8 |
| `ui/shared/views/text_util.h` (new) | 5,8 |
| `ui/shared/up_connector_core.h` / `.cpp` (new) | 6 |
| `ui/shared/screen_lock_state.h` (new) | 7 |
| `ui/shared/CMakeLists.txt` | 6 (if a new .cpp TU) |
| `ui/linux-qt/src/MainWindow.{cpp,h}` | 1,2,3,4 |
| `ui/linux-gtk/src/MainWindow.{cpp,h}` | 1,2,3,4 |
| `ui/windows/src/MainWindow.{cpp,h}` | 1,2,3,4 (parity) |
| `ui/macos/src/MainWindowController.mm` | 1,2,3,4 (parity) |
| `ui/{linux-qt,linux-gtk,windows}/src/RoomWindow.cpp`, `ui/macos/src/RoomWindowController.mm` | 5,8 |
| `ui/{linux-qt,linux-gtk}/src/LinuxUpConnector{Qt,Gtk}.cpp` | 6 |
| `ui/{linux-qt,linux-gtk}/src/LinuxScreenLock{Qt,Gtk}.{cpp,h}` | 7 |
| `ui/macos/src/util.h` | 5 |
| `CHANGES.md`, `STATUS.md` | 9 |

---

## Task 1: `ShellBase::build_rows_()` — extract the snapshot→rows loop

The loop `for ev in snapshot { skip null; ensure_row_media_(*ev); if(!in_reply_to_id.empty()) ensure_reply_details_(event_id); rows.push_back(make_row_data(*ev, my_user_id_)); }` is duplicated ~8× across the 4 shells (primary + secondary in each timeline-reset / message-inserted/updated path). Extract one shared helper. **Qt6 wraps `ensure_row_media_` in a local `ensureRowMedia` that adds `mediaImageSizes_` decode-size hints — this wrapper must NOT be lost.** Solution: `build_rows_` calls a `virtual void prep_row_media_(const Event&)` hook whose default is `ensure_row_media_(ev)`; Qt6 overrides it with its existing `ensureRowMedia` body.

**Files:** `ui/shared/app/ShellBase.h`, `ui/shared/app/ShellBase.cpp`, all 4 shells.

- [ ] **Step 1: Add the hook + helper declarations to `ShellBase.h`**

In the "EventHandlerBase UI-thread hooks" virtual block (near `handle_image_packs_updated_ui_`), add:
```cpp
    // Per-shell media-prefetch for one row. Default = ensure_row_media_.
    // Qt6 overrides to also record decode-size hints (mediaImageSizes_).
    virtual void prep_row_media_(const Event& ev) { ensure_row_media_(ev); }
```
In the "Concrete helpers" block (near `ensure_row_media_` / `push_rooms_`), add:
```cpp
    // Build MessageRowData rows from an event snapshot: prep media, request
    // reply details, make_row_data. Used by every shell's timeline-reset and
    // message handlers (primary + secondary-window paths).
    std::vector<views::MessageRowData>
    build_rows_(const std::vector<std::unique_ptr<Event>>& snapshot);
    // macOS hands primary-path events across the ObjC boundary as raw
    // pointers; this overload serves that path.
    std::vector<views::MessageRowData>
    build_rows_(const std::vector<Event*>& snapshot);
```
Ensure `#include "views/MessageListView.h"` (for `views::MessageRowData` / `make_row_data`) is present in `ShellBase.h` (it already pulls `tesseract/types.h`; `make_row_data` is declared in `ui/shared/views/MessageListView.h:152`). If not already included, add it.

- [ ] **Step 2: Implement in `ShellBase.cpp`**

Add (near `ensure_row_media_` / `push_rooms_`):
```cpp
std::vector<views::MessageRowData>
ShellBase::build_rows_(const std::vector<std::unique_ptr<Event>>& snapshot)
{
    std::vector<views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    for (const auto& ev : snapshot)
    {
        if (!ev)
        {
            continue;
        }
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        rows.push_back(views::make_row_data(*ev, my_user_id_));
    }
    return rows;
}

std::vector<views::MessageRowData>
ShellBase::build_rows_(const std::vector<Event*>& snapshot)
{
    std::vector<views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    for (auto* ev : snapshot)
    {
        if (!ev)
        {
            continue;
        }
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        rows.push_back(views::make_row_data(*ev, my_user_id_));
    }
    return rows;
}
```

- [ ] **Step 3: Qt6 — override `prep_row_media_`, replace the loops**

In `ui/linux-qt/src/MainWindow.h` declare `void prep_row_media_(const tesseract::Event& ev) override;`. In `MainWindow.cpp`, rename the existing `void MainWindow::ensureRowMedia(const tesseract::Event& ev)` definition to `void MainWindow::prep_row_media_(const tesseract::Event& ev)` (its body — the `mediaImageSizes_` size-hint prologue then `ensure_row_media_(ev)` — is unchanged). Update its internal/other callers (grep `ensureRowMedia(`) to `prep_row_media_(`. Then in `handle_timeline_reset_ui_` replace the inline primary loop with `auto rows = build_rows_(snapshot);` and the secondary lambda's inline loop with `w->on_timeline_reset(build_rows_(snapshot));` (delete the duplicated loop bodies). Do the same in `handle_message_inserted_ui_`/`handle_message_updated_ui_` secondary lambdas: replace the inline `ensureRowMedia/ensure_reply_details_/make_row_data` 3-liner with a single-event build — since these are single-event, keep using the existing inline form OR add a tiny single-event path; **for single-event handlers, do NOT force build_rows_** (it takes a vector) — leave the single-event 3-liners as-is in Task 1; they are addressed structurally in Task 4. Task 1 only collapses the **snapshot** loops (timeline_reset primary + secondary).

- [ ] **Step 4: GTK4 — replace the loops in `push_timeline_reset`**

In `ui/linux-gtk/src/MainWindow.cpp` `push_timeline_reset`, replace the primary inline loop with `auto rows = build_rows_(snapshot);` and the secondary lambda's inline loop with `w->on_timeline_reset(build_rows_(snapshot));`. GTK4 calls `ensure_row_media_` directly and has no size-hint wrapper, so the default `prep_row_media_` is correct — do **not** override it for GTK4.

- [ ] **Step 5: Win32 — replace the loops in `on_tesseract_timeline_reset`**

In `ui/windows/src/MainWindow.cpp` `on_tesseract_timeline_reset(PostedTimelineReset* payload)`, replace the primary inline loop with `auto rows = build_rows_(payload->snapshot);` and the secondary lambda likewise. Win32's local `ensure_row_media` is a pure delegate (no size hints) → default `prep_row_media_` is correct; do not override. (Parity-only build.)

- [ ] **Step 6: macOS — replace the C++ secondary loop + the ObjC raw-pointer primary loop**

In `ui/macos/src/MainWindowController.mm` `MacShell::handle_timeline_reset_ui_`, replace the C++ secondary lambda's inline loop with `w->on_timeline_reset(build_rows_(snapshot));` (still over `unique_ptr` before the `.release()`). The ObjC `- (void)handleTimelineReset:snapshot:` receives `std::vector<tesseract::Event*>` — replace its inline primary loop with `auto rows = _shell->build_rows_(snapshot);` (the new `Event*` overload). Keep the existing `for (auto* ev : snapshot) delete ev;` cleanup loop and the `.release()` marshaling unchanged. macOS calls `_shell->ensure_row_media_` directly → default `prep_row_media_` correct. (Parity-only build.)

- [ ] **Step 7: Build, test, commit**
```bash
cmake --build build/linux-qt6-debug 2>&1 | grep -E "error:" | head
ctest --test-dir build/linux-qt6-debug 2>&1 | grep -E "% tests passed|failed out of"
cmake --build build/linux-gtk-debug --target tesseract 2>&1 | grep -E "error:" | head
clang-format -i ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp ui/linux-qt/src/MainWindow.{cpp,h} ui/linux-gtk/src/MainWindow.cpp ui/windows/src/MainWindow.cpp ui/macos/src/MainWindowController.mm
git add -A ui/shared/app ui/linux-qt ui/linux-gtk ui/windows ui/macos
git commit -m "refactor(shell): hoist build_rows_ snapshot loop into ShellBase"
```
Expected: zero errors, ctest 323/323, GTK4 clean.

---

## Task 2: Concrete `ShellBase::handle_account_prefs_updated_ui_()`

The body (parse prefs json; if `last_room` non-empty && `pending_restore_room_` empty && `current_room_id_` empty → set `pending_restore_room_`) is identical in 4 shells. Qt6 additionally (correctly) gates on the active account's user-id; that is the canonical behavior. Make the `ShellBase` virtual a concrete method with the Qt6 gate; delete all overrides.

**Files:** `ui/shared/app/ShellBase.h` / `.cpp`, all 4 shells.

- [ ] **Step 1: ShellBase — replace the empty virtual with a concrete declaration**

In `ShellBase.h`, change the inline empty virtual to a declaration:
```cpp
    // Concrete: only the active account's prefs set the pending restore room.
    virtual void handle_account_prefs_updated_ui_(std::string user_id,
                                                  std::string json);
```
(Keep it `virtual` so a shell *could* still override, but it now has a real body.)

- [ ] **Step 2: ShellBase.cpp — implement it (Qt6 canonical form)**
```cpp
void ShellBase::handle_account_prefs_updated_ui_(std::string user_id,
                                                 std::string json)
{
    if (active_account_index_ < 0 ||
        accounts_[active_account_index_]->user_id != user_id)
    {
        return;
    }
    auto prefs = tesseract::Prefs::parse(json);
    if (!prefs.last_room.empty() && pending_restore_room_.empty() &&
        current_room_id_.empty())
    {
        pending_restore_room_ = prefs.last_room;
    }
}
```
Add `#include <tesseract/prefs.h>` to `ShellBase.cpp` if not present (grep; `Prefs::parse` must resolve).

- [ ] **Step 3: Delete the per-shell overrides**

- Qt6: delete `MainWindow::handle_account_prefs_updated_ui_` definition + its `MainWindow.h` declaration.
- GTK4: delete the `handle_account_prefs_updated_ui_` forwarder, the `push_account_prefs_updated` method, and both declarations. (Confirm `push_account_prefs_updated` has no other caller via grep; it does not.)
- Win32: delete `MainWindow::handle_account_prefs_updated_ui_` + declaration.
- macOS: delete `MacShell::handle_account_prefs_updated_ui_` (C++) and the ObjC `- (void)handleAccountPrefsUpdated:` and its declaration. (Confirm no other caller of `handleAccountPrefsUpdated:` via grep; there is none.)

**Behavior reconciliation (intended):** GTK4/Win32/macOS previously ignored `user_id` (applied unconditionally); they now get Qt6's stricter active-account gate. This is the correct behavior and is the explicit reconciliation for this task.

- [ ] **Step 4: Build, test, commit**
```bash
# (run the per-task verification block)
clang-format -i ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp ui/linux-qt/src/MainWindow.{cpp,h} ui/linux-gtk/src/MainWindow.{cpp,h} ui/windows/src/MainWindow.{cpp,h} ui/macos/src/MainWindowController.mm
git add -A ui/shared/app ui/linux-qt ui/linux-gtk ui/windows ui/macos
git commit -m "refactor(shell): concrete ShellBase::handle_account_prefs_updated_ui_ (active-account gate)"
```
Expected: zero errors, ctest 323/323, GTK4 clean.

---

## Task 3: Hoist `cached_emoticons_` + its rebuild

The `cached_emoticons_` member exists per shell; the rebuild loop (`clear()` + `for pack in list_image_packs(): for img in list_pack_images(pack.id, Emoticon): push_back`) is byte-identical 4×. The per-shell native picker-refresh prologue differs and stays behind a hook.

**Files:** `ui/shared/app/ShellBase.h` / `.cpp`, all 4 shells.

- [ ] **Step 1: ShellBase.h — add member, hook, concrete handler**

Add data member near the image-cache members:
```cpp
    // MSC2545 emoticon flat list (shortcode popup source). Rebuilt on
    // handle_image_packs_updated_ui_.
    std::vector<tesseract::ImagePackImage> cached_emoticons_;
```
Add to the virtual hooks block:
```cpp
    // Per-shell native sticker/emoji picker refresh prologue. Default no-op.
    virtual void refresh_pickers_packs_() {}
```
Change `handle_image_packs_updated_ui_` from inline-empty to a concrete declaration:
```cpp
    virtual void handle_image_packs_updated_ui_();
```
Ensure `#include <tesseract/image_pack.h>` is present in `ShellBase.h` (grep; `ImagePackImage` / `PackUsageFilter` must resolve — `client_->list_image_packs()` etc. already used elsewhere in ShellBase translation unit).

- [ ] **Step 2: ShellBase.cpp — implement**
```cpp
void ShellBase::handle_image_packs_updated_ui_()
{
    refresh_pickers_packs_();
    cached_emoticons_.clear();
    if (client_)
    {
        for (auto& pack : client_->list_image_packs())
        {
            for (auto& img : client_->list_pack_images(
                     pack.id, tesseract::PackUsageFilter::Emoticon))
            {
                cached_emoticons_.push_back(std::move(img));
            }
        }
    }
}
```

- [ ] **Step 3: Per-shell — delete the member + rebuild, move prologue into the hook**

For each shell: delete the `cached_emoticons_` member declaration (it is now inherited from ShellBase). Replace the shell's `handle_image_packs_updated_ui_` with an override of `refresh_pickers_packs_()` containing ONLY that shell's native picker-refresh prologue, and delete the duplicated rebuild loop:
- **Qt6**: `void MainWindow::refresh_pickers_packs_()` body = the `stickerPicker_->refreshPacks(); emojiPicker_->refreshEmoticonPacks();` block (null-guarded as today). Delete `MainWindow::handle_image_packs_updated_ui_` + its declaration; declare `void refresh_pickers_packs_() override;`.
- **GTK4**: `refresh_pickers_packs_()` body = the existing `apply_image_packs_updated()` content (sticker/emoji `refresh_*` + `_surface_->relayout()`). Delete the GTK4 `handle_image_packs_updated_ui_`, `push_image_packs_updated`, and `apply_image_packs_updated` (fold its body into the override; confirm no other callers via grep).
- **Win32**: `refresh_pickers_packs_()` body = `refresh_sticker_picker(); refresh_emoji_picker();`. Delete Win32 `handle_image_packs_updated_ui_`; keep `refresh_sticker_picker`/`refresh_emoji_picker` (still called). (Parity.)
- **macOS**: override is on `MacShell`: `void MacShell::refresh_pickers_packs_()` calls `[ctrl_ handleImagePacksRefreshPickers]` (rename/repurpose the ObjC `handleImagePacksUpdated` to do ONLY the `StickerPickerPanel` refresh prologue, dropping the `cached_emoticons_` rebuild block since the base now does it). Delete `MacShell::handle_image_packs_updated_ui_`; the ObjC method keeps only the panel-refresh lines. Confirm `_shell->cached_emoticons_` still resolves (now an inherited ShellBase member, already reachable via the existing `MacShell` `using` block — if `cached_emoticons_` is not in that `using` block and ObjC needs it elsewhere, add `using ShellBase::cached_emoticons_;`). (Parity.)

Grep every shell for other readers of `cached_emoticons_` (shortcode popup code) and confirm they still compile against the inherited member (same name/type, no access change — it's `protected` in ShellBase and the shells are subclasses; macOS reads via `_shell->` so add the `using` if needed).

- [ ] **Step 4: Build, test, commit**
```bash
# per-task verification block
clang-format -i ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp ui/linux-qt/src/MainWindow.{cpp,h} ui/linux-gtk/src/MainWindow.{cpp,h} ui/windows/src/MainWindow.{cpp,h} ui/macos/src/MainWindowController.mm
git add -A ui/shared/app ui/linux-qt ui/linux-gtk ui/windows ui/macos
git commit -m "refactor(shell): hoist cached_emoticons_ + rebuild into ShellBase (picker-refresh hook)"
```
Expected: zero errors, ctest 323/323, GTK4 clean. (Shortcode popup tests still pass — they read the now-inherited member.)

---

## Task 4: Hoist the secondary-window dispatch into ShellBase

For `timeline_reset` / `message_inserted` / `message_updated` / `message_removed` / `rooms_updated`, the **secondary-window** portion is structurally identical across shells (build rows via `build_rows_` from Task 1, call the matching `RoomWindowBase::on_*`). Give `ShellBase` concrete helpers the shells call, so the per-shell secondary blocks vanish. The **primary** mutation stays per-shell (still its own hook/path; macOS keeps its ObjC split). `dispatch_to_secondary_windows_` is already shared.

**Files:** `ui/shared/app/ShellBase.h` / `.cpp`, all 4 shells.

- [ ] **Step 1: ShellBase.h — declare the secondary helpers**

In the "Concrete helpers" block:
```cpp
    // Secondary-window fan-out (primary-window mutation stays per-shell).
    void dispatch_timeline_reset_secondary_(
        const std::string& room_id,
        const std::vector<std::unique_ptr<Event>>& snapshot);
    void dispatch_message_inserted_secondary_(const std::string& room_id,
                                              std::size_t index,
                                              const Event& ev);
    void dispatch_message_updated_secondary_(const std::string& room_id,
                                             std::size_t index,
                                             const Event& ev);
    void dispatch_message_removed_secondary_(const std::string& room_id,
                                             std::size_t index);
    // Refresh open pop-out windows' room metadata from rooms_.
    void update_secondary_room_infos_();
```

- [ ] **Step 2: ShellBase.cpp — implement**
```cpp
void ShellBase::dispatch_timeline_reset_secondary_(
    const std::string& room_id,
    const std::vector<std::unique_ptr<Event>>& snapshot)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w) { w->on_timeline_reset(build_rows_(snapshot)); });
}

void ShellBase::dispatch_message_inserted_secondary_(
    const std::string& room_id, std::size_t index, const Event& ev)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w)
        {
            prep_row_media_(ev);
            if (!ev.in_reply_to_id.empty())
            {
                ensure_reply_details_(ev.event_id);
            }
            w->on_message_inserted(index,
                                   views::make_row_data(ev, my_user_id_));
        });
}

void ShellBase::dispatch_message_updated_secondary_(
    const std::string& room_id, std::size_t index, const Event& ev)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w)
        {
            prep_row_media_(ev);
            if (!ev.in_reply_to_id.empty())
            {
                ensure_reply_details_(ev.event_id);
            }
            w->on_message_updated(index,
                                  views::make_row_data(ev, my_user_id_));
        });
}

void ShellBase::dispatch_message_removed_secondary_(
    const std::string& room_id, std::size_t index)
{
    dispatch_to_secondary_windows_(
        room_id, [&](RoomWindowBase* w) { w->on_message_removed(index); });
}

void ShellBase::update_secondary_room_infos_()
{
    for (const auto& [rid, w] : secondary_windows_)
    {
        for (const auto& r : rooms_)
        {
            if (r.id == rid)
            {
                w->on_room_info_updated(r);
                break;
            }
        }
    }
}
```
(`RoomWindowBase.h` is already a transitive include of `ShellBase.h`; if `on_*` signatures aren't visible, `#include "app/RoomWindowBase.h"` in `ShellBase.cpp`.)

- [ ] **Step 3: Replace the per-shell secondary blocks with the helper calls**

For each shell, in each of the five handlers, **delete** the inline `dispatch_to_secondary_windows_([&]{ …rebuild… w->on_*() })` block (and the manual `for (auto& [room_id,w] : secondary_windows_)` loop in `on_rooms_updated_`) and replace with the one matching `dispatch_*_secondary_` / `update_secondary_room_infos_()` call:
- Qt6: in `handle_timeline_reset_ui_` → `dispatch_timeline_reset_secondary_(room_id, snapshot);`; `handle_message_inserted_ui_` → `dispatch_message_inserted_secondary_(room_id, index, *ev);` (keep the `if (ev && ev->type != Unhandled)` guard around it); `_updated_`/`_removed_` similarly; `on_rooms_updated_` → replace the manual secondary loop with `update_secondary_room_infos_();`.
- GTK4: same in `push_timeline_reset`/`push_message_*`/`on_rooms_updated_`.
- Win32: same in `on_tesseract_timeline_reset`/`on_tesseract_message_*`/`on_rooms_updated_` (event is `*payload->event`, index `payload->index`). (Parity.)
- macOS: in each `MacShell::handle_*` the C++ secondary block becomes the one helper call (it runs in C++ before the ObjC `.release()` handoff — unchanged ordering); `MacShell::on_rooms_updated_` manual secondary loop → `update_secondary_room_infos_();`. The ObjC primary methods are unchanged. (Parity.)

**Divergence reconciliation:** macOS `on_rooms_updated_` gates the `pending_restore_room_` consume on `last_room_list_state_ == RoomListState::Running`; the other three don't. This task does NOT touch the restore-room branch (only the secondary loop) — leave each shell's restore-room logic exactly as-is. Note the divergence in the commit message; unifying it is out of scope here.

- [ ] **Step 4: Build, test, commit**
```bash
# per-task verification block
clang-format -i ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp ui/linux-qt/src/MainWindow.cpp ui/linux-gtk/src/MainWindow.cpp ui/windows/src/MainWindow.cpp ui/macos/src/MainWindowController.mm
git add -A ui/shared/app ui/linux-qt ui/linux-gtk ui/windows ui/macos
git commit -m "refactor(shell): hoist secondary-window dispatch into ShellBase"
```
Expected: zero errors, ctest 323/323, GTK4 clean.

---

## Task 5: Shared `tesseract::text::trim`

A leading/trailing `" \t\n\r"` trim is inlined ≥3× (per-shell RoomWindow + main-window compose `on_send`) and exists once as macOS-only `tesseract::macos::trim` (`ui/macos/src/util.h`). Add one shared helper; replace all copies.

**Files:** new `ui/shared/views/text_util.h`; `ui/macos/src/util.h`; the inline copies.

- [ ] **Step 1: Create `ui/shared/views/text_util.h`**
```cpp
#pragma once
#include <string>
#include <string_view>

namespace tesseract::text
{

// Trim leading/trailing ASCII whitespace (space, tab, CR, LF).
inline std::string trim(std::string_view s)
{
    constexpr const char* ws = " \t\n\r";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos)
    {
        return {};
    }
    const auto e = s.find_last_not_of(ws);
    return std::string(s.substr(b, e - b + 1));
}

} // namespace tesseract::text
```
(Header-only, no TU/CMake change. It lives under `ui/shared/views/` next to `markdown`/`html_spans` utilities.)

- [ ] **Step 2: Replace every inline trim + the macOS helper**

`grep -rnE "find_first_not_of|find_last_not_of" ui/{linux-qt,linux-gtk,windows,macos}/src` and the main-window `on_send` wiring. For each inlined trim block producing the trimmed compose body, `#include "views/text_util.h"` and replace the block with `tesseract::text::trim(<the input>)`. In `ui/macos/src/util.h`, replace the body of `tesseract::macos::trim` with `return tesseract::text::trim(s);` (keep the macOS symbol as a thin alias so existing macOS call sites compile unchanged; add `#include "views/text_util.h"`), OR delete `tesseract::macos::trim` and repoint its callers to `tesseract::text::trim` — prefer the alias to minimize parity-only macOS churn. Behavior is identical (same whitespace set, same first/last-not-of logic).

- [ ] **Step 3: Build, test, commit**
```bash
# per-task verification block
clang-format -i ui/shared/views/text_util.h ui/macos/src/util.h <each changed shell file>
git add -A ui/shared/views/text_util.h ui/macos/src/util.h ui/linux-qt ui/linux-gtk ui/windows ui/macos
git commit -m "refactor(ui): shared tesseract::text::trim; drop inlined copies"
```
Expected: zero errors, ctest 323/323, GTK4 clean.

---

## Task 6: Shared `UpConnectorCore`

`LinuxUpConnectorQt` and `LinuxUpConnectorGtk` each reimplement `sanitize_token()`, the push-endpoint normalize rule (require `https://`, non-empty host, force path `/_matrix/push/v1/notify`, strip query/fragment), and the `register_pusher`/`remove_pusher` calls. Extract the pure logic; the QtDBus/GDBus bus glue stays per-shell.

**Files:** new `ui/shared/up_connector_core.{h,cpp}` (or header-only); `ui/shared/CMakeLists.txt` (if a .cpp); `LinuxUpConnector{Qt,Gtk}.cpp`.

- [ ] **Step 1: Read both connectors' exact current code**

`grep -n "sanitize_token\|register_pusher\|remove_pusher\|https://\|/_matrix/push/v1/notify" ui/linux-qt/src/LinuxUpConnectorQt.cpp ui/linux-gtk/src/LinuxUpConnectorGtk.cpp` and read the `sanitize_token` body + the endpoint-validation block + the `register_pusher`/`remove_pusher` call sites + `client/include/tesseract/up_connector.h`. The two `sanitize_token` bodies are byte-identical; the normalize rule is identical (Qt uses `QUrl`, GTK hand-parses — same output contract).

- [ ] **Step 2: Create `ui/shared/up_connector_core.h`** (header-only; pure std + `tesseract::Client*`)
```cpp
#pragma once
#include <tesseract/client.h>
#include <optional>
#include <string>

namespace tesseract::up
{

// Keep only chars valid in a UnifiedPush registration token; identical to
// the prior per-shell sanitize_token().
std::string sanitize_token(const std::string& raw);

// Validate + normalize a UnifiedPush endpoint to the Matrix push URL form:
// require https scheme + non-empty host, force path /_matrix/push/v1/notify,
// drop query/fragment. Returns std::nullopt if invalid.
std::optional<std::string> normalize_endpoint(const std::string& endpoint);

// Thin wrappers over the SDK push API (centralise the fixed arg strings).
inline void register_push(tesseract::Client& c, const std::string& token,
                          const std::string& url)
{
    c.register_pusher(token, "im.gnomos.tesseract", "Tesseract",
                      "Linux Desktop", url, "en");
}
inline void remove_push(tesseract::Client& c, const std::string& token)
{
    c.remove_pusher(token, "im.gnomos.tesseract");
}

} // namespace tesseract::up
```
Implement `sanitize_token` / `normalize_endpoint` in a new `ui/shared/up_connector_core.cpp` (or `inline` in the header to avoid a TU). `sanitize_token` = the exact char-filter loop copied verbatim from `LinuxUpConnectorQt.cpp`. `normalize_endpoint` = a dependency-free reimplementation of the shared rule (parse scheme/host/path without QUrl/GLib: reject if not starting `https://`; split authority/path; require non-empty host; set path to `/_matrix/push/v1/notify`; drop `?`/`#`). **Verify** it produces the same output as BOTH current implementations for: a normal endpoint, an endpoint with a query string, an http (reject) endpoint, an endpoint with no path. If header-only is awkward (loop logic), add the `.cpp` and append `up_connector_core.cpp` to the `tesseract_*` shared sources in `ui/shared/CMakeLists.txt` (mirror how an existing shared `.cpp` like `blurhash.cpp`/`map_tiles.cpp` is listed).

- [ ] **Step 3: Repoint both Linux connectors**

In `LinuxUpConnectorQt.cpp` and `LinuxUpConnectorGtk.cpp`: `#include "up_connector_core.h"`; delete the local `sanitize_token` and the inline endpoint-normalize block; call `tesseract::up::sanitize_token(...)` and `tesseract::up::normalize_endpoint(...)` (handle `std::nullopt` exactly as the old invalid-endpoint path did — early return / log, matching current behavior). Replace the `client_->register_pusher(...)` / `remove_pusher(...)` calls with `tesseract::up::register_push(*client_, …)` / `remove_push(*client_, …)`. The D-Bus subscription/bus lifecycle code stays untouched.

- [ ] **Step 4: Build, test, commit** (both shells are Linux-buildable — fully verifiable)
```bash
# per-task verification block (both Qt6 + GTK4 exercise this)
clang-format -i ui/shared/up_connector_core.* ui/linux-qt/src/LinuxUpConnectorQt.cpp ui/linux-gtk/src/LinuxUpConnectorGtk.cpp
git add -A ui/shared ui/linux-qt ui/linux-gtk
git commit -m "refactor(ui): shared UpConnectorCore (token sanitize + endpoint normalize)"
```
Expected: zero errors, ctest 323/323, GTK4 clean.

---

## Task 7: Shared `ScreenLockState`

`LinuxScreenLockQt` and `LinuxScreenLockGtk` duplicate the logind D-Bus constants and the locked-state machine (`locked_` default false; `LockedHint` initial read; `Lock`/`Unlock` flip). The bus subscription glue differs (QtDBus vs GDBus) and stays.

**Files:** new `ui/shared/screen_lock_state.h`; `LinuxScreenLock{Qt,Gtk}.{cpp,h}`.

- [ ] **Step 1: Read both implementations** (`LinuxScreenLock{Qt,Gtk}.{cpp,h}` are small; read fully) and `client/include/tesseract/screen_lock.h` (the `IScreenLock` interface + `NullScreenLock`). Confirm the constants/state are identical.

- [ ] **Step 2: Create `ui/shared/screen_lock_state.h`**
```cpp
#pragma once
#include <atomic>

namespace tesseract::screenlock
{

// logind session D-Bus identifiers shared by the Qt6/GTK4 probes.
inline constexpr const char* kLogindService = "org.freedesktop.login1";
inline constexpr const char* kSessionIface  = "org.freedesktop.login1.Session";
inline constexpr const char* kSessionPath   =
    "/org/freedesktop/login1/session/auto";

// Best-effort locked state: defaults to UNLOCKED (false) when the hint is
// unavailable, matching the prior per-shell policy.
class State
{
public:
    bool is_locked() const { return locked_.load(std::memory_order_relaxed); }
    void set_initial(bool v) { locked_.store(v, std::memory_order_relaxed); }
    void on_lock()   { locked_.store(true,  std::memory_order_relaxed); }
    void on_unlock() { locked_.store(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> locked_{false};
};

} // namespace tesseract::screenlock
```

- [ ] **Step 3: Repoint both Linux screen locks**

In each `LinuxScreenLock{Qt,Gtk}`: replace the hardcoded `org.freedesktop.login1`/`.Session`/`/…/session/auto` literals with the `screenlock::k*` constants; replace the `bool locked_` member + manual flips with a `tesseract::screenlock::State state_;`; `is_locked()` returns `state_.is_locked()`; the constructor's initial `LockedHint` read calls `state_.set_initial(...)`; the `Lock`/`Unlock` signal callbacks call `state_.on_lock()` / `state_.on_unlock()`. Keep each shell's ~15-line QtDBus / GDBus subscription glue exactly as-is.

- [ ] **Step 4: Build, test, commit** (both Linux-buildable — fully verifiable)
```bash
# per-task verification block
clang-format -i ui/shared/screen_lock_state.h ui/linux-qt/src/LinuxScreenLockQt.{cpp,h} ui/linux-gtk/src/LinuxScreenLockGtk.{cpp,h}
git add -A ui/shared ui/linux-qt ui/linux-gtk
git commit -m "refactor(ui): shared ScreenLockState (logind constants + locked state)"
```
Expected: zero errors, ctest 323/323, GTK4 clean.

---

## Task 8 (flagship): `RoomWindowBase::wire_room_view_()`

Each shell's secondary RoomWindow constructor repeats ~75-90 lines wiring identical `RoomView` providers + compose closures; only the repaint primitive and an optional `text_area_->set_text("")` differ. Hoist all of it into `RoomWindowBase` behind two hooks. The closures call already-shared `RoomWindowBase` SDK helpers (`send_message_`, `send_reply_`, `send_edit_`, `delete_event_`, `toggle_reaction_`, `send_receipt_`, `request_pagination_back_`, `shell_avatar_`, `shell_image_`, `fetch_source_bytes_`), so the bodies move down almost verbatim.

**Files:** `ui/shared/app/RoomWindowBase.h` / `.cpp`; the 4 RoomWindow subclasses.

- [ ] **Step 1: Read the canonical Qt6 wiring + one other**

Read `ui/linux-qt/src/RoomWindow.cpp` (the full ctor provider/closure block — canonical) and `ui/linux-gtk/src/RoomWindow.cpp` to confirm the closures differ ONLY by (a) the repaint primitive and (b) macOS/Win32's `text_area_->set_text("")` after send. Note exactly which closures exist (`on_send`, `on_send_reply`, `on_send_edit`, `on_edit_cancelled`, `on_edit_prefill`, `on_reply_focus`, `on_delete_requested`, `on_reaction_toggled`, `on_receipt_needed`, `on_link_clicked`, `on_near_top`, plus avatar/image/preview/voice providers) and the exact preview-provider source (`shell_->url_preview_data_` — reachable since `RoomWindowBase` is `friend class ShellBase` and holds `shell_`).

- [ ] **Step 2: RoomWindowBase.h — add the API + 2 hooks**

In `RoomWindowBase` `protected:`:
```cpp
    // Install all RoomView providers + compose callbacks on `rv` (which the
    // subclass has just created and parented to its surface). Call before
    // finish_init_(). Uses the shared SDK helpers + shell_ caches.
    void wire_room_view_(views::RoomView* rv);

    // The single per-shell repaint primitive (surface->update() /
    // gtk_widget_queue_draw / InvalidateRect / surface->relayout()).
    virtual void surface_repaint_() = 0;

    // Compose text widget to clear after a successful send, or nullptr if the
    // shell clears it another way (Qt6/GTK4 return nullptr; Win32/macOS
    // return their native text area).
    virtual tk::NativeTextArea* compose_text_area_() { return nullptr; }
```
(`tk::NativeTextArea` is declared via `tk/host.h`; add `#include "tk/host.h"` to `RoomWindowBase.h` if not already transitively present.)

- [ ] **Step 3: RoomWindowBase.cpp — implement `wire_room_view_`**

Move the canonical Qt6 closure/provider bodies here verbatim, substituting: every repaint call → `surface_repaint_();`; the post-send native-clear → `if (auto* ta = compose_text_area_()) { ta->set_text(""); }`; the trim → `tesseract::text::trim(...)` (`#include "views/text_util.h"`); avatar/image → `shell_avatar_`/`shell_image_`; voice-bytes → `fetch_source_bytes_`; preview provider → read `shell_->url_preview_data_` (already friend-accessible). Sends route through the existing `send_message_`/`send_reply_`/`send_edit_`/`delete_event_`/`toggle_reaction_`/`send_receipt_`/`request_pagination_back_`. Skeleton (fill each closure body from the canonical Qt6 source read in Step 1 — bodies are identical across shells modulo the two hooked primitives):
```cpp
void RoomWindowBase::wire_room_view_(views::RoomView* rv)
{
    rv->set_avatar_provider(
        [this](const std::string& mxc) { return shell_avatar_(mxc); });
    rv->set_image_provider(
        [this](const std::string& mxc) { return shell_image_(mxc); });
    rv->set_preview_provider(/* read shell_->url_preview_data_ as Qt6 does */);
    rv->set_voice_bytes_provider(
        [this](const std::string& s) { return fetch_source_bytes_(s); });
    rv->set_repaint([this] { surface_repaint_(); });
    rv->on_send = [this](const std::string& body)
    {
        const std::string t = tesseract::text::trim(body);
        if (t.empty()) { return; }
        send_message_(t);
        if (auto* ta = compose_text_area_()) { ta->set_text(""); }
    };
    rv->on_send_reply   = /* … shared body, send_reply_ + optional ta clear */;
    rv->on_send_edit    = /* … send_edit_ … */;
    rv->on_edit_cancelled  = /* … */;
    rv->on_edit_prefill    = /* … */;
    rv->on_reply_focus     = /* … */;
    rv->on_delete_requested  = [this](const std::string& id){ delete_event_(id); };
    rv->on_reaction_toggled  = [this](const std::string& id, const std::string& k){ toggle_reaction_(id, k); };
    rv->on_receipt_needed    = [this](const std::string& id){ send_receipt_(id); };
    rv->on_link_clicked      = /* … shell open-url; if per-shell, add a hook */;
    rv->on_near_top          = [this]{ request_pagination_back_(); };
}
```
**If `on_link_clicked` opens a URL via a per-shell API** (it may), add a `virtual void open_external_url_(const std::string&) = 0;` hook (one line per shell) rather than guessing — match what the canonical shells actually do (read Step 1). Every closure body must be copied from the real Qt6 source, not paraphrased.

- [ ] **Step 4: Shrink each subclass ctor + implement the 2 hooks**

For each of `ui/linux-qt/src/RoomWindow.cpp`, `ui/linux-gtk/src/RoomWindow.cpp`, `ui/windows/src/RoomWindow.cpp`, `ui/macos/src/RoomWindowController.mm`: delete the entire duplicated provider/closure wiring block; the ctor becomes: create native window + surface, build `RoomView` (parent to surface), `wire_room_view_(rv);`, `finish_init_();`. Implement `surface_repaint_()` = that shell's one repaint primitive; implement `compose_text_area_()` = `return text_area_;` for Win32/macOS, default `nullptr` (don't override) for Qt6/GTK4. If Step 3 added `open_external_url_`, implement it per shell (the one native open-url call already present in the deleted block).

- [ ] **Step 5: Build, test (Qt6+GTK4 verifiable), parity-review Win32/macOS, commit**
```bash
# per-task verification block — Qt6 + GTK4 fully exercise secondary windows
clang-format -i ui/shared/app/RoomWindowBase.{h,cpp} ui/linux-qt/src/RoomWindow.cpp ui/linux-gtk/src/RoomWindow.cpp ui/windows/src/RoomWindow.cpp ui/macos/src/RoomWindowController.mm
git add -A ui/shared/app ui/linux-qt ui/linux-gtk ui/windows ui/macos
git commit -m "refactor(rooms): hoist RoomView wiring into RoomWindowBase::wire_room_view_"
```
Expected: zero errors, ctest 323/323, GTK4 clean. **Manual check (Qt6/GTK4):** open a room in a new window, send/reply/edit/delete/react, scroll up (pagination), click a link, click an image — all must behave exactly as before. Win32/macOS: diff-review the shrunk ctor + 2 hooks against the verified Qt6 result (parity-only here).

---

## Task 9: Docs + final review

- [ ] **Step 1: Full verification** (per-task block) — confirm ctest 323/323, cargo 92/92, Qt6+GTK4 clean on the final tree.
- [ ] **Step 2: CHANGES.md** — under `## Unreleased` → `### 2026-05-18`, add:
  `- refactor(shell): de-duplicated ~650 lines across the four shells into ui/shared/ — ShellBase gains build_rows_, a concrete account-prefs/image-packs handler, cached_emoticons_, and secondary-window dispatch helpers; new shared text::trim, UpConnectorCore, ScreenLockState; RoomWindowBase::wire_room_view_ absorbs the per-shell secondary-window RoomView wiring (behaviour-preserving; 323/323 ctest, 92/92 cargo)`
- [ ] **Step 3: STATUS.md** — bump `Last updated` if needed; counts unchanged (no new tests; this is pure refactor verified by the existing suite). Optionally note the shell-thinning under the architecture section.
- [ ] **Step 4: Commit** `docs: record shell de-duplication hoist`.

---

## Task 10: Final whole-branch review

- [ ] Dispatch a holistic reviewer over the full task range. Verify: each hoist is behavior-preserving; the divergence reconciliations were applied exactly as documented (Qt6 account-prefs gate now universal; macOS `RoomListState::Running` restore-gate left untouched; Qt6 `prep_row_media_` size-hint wrapper preserved; macOS ObjC `Event*` boundary intact); no per-shell secondary block or duplicated trim/sanitize/screenlock-state remains (grep); shared APIs consistent across tasks; Win32/macOS edits are faithful parity mirrors of the Qt6/GTK4 result. Fix findings, re-verify (323/323 + 92/92), commit.

---

## Self-Review (author)

- **Spec coverage:** Tasks 1-8 map 1:1 to the eight audit items in the requested order; Task 4 depends on Task 1 (`build_rows_`/`prep_row_media_`) — ordered correctly. Reconciliations called out explicitly (account-prefs gate = Task 2; macOS restore-gate explicitly left = Task 4; Qt6 `ensureRowMedia`→`prep_row_media_` hook preserves the size-hint behavior = Task 1; macOS `Event*` overload = Task 1/Task 6-of-macOS).
- **Placeholder scan:** The only non-verbatim spots are Task 8's closure bodies and Task 6's `normalize_endpoint`, where the plan mandates copying the *actual current* canonical source (read in Step 1 of each) rather than paraphrasing, with explicit equivalence checks — this is required because the source lines drift daily and the duplicated bodies must be lifted verbatim, not reinvented. New shared code (Tasks 1-7 ShellBase/headers) is fully written.
- **Type consistency:** `build_rows_`, `prep_row_media_`, `refresh_pickers_packs_`, `cached_emoticons_`, `dispatch_*_secondary_`, `update_secondary_room_infos_`, `tesseract::text::trim`, `tesseract::up::{sanitize_token,normalize_endpoint,register_push,remove_push}`, `tesseract::screenlock::{State,k*}`, `wire_room_view_`/`surface_repaint_`/`compose_text_area_` are used consistently across tasks.
- **Risk:** Tasks 6 & 7 are fully Linux-verifiable (both Linux shells). Tasks 1-4 & 8 touch Win32/macOS which are parity-only on this host — the plan keeps the macOS ObjC boundary intact and mandates diff-parity review. Behavior-preserving throughout; the existing 323-test suite is the safety net.
