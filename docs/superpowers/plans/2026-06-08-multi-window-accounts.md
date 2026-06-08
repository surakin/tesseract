# Multi-Window Multi-Account Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Support multiple simultaneous main windows (one per account), with Ctrl+click on the account picker spawning a dedicated window and the window registry activating existing ones.

**Architecture:** A new `AccountManager` class (created in `main()`, passed by reference) owns `accounts_` and shared caches; `ShellBase` holds a reference to it and a `shared_ptr<AccountSession>` for its active account. A window registry in `AccountManager` maps user_id → dedicated window; the account picker checks this before switching or spawning.

**Tech Stack:** C++20, `std::shared_ptr`, `std::span`, Catch2/ctest, Qt6/GTK4/Win32/AppKit.

**Spec:** `docs/superpowers/specs/2026-06-08-multi-window-accounts-design.md`

---

## File Structure

### New files
| File | Purpose |
| --- | --- |
| `ui/shared/app/AccountManager.h` | Owns `accounts_`, shared caches, window registry |
| `ui/shared/app/AccountManager.cpp` | Implementation |
| `tests/cpp/test_account_manager.cpp` | Catch2 unit tests — add to `tests/CMakeLists.txt` |

### Phase 1 modified files
| File | Change |
| --- | --- |
| `ui/shared/app/ShellBase.h` | Replace `accounts_`/caches with `AccountManager&` + `shared_ptr<AccountSession> active_account_` |
| `ui/shared/app/ShellBase.cpp` | Update all `accounts_[active_account_index_]`, cache, and logout references |
| `ui/linux-qt/src/MainWindow.h/.cpp` | Constructor takes `AccountManager&`; update `switch_active_account` |
| `ui/linux-qt/src/main.cpp` | Create `AccountManager` before `MainWindow` |
| `ui/linux-gtk/src/MainWindow.h/.cpp` | Same pattern as Qt6 |
| `ui/linux-gtk/src/main.cpp` | Same |
| `ui/windows/src/MainWindow.h/.cpp` | Same |
| `ui/windows/src/main.cpp` | Same |
| `ui/macos/src/MainWindowController.h/.mm` | Same (MacShell composition) |
| `ui/macos/src/AppDelegate.mm` | Same |

### Phase 2 modified files
| File | Change |
| --- | --- |
| `ui/shared/app/AccountManager.h/.cpp` | Add registry methods + `all_windows()` |
| `ui/shared/app/ShellBase.h` | Add `raise_and_activate()` pure-virtual; add `is_ctrl_held_()` pure-virtual |
| `ui/shared/views/AccountPicker.h` | No change (routing logic goes in ShellBase wiring) |
| `ui/linux-qt/src/MainWindow.h/.cpp` | `raise_and_activate()`, `is_ctrl_held_()`, register/unregister, tray menu |
| `ui/linux-qt/src/LinuxQtTrayIcon.h/.cpp` | Per-window menu items |
| `ui/linux-gtk/src/MainWindow.h/.cpp` | Same |
| `ui/linux-gtk/src/GtkSniTrayIcon.h/.cpp` | Same |
| `ui/windows/src/MainWindow.h/.cpp` | Same |
| `ui/macos/src/MainWindowController.h/.mm` | Same |

### Phase 3 modified files
| File | Change |
| --- | --- |
| `ui/shared/app/ShellBase.h/.cpp` | Add `spawn_main_window_()` pure-virtual; update `on_select` routing |
| `ui/linux-qt/src/MainWindow.h/.cpp` | `spawn_main_window_()`, close handler quit-on-last |
| `ui/linux-gtk/src/MainWindow.h/.cpp` | Same |
| `ui/windows/src/MainWindow.h/.cpp` | Same |
| `ui/macos/src/MainWindowController.h/.mm` | Same |
| `client/include/tesseract/settings.h` | Add `user_id` to `PopoutEntry` |
| `ui/shared/app/ShellBase.cpp` | Filter `populate_pending_restore_popouts_` by `active_account_->user_id` |

---

## Phase 1 — Extract AccountManager (pure refactor, no behavior change)

---

### Task 1: Create `AccountManager` header

**Files:**
- Create: `ui/shared/app/AccountManager.h`

- [ ] **Step 1: Write the header**

```cpp
#pragma once
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare types owned or cached here. Use the same headers that
// ShellBase.h currently includes for these types.
#include <tesseract/account_session.h>
#include <tk/pixmap_cache.h>
#include <tk/anim_image_cache.h>
#include <tk/media_disk_cache.h>
#include <tesseract/paths.h>           // tesseract::cache_dir()

#include <chrono>

namespace tesseract {

class AccountManager
{
public:
    AccountManager();
    ~AccountManager();

    // Sessions — ShellBase creates AccountSession objects (because they
    // contain a platform bridge) and hands them over here for storage.
    void add_account(std::shared_ptr<AccountSession> session);
    void remove_account(std::string_view user_id);
    std::shared_ptr<AccountSession> find(std::string_view user_id) const;
    std::span<std::shared_ptr<AccountSession> const> accounts() const;

    // Shared media caches — previously owned by ShellBase.
    tk::PixmapCache& thumbnail_cache() { return thumbnail_cache_; }
    tk::PixmapCache& image_cache()     { return image_cache_; }
    tk::AnimImageCache& anim_cache()   { return anim_cache_; }
    tk::MediaDiskCache& media_disk_cache() { return media_disk_cache_; }

private:
    std::vector<std::shared_ptr<AccountSession>> accounts_;

    tk::PixmapCache    thumbnail_cache_{48u * 1024u * 1024u,
                                        std::chrono::minutes{30}};
    tk::PixmapCache    image_cache_{64u * 1024u * 1024u};
    tk::AnimImageCache anim_cache_;
    tk::MediaDiskCache media_disk_cache_{tesseract::cache_dir() / "media"};
};

} // namespace tesseract
```

- [ ] **Step 2: Build to verify header compiles**

```bash
cmake --preset linux-qt6-debug
cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | head -30
```

Expected: only errors about `AccountManager.cpp` not existing yet (missing symbol), not header parse errors.

---

### Task 2: Implement `AccountManager.cpp`

**Files:**
- Create: `ui/shared/app/AccountManager.cpp`
- Modify: `ui/shared/app/CMakeLists.txt` — add `AccountManager.cpp` to `tesseract_shared` (or wherever ShellBase.cpp is listed; grep for `ShellBase.cpp` in `CMakeLists.txt` files to find the right one)

- [ ] **Step 1: Find the CMakeLists.txt that lists ShellBase.cpp**

```bash
grep -rn "ShellBase.cpp" /home/rayden/src/tesseract --include="CMakeLists.txt"
```

- [ ] **Step 2: Write `AccountManager.cpp`**

```cpp
#include "AccountManager.h"
#include <algorithm>

namespace tesseract {

AccountManager::AccountManager()  = default;
AccountManager::~AccountManager() = default;

void AccountManager::add_account(std::shared_ptr<AccountSession> session)
{
    accounts_.push_back(std::move(session));
}

void AccountManager::remove_account(std::string_view user_id)
{
    accounts_.erase(
        std::remove_if(accounts_.begin(), accounts_.end(),
                       [&](const auto& s) { return s->user_id == user_id; }),
        accounts_.end());
}

std::shared_ptr<AccountSession> AccountManager::find(std::string_view user_id) const
{
    for (const auto& s : accounts_)
        if (s->user_id == user_id)
            return s;
    return nullptr;
}

std::span<std::shared_ptr<AccountSession> const> AccountManager::accounts() const
{
    return accounts_;
}

} // namespace tesseract
```

- [ ] **Step 3: Add `AccountManager.cpp` to the right CMakeLists.txt**

In the file found in Step 1, add `AccountManager.cpp` next to `ShellBase.cpp` in the source list.

- [ ] **Step 4: Build**

```bash
cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | head -30
```

Expected: `AccountManager.cpp` compiles cleanly; any remaining errors are in files that still reference the old `accounts_` members of `ShellBase`.

---

### Task 3: Write `AccountManager` unit tests

**Files:**
- Create: `tests/cpp/test_account_manager.cpp`
- Modify: `tests/CMakeLists.txt` — add `cpp/test_account_manager.cpp` to the `add_executable(tesseract_tests ...)` source list

- [ ] **Step 1: Write the test file**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "app/AccountManager.h"
#include <tesseract/account_session.h>

using tesseract::AccountManager;
using tesseract::AccountSession;

namespace
{
std::shared_ptr<AccountSession> make_session(std::string user_id,
                                              std::string display_name = "")
{
    auto s          = std::make_shared<AccountSession>();
    s->user_id      = std::move(user_id);
    s->display_name = std::move(display_name);
    return s;
}
} // namespace

TEST_CASE("AccountManager - add and find", "[account_manager]")
{
    AccountManager mgr;
    mgr.add_account(make_session("@alice:example.org", "Alice"));

    auto found = mgr.find("@alice:example.org");
    REQUIRE(found != nullptr);
    CHECK(found->display_name == "Alice");
}

TEST_CASE("AccountManager - find unknown returns nullptr", "[account_manager]")
{
    AccountManager mgr;
    CHECK(mgr.find("@nobody:example.org") == nullptr);
}

TEST_CASE("AccountManager - remove account", "[account_manager]")
{
    AccountManager mgr;
    mgr.add_account(make_session("@alice:example.org"));
    mgr.remove_account("@alice:example.org");

    CHECK(mgr.find("@alice:example.org") == nullptr);
    CHECK(mgr.accounts().empty());
}

TEST_CASE("AccountManager - accounts span contains all added sessions",
          "[account_manager]")
{
    AccountManager mgr;
    mgr.add_account(make_session("@alice:example.org"));
    mgr.add_account(make_session("@bob:matrix.org"));

    REQUIRE(mgr.accounts().size() == 2);
    CHECK(mgr.accounts()[0]->user_id == "@alice:example.org");
    CHECK(mgr.accounts()[1]->user_id == "@bob:matrix.org");
}

TEST_CASE("AccountManager - remove non-existent is a no-op", "[account_manager]")
{
    AccountManager mgr;
    mgr.add_account(make_session("@alice:example.org"));
    mgr.remove_account("@nobody:example.org");  // must not crash or remove alice

    REQUIRE(mgr.accounts().size() == 1);
}

TEST_CASE("AccountManager - shared_ptr identity preserved", "[account_manager]")
{
    AccountManager mgr;
    auto session = make_session("@alice:example.org");
    mgr.add_account(session);

    // find() returns the same shared_ptr, not a copy of the object.
    CHECK(mgr.find("@alice:example.org").get() == session.get());
}
```

- [ ] **Step 2: Add to `tests/CMakeLists.txt`**

In the `add_executable(tesseract_tests ...)` block, add:
```cmake
cpp/test_account_manager.cpp
```

- [ ] **Step 3: Run the failing tests**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
ctest --test-dir build/linux-qt6-debug -R account_manager --output-on-failure
```

Expected: all 6 AccountManager tests pass (AccountManager is independent of the platform UI).

---

### Task 4: Update `ShellBase.h` — swap members

**Files:**
- Modify: `ui/shared/app/ShellBase.h`

The goal is a pure header change: `accounts_` and caches move out; `AccountManager&` and `active_account_` move in. No behavior changes yet.

- [ ] **Step 1: Add `AccountManager` include at the top of `ShellBase.h`**

Find the block of includes near the top of `ShellBase.h` and add:
```cpp
#include "AccountManager.h"
```

- [ ] **Step 2: Replace the `accounts_` and cache members (around line 233–240 and 336–340)**

Remove:
```cpp
std::vector<std::unique_ptr<AccountSession>> accounts_;
int active_account_index_ = -1;
```

Add in the `// ── Multi-account ──` block:
```cpp
AccountManager& account_manager_;     // non-owning; lives in main()
std::shared_ptr<AccountSession> active_account_; // null when no account active
```

Remove from the cache block (lines 336–340):
```cpp
tk::PixmapCache thumbnail_cache_{48u * 1024u * 1024u,
                                  std::chrono::minutes{30}};
tk::PixmapCache image_cache_{64u * 1024u * 1024u};
tk::AnimImageCache anim_cache_;
tk::MediaDiskCache media_disk_cache_{tesseract::cache_dir() / "media"};
bool media_disk_cache_pruned_ = false;
```

Keep `media_disk_cache_pruned_` as a plain `bool` member (it tracks a ShellBase-level state, not the cache itself):
```cpp
bool media_disk_cache_pruned_ = false;
```

- [ ] **Step 3: Update the `ShellBase` constructor declaration to accept `AccountManager&`**

Find the constructor declaration in `ShellBase.h`. The constructor currently takes several parameters (default_w, default_h, etc. — look at lines ~65–115 in ShellBase.cpp). Add `AccountManager& account_manager` as the first parameter:

```cpp
// Before (example; adjust to actual signature):
explicit ShellBase(int default_w, int default_h, ...);

// After:
explicit ShellBase(AccountManager& account_manager, int default_w, int default_h, ...);
```

- [ ] **Step 4: Remove `tk/pixmap_cache.h`, `tk/anim_image_cache.h`, `tk/media_disk_cache.h` from ShellBase.h includes IF they are not used elsewhere in the header**

Check: `grep -n "PixmapCache\|AnimImageCache\|MediaDiskCache" ui/shared/app/ShellBase.h`. If the types still appear (e.g. in inline methods or other member declarations), keep the includes. Otherwise remove them — AccountManager.h now brings them in.

---

### Task 5: Update `ShellBase.cpp` — mechanical cache and account references

**Files:**
- Modify: `ui/shared/app/ShellBase.cpp`

This is the largest mechanical change. Every reference to `thumbnail_cache_`, `image_cache_`, `anim_cache_`, `media_disk_cache_` becomes `account_manager_.thumbnail_cache()`, etc. Every reference to `accounts_[active_account_index_]` becomes `*active_account_`. The `active_account_index_` field is gone.

- [ ] **Step 1: Update the constructor to initialise `account_manager_`**

Find the `ShellBase::ShellBase(...)` constructor (around line 65 in ShellBase.cpp). Add `account_manager` parameter and member-initialise it:

```cpp
// Add to constructor parameter list:
AccountManager& account_manager,

// Add to member initialiser list (reference members must be first):
account_manager_(account_manager),
```

Remove the in-class initialisers for the four caches that were in the header (they are now in `AccountManager`).

- [ ] **Step 2: Replace cache member accesses throughout ShellBase.cpp**

Run these replacements throughout the file (every occurrence):
```
thumbnail_cache_  →  account_manager_.thumbnail_cache()
image_cache_      →  account_manager_.image_cache()
anim_cache_       →  account_manager_.anim_cache()
media_disk_cache_ →  account_manager_.media_disk_cache()
```

Verify count of replacements by running:
```bash
grep -c "thumbnail_cache_\|image_cache_\|anim_cache_\|media_disk_cache_" \
    ui/shared/app/ShellBase.cpp
```

Expected: 0 matches after replacements.

- [ ] **Step 3: Replace `accounts_[active_account_index_]` accesses**

Every place that dereferences the active account now uses `active_account_`. Common patterns to update:

```cpp
// Before:
accounts_[active_account_index_].client.get()
accounts_[active_account_index_]->user_id
accounts_[active_account_index_].display_name

// After:
active_account_->client.get()
active_account_->user_id
active_account_->display_name
```

For places that iterate all accounts (`for (auto& acc : accounts_)`), replace with:
```cpp
for (const auto& acc : account_manager_.accounts())
```

- [ ] **Step 4: Replace `accounts_.push_back` / `accounts_.emplace_back` (login / restore paths)**

Find anywhere a new `AccountSession` is created and added:
```bash
grep -n "accounts_\.push_back\|accounts_\.emplace\|make_unique<AccountSession\|AccountSession{" \
    ui/shared/app/ShellBase.cpp
```

Replace each with `account_manager_.add_account(std::move(session))` where `session` is a `std::shared_ptr<AccountSession>`. Update the allocation from `std::make_unique` to `std::make_shared`.

- [ ] **Step 5: Replace `accounts_.erase` (logout paths)**

Find `accounts_.erase` (near line 3292 comment). Replace with:
```cpp
account_manager_.remove_account(active_account_->user_id);
active_account_.reset();
```

- [ ] **Step 6: Replace remaining `active_account_index_` references**

```bash
grep -n "active_account_index_" ui/shared/app/ShellBase.cpp
```

For any remaining index-based accesses, convert to use `active_account_` directly or `account_manager_.find(uid)`.

- [ ] **Step 7: Build and check for errors**

```bash
cmake --build build/linux-qt6-debug 2>&1 | grep "error:" | head -40
```

Work through remaining errors one by one. Most will be in platform shells that still pass the old constructor signature or access members that no longer exist.

---

### Task 6: Update Qt6 `MainWindow` constructor

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h`
- Modify: `ui/linux-qt/src/MainWindow.cpp`

- [ ] **Step 1: Update `MainWindow.h` constructor declaration**

```cpp
// Before:
explicit MainWindow(QWidget* parent = nullptr);

// After:
explicit MainWindow(tesseract::AccountManager& account_manager,
                    QWidget* parent = nullptr);
```

Add `#include "app/AccountManager.h"` (or forward-declare via a relative path) in `MainWindow.h`.

- [ ] **Step 2: Update `MainWindow.cpp` constructor definition**

```cpp
// Before:
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ShellBase(...)

// After:
MainWindow::MainWindow(tesseract::AccountManager& account_manager, QWidget* parent)
    : QMainWindow(parent)
    , ShellBase(account_manager, ...)
```

- [ ] **Step 3: Update `switch_active_account`**

Find `switch_active_account` in `MainWindow.cpp`. It currently takes an `int new_idx` and looks up `accounts_[new_idx]`. Change it to look up by user_id through `account_manager_`:

```cpp
void MainWindow::switch_active_account_(const std::string& user_id)
{
    auto session = account_manager_.find(user_id);
    if (!session || session == active_account_)
        return;
    // ... rest of the existing logic, replacing accounts_[new_idx] with *session ...
    active_account_ = session;
    client_         = session->client.get();
    event_handler_  = session->bridge.get();
    // ... continue existing rebind/repaint logic ...
}
```

Also update any call sites within `MainWindow.cpp` that pass an integer index; instead look up the session by user_id.

---

### Task 7: Update GTK4, Win32, macOS `MainWindow` constructors

**Files:**
- Modify: `ui/linux-gtk/src/MainWindow.h/.cpp`
- Modify: `ui/windows/src/MainWindow.h/.cpp`
- Modify: `ui/macos/src/MainWindowController.h/.mm`

- [ ] **Step 1: Apply the same constructor-signature change as Task 6 to each platform shell**

For each platform, add `tesseract::AccountManager& account_manager` as the first parameter to the shell constructor and forward it to `ShellBase(account_manager, ...)`.

For macOS, `MacShell` is nested inside `MainWindowController` via composition. Add `AccountManager&` to `MacShell`'s constructor and forward to `ShellBase`. Update `MainWindowController` to hold and pass the reference.

- [ ] **Step 2: Update `switch_active_account` on each platform**

Apply the same `find(user_id)` → `shared_ptr<AccountSession>` pattern from Task 6 to all three remaining platforms.

- [ ] **Step 3: Build all platforms (or at least Linux Qt6 + Linux GTK)**

```bash
cmake --build build/linux-qt6-debug 2>&1 | grep "error:" | head -20
```

Fix any errors before proceeding to the platform `main()` updates.

---

### Task 8: Update all four platform `main()` functions

**Files:**
- Modify: `ui/linux-qt/src/main.cpp`
- Modify: `ui/linux-gtk/src/main.cpp`
- Modify: `ui/windows/src/main.cpp` (or `WinMain.cpp`)
- Modify: `ui/macos/src/AppDelegate.mm`

- [ ] **Step 1: Qt6 `main.cpp`**

```cpp
// Before:
qt6::MainWindow window;
window.show();

// After:
tesseract::AccountManager account_manager;
qt6::MainWindow window{account_manager};
window.show();
```

Add `#include "app/AccountManager.h"` at the top.

- [ ] **Step 2: GTK4 `main.cpp`**

Find where `gtk4::MainWindow` is constructed (inside the `activate` signal handler lambda). Add `AccountManager` before it:

```cpp
// In the activate lambda capture or surrounding scope:
tesseract::AccountManager account_manager;
// ...
window = std::make_unique<gtk4::MainWindow>(account_manager);
```

Because the GTK `activate` signal may fire inside `g_application_run`, `AccountManager` must either be declared before the `g_application_run` call or captured in a lambda. Check the current GTK `main.cpp` structure and place it accordingly so it outlives the `MainWindow`.

- [ ] **Step 3: Win32 `main.cpp`**

```cpp
// Before:
win32::MainWindow window;

// After:
tesseract::AccountManager account_manager;
win32::MainWindow window{account_manager};
```

- [ ] **Step 4: macOS `AppDelegate.mm`**

`AppDelegate` creates (or owns) `MainWindowController`. Add `AccountManager` as a member of `AppDelegate`:

```objc
// AppDelegate.h or AppDelegate.mm:
tesseract::AccountManager account_manager_;

// In applicationDidFinishLaunching: or equivalent:
controller_ = [[MainWindowController alloc] initWithAccountManager:account_manager_];
```

Update `MainWindowController`'s designated initialiser to accept an `AccountManager&`.

- [ ] **Step 5: Build and run all tests**

```bash
cmake --build build/linux-qt6-debug
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

Expected: **all existing tests pass**. No behavior change — this was a pure refactor.

- [ ] **Step 6: Commit Phase 1**

```bash
git add ui/shared/app/AccountManager.h \
        ui/shared/app/AccountManager.cpp \
        ui/shared/app/ShellBase.h \
        ui/shared/app/ShellBase.cpp \
        ui/linux-qt/src/MainWindow.h \
        ui/linux-qt/src/MainWindow.cpp \
        ui/linux-qt/src/main.cpp \
        ui/linux-gtk/src/MainWindow.h \
        ui/linux-gtk/src/MainWindow.cpp \
        ui/linux-gtk/src/main.cpp \
        ui/windows/src/MainWindow.h \
        ui/windows/src/MainWindow.cpp \
        ui/windows/src/main.cpp \
        ui/macos/src/MainWindowController.h \
        ui/macos/src/MainWindowController.mm \
        ui/macos/src/AppDelegate.mm \
        tests/cpp/test_account_manager.cpp \
        tests/CMakeLists.txt
git commit -m "refactor: extract AccountManager from ShellBase (phase 1)"
```

---

## Phase 2 — Window Registry + Picker Behavior + Systray

---

### Task 9: Add window registry to `AccountManager`

**Files:**
- Modify: `ui/shared/app/AccountManager.h`
- Modify: `ui/shared/app/AccountManager.cpp`

- [ ] **Step 1: Add registry declarations to `AccountManager.h`**

Add forward declaration before the class:
```cpp
class ShellBase;
```

Add to the `public:` section:
```cpp
// Window registry
void register_window(ShellBase* w);
void unregister_window(ShellBase* w);
void set_dedicated(std::string_view user_id, ShellBase* w);
void clear_dedicated(std::string_view user_id);
ShellBase* dedicated_window(std::string_view user_id) const;
int window_count() const;
std::span<ShellBase* const> all_windows() const;
```

Add to `private:`:
```cpp
#include <unordered_map>  // already at top of header — verify it's there
// ...
std::vector<ShellBase*> all_windows_;
std::unordered_map<std::string, ShellBase*> dedicated_windows_;
```

- [ ] **Step 2: Implement registry methods in `AccountManager.cpp`**

```cpp
void AccountManager::register_window(ShellBase* w)
{
    all_windows_.push_back(w);
}

void AccountManager::unregister_window(ShellBase* w)
{
    all_windows_.erase(
        std::remove(all_windows_.begin(), all_windows_.end(), w),
        all_windows_.end());
}

void AccountManager::set_dedicated(std::string_view user_id, ShellBase* w)
{
    dedicated_windows_[std::string(user_id)] = w;
}

void AccountManager::clear_dedicated(std::string_view user_id)
{
    dedicated_windows_.erase(std::string(user_id));
}

ShellBase* AccountManager::dedicated_window(std::string_view user_id) const
{
    auto it = dedicated_windows_.find(std::string(user_id));
    return it != dedicated_windows_.end() ? it->second : nullptr;
}

int AccountManager::window_count() const
{
    return static_cast<int>(all_windows_.size());
}

std::span<ShellBase* const> AccountManager::all_windows() const
{
    return all_windows_;
}
```

---

### Task 10: Write window registry tests

**Files:**
- Modify: `tests/cpp/test_account_manager.cpp`

- [ ] **Step 1: Add registry tests to the existing test file**

Because `ShellBase` requires platform UI, use a minimal stub:

```cpp
// At the top of test_account_manager.cpp, add after existing includes:
#include "app/ShellBase.h"   // for pointer type only in registry tests

// Use a forward-declared fake ShellBase* — we never dereference it in registry tests.
// Cast a local int address to ShellBase* so the pointer is unique and non-null.
namespace
{
ShellBase* fake_window(int& tag) { return reinterpret_cast<ShellBase*>(&tag); }
} // namespace

TEST_CASE("AccountManager registry - window_count", "[account_manager][registry]")
{
    AccountManager mgr;
    CHECK(mgr.window_count() == 0);

    int t1, t2;
    mgr.register_window(fake_window(t1));
    CHECK(mgr.window_count() == 1);

    mgr.register_window(fake_window(t2));
    CHECK(mgr.window_count() == 2);

    mgr.unregister_window(fake_window(t1));
    CHECK(mgr.window_count() == 1);
}

TEST_CASE("AccountManager registry - dedicated_window round-trip",
          "[account_manager][registry]")
{
    AccountManager mgr;
    int t1;
    ShellBase* w = fake_window(t1);

    CHECK(mgr.dedicated_window("@alice:example.org") == nullptr);
    mgr.set_dedicated("@alice:example.org", w);
    CHECK(mgr.dedicated_window("@alice:example.org") == w);
    mgr.clear_dedicated("@alice:example.org");
    CHECK(mgr.dedicated_window("@alice:example.org") == nullptr);
}

TEST_CASE("AccountManager registry - all_windows span", "[account_manager][registry]")
{
    AccountManager mgr;
    int t1, t2;
    mgr.register_window(fake_window(t1));
    mgr.register_window(fake_window(t2));

    REQUIRE(mgr.all_windows().size() == 2);
    CHECK(mgr.all_windows()[0] == fake_window(t1));
    CHECK(mgr.all_windows()[1] == fake_window(t2));
}

TEST_CASE("AccountManager registry - unregister_window is idempotent for unknown ptr",
          "[account_manager][registry]")
{
    AccountManager mgr;
    int t1;
    mgr.unregister_window(fake_window(t1));  // must not crash
    CHECK(mgr.window_count() == 0);
}
```

- [ ] **Step 2: Run the new tests**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests
ctest --test-dir build/linux-qt6-debug -R account_manager --output-on-failure
```

Expected: all registry tests pass.

---

### Task 11: Add `raise_and_activate_()` and `is_ctrl_held_()` to `ShellBase`; centralise picker routing

**Files:**
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`
- Modify: `ui/linux-qt/src/MainWindow.cpp` (~line 4869, `on_select` wiring)
- Modify: `ui/linux-gtk/src/MainWindow.cpp` (~line 6371)
- Modify: `ui/windows/src/MainWindow.cpp` (~line 6053)
- Modify: macOS shell equivalent

**Background:** `on_select` is wired in each platform's `openAccountPicker()` (or equivalent), calling a per-platform method like `onAccountSelected(uid)`. We add a shared `on_account_picker_select_` method to `ShellBase` so all routing logic lives in one place.

- [ ] **Step 1: Add two new pure-virtuals to `ShellBase.h`**

```cpp
// public — called by picker routing on *other* window instances:
virtual void raise_and_activate_() = 0;

// protected — only called by ShellBase's own on_account_picker_select_:
virtual bool is_ctrl_held_() const = 0;
```

- [ ] **Step 2: Add `on_account_picker_select_` to `ShellBase.h`** (protected, non-virtual)

```cpp
// Each platform's openAccountPicker wires:
//   picker->on_select = [this](const std::string& uid) { on_account_picker_select_(uid); };
// Ctrl routing is added in Phase 3 when spawn_main_window_ exists.
void on_account_picker_select_(const std::string& uid);
```

- [ ] **Step 3: Implement `on_account_picker_select_` in `ShellBase.cpp`**

```cpp
void ShellBase::on_account_picker_select_(const std::string& uid)
{
    if (auto* win = account_manager_.dedicated_window(uid))
    {
        win->raise_and_activate_();
        return;
    }
    // No dedicated window — switch this window to the account.
    switch_active_account_(uid);
}
```

(Ctrl+click routing is added in Phase 3, Task 15.)

- [ ] **Step 4: Update each platform's `on_select` wiring to delegate to `on_account_picker_select_`**

Qt6 `MainWindow.cpp` (~line 4869):
```cpp
// Before:
accountPicker_->on_select = [this](const std::string& uid)
{
    onAccountSelected(uid);
};

// After:
accountPicker_->on_select = [this](const std::string& uid)
{
    on_account_picker_select_(uid);
};
```

Apply the same one-line change to the equivalent wiring in `ui/linux-gtk/src/MainWindow.cpp` (~line 6371), `ui/windows/src/MainWindow.cpp` (~line 6053), and the macOS shell.

- [ ] **Step 5: Fix `accounts_.size()` reference in each platform's picker-open method**

Qt6 `openAccountPicker` (~line 4880):
```cpp
// Before:
const int rows = static_cast<int>(accounts_.size());

// After:
const int rows = static_cast<int>(account_manager_.accounts().size());
```

Apply the same fix to GTK4, Win32, and macOS equivalents.

---

### Task 12: Implement `raise_and_activate_()` and `is_ctrl_held_()` on all four platforms

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h/.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.h/.cpp`
- Modify: `ui/windows/src/MainWindow.h/.cpp`
- Modify: `ui/macos/src/MainWindowController.h/.mm`

- [ ] **Step 1: Qt6**

In `MainWindow.h`, add the overrides:
```cpp
void raise_and_activate_() override;
bool is_ctrl_held_() const override;
```

In `MainWindow.cpp`:
```cpp
void MainWindow::raise_and_activate_()
{
    show();
    raise();
    activateWindow();
}

bool MainWindow::is_ctrl_held_() const
{
    return QApplication::keyboardModifiers() & Qt::ControlModifier;
}
```

- [ ] **Step 2: GTK4**

In `MainWindow.h`:
```cpp
void raise_and_activate_() override;
bool is_ctrl_held_() const override;
```

In `MainWindow.cpp`:
```cpp
void MainWindow::raise_and_activate_()
{
    gtk_window_present(GTK_WINDOW(window_));
}

bool MainWindow::is_ctrl_held_() const
{
    GdkDisplay* display = gdk_display_get_default();
    GdkSeat*    seat    = gdk_display_get_default_seat(display);
    GdkDevice*  kbd     = gdk_seat_get_keyboard(seat);
    GdkModifierType mods{};
    gdk_device_get_modifier_state(kbd, &mods);
    return (mods & GDK_CONTROL_MASK) != 0;
}
```

- [ ] **Step 3: Win32**

In `MainWindow.h`:
```cpp
void raise_and_activate_() override;
bool is_ctrl_held_() const override;
```

In `MainWindow.cpp`:
```cpp
void MainWindow::raise_and_activate_()
{
    if (IsIconic(hwnd_))
        ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

bool MainWindow::is_ctrl_held_() const
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}
```

- [ ] **Step 4: macOS**

In `MainWindowController.mm` (via `MacShell`):
```objc
// MacShell::raise_and_activate_() — called on UI thread
void MacShell::raise_and_activate_()
{
    [NSApp activateIgnoringOtherApps:YES];
    [controller_.window makeKeyAndOrderFront:nil];
}

bool MacShell::is_ctrl_held_() const
{
    // macOS uses Command (⌘) for keyboard shortcuts by convention.
    NSEventModifierFlags flags = [NSApp currentEvent].modifierFlags;
    return (flags & NSEventModifierFlagCommand) != 0;
}
```

---

### Task 13: Register/unregister the initial window

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`
- Modify: `ui/windows/src/MainWindow.cpp`
- Modify: `ui/macos/src/MainWindowController.mm`

Each platform shell registers itself during construction and unregisters during destruction.

- [ ] **Step 1: Qt6 — in `MainWindow` constructor and destructor**

```cpp
// Constructor (after ShellBase is fully constructed):
account_manager_.register_window(this);

// Destructor:
MainWindow::~MainWindow()
{
    account_manager_.unregister_window(this);
}
```

- [ ] **Step 2: GTK4, Win32, macOS — same pattern**

Apply the `register_window(this)` call in each platform's constructor body (after `ShellBase` init) and `unregister_window(this)` in the destructor (or the equivalent teardown point — for GTK4 this may be the `destroy` signal handler rather than the C++ destructor).

---

### Task 14: Update systray menus to list all open windows

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.cpp` + `LinuxQtTrayIcon.h/.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.cpp` + `GtkSniTrayIcon.h/.cpp`
- Modify: `ui/windows/src/MainWindow.cpp`
- Modify: `ui/macos/src/MainWindowController.mm`

The tray menu needs a menu item per open window. The tray is rebuilt on demand by each shell. The `AccountManager::all_windows()` span gives the list.

- [ ] **Step 1: Qt6 tray menu**

Find the method in `MainWindow.cpp` (or `LinuxQtTrayIcon.cpp`) that builds the `QMenu` for the tray icon. Before the separator that precedes "Quit", insert one action per window:

```cpp
// Insert before the Quit action:
for (ShellBase* shell : account_manager_.all_windows())
{
    const std::string label =
        shell->active_account()
            ? shell->active_account()->display_name + " (" +
              shell->active_account()->user_id + ")"
            : "Tesseract";
    auto* action = menu->addAction(QString::fromStdString(label));
    connect(action, &QAction::triggered, this, [shell]
    {
        shell->raise_and_activate_();
    });
}
menu->addSeparator();
```

Add a public getter to `ShellBase`:
```cpp
// ShellBase.h public section:
std::shared_ptr<AccountSession> active_account() const { return active_account_; }
```

Rebuild the tray menu whenever `register_window` or `unregister_window` is called. You can do this by calling the existing tray-rebuild method from within `register_window`/`unregister_window`, or by having `ShellBase` call a `rebuild_tray_()` virtual after registration (see below).

- [ ] **Step 2: Add `rebuild_tray_()` hook to `ShellBase`**

In `ShellBase.h`, add a virtual no-op that each platform overrides to rebuild its tray menu:
```cpp
virtual void rebuild_tray_() {}
```

In `AccountManager`, after `register_window` / `unregister_window`, notify all windows:
```cpp
void AccountManager::register_window(ShellBase* w)
{
    all_windows_.push_back(w);
    for (ShellBase* win : all_windows_) win->rebuild_tray_();
}

void AccountManager::unregister_window(ShellBase* w)
{
    all_windows_.erase(...);
    for (ShellBase* win : all_windows_) win->rebuild_tray_();
}
```

- [ ] **Step 3: Qt6 — implement `rebuild_tray_()`**

```cpp
void MainWindow::rebuild_tray_()
{
    if (tray_)
        tray_->rebuild_menu();  // call whatever method builds the QMenu
}
```

Implement the menu-building logic (window items + separator + Quit) inside `tray_->rebuild_menu()` (or inline in `MainWindow::rebuild_tray_()`).

- [ ] **Step 4: GTK4, Win32, macOS — implement `rebuild_tray_()`**

Apply the same rebuild pattern on each platform, iterating `account_manager_.all_windows()` and building one menu item per window.

- [ ] **Step 5: Build and run all tests**

```bash
cmake --build build/linux-qt6-debug
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 6: Commit Phase 2**

```bash
git add -u
git commit -m "feat: window registry, picker activate-or-switch, per-window tray items (phase 2)"
```

---

## Phase 3 — New Window Spawning + Quit Behavior

---

### Task 15: Add `spawn_main_window_()` pure-virtual to `ShellBase`

**Files:**
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`

- [ ] **Step 1: Add pure-virtual to `ShellBase.h`**

```cpp
// Constructs and shows a new platform main window bound to `account`.
// The new window registers itself with account_manager_ in its constructor.
virtual void spawn_main_window_(std::shared_ptr<AccountSession> account) = 0;
```

- [ ] **Step 2: Update `on_account_picker_select_` in `ShellBase.cpp` to handle Ctrl**

Find the implementation added in Task 11 Step 3 and replace it with the full routing:

```cpp
void ShellBase::on_account_picker_select_(const std::string& uid)
{
    const bool ctrl = is_ctrl_held_();

    if (auto* win = account_manager_.dedicated_window(uid))
    {
        // Dedicated window already exists — always just raise it,
        // even on Ctrl+click (don't spawn a second window).
        win->raise_and_activate_();
        return;
    }

    if (ctrl)
    {
        // Ctrl+click, no dedicated window — spawn one.
        auto session = account_manager_.find(uid);
        if (session)
            spawn_main_window_(session);
    }
    else
    {
        // Regular click, no dedicated window — switch this window.
        switch_active_account_(uid);
    }
}
```

---

### Task 16: Implement `spawn_main_window_()` on all four platforms

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h/.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.h/.cpp`
- Modify: `ui/windows/src/MainWindow.h/.cpp`
- Modify: `ui/macos/src/MainWindowController.h/.mm`

Each implementation constructs a new native window, sets its active account, registers it as dedicated, and shows it.

- [ ] **Step 1: Qt6**

In `MainWindow.h`:
```cpp
void spawn_main_window_(std::shared_ptr<tesseract::AccountSession> account) override;
```

In `MainWindow.cpp`:
```cpp
void MainWindow::spawn_main_window_(std::shared_ptr<tesseract::AccountSession> account)
{
    auto* w = new qt6::MainWindow{account_manager_};
    w->set_initial_account(std::move(account));  // see below
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->show();
    w->raise_and_activate_();
}
```

Add a `set_initial_account(shared_ptr<AccountSession>)` method to `ShellBase` (non-virtual, calls `switch_active_account_` internally with the given session set as `active_account_`). This is needed because the new window starts with no account; we want to bind it to the spawned account before showing.

```cpp
// ShellBase.h:
void set_initial_account(std::shared_ptr<AccountSession> session);

// ShellBase.cpp:
void ShellBase::set_initial_account(std::shared_ptr<AccountSession> session)
{
    active_account_ = std::move(session);
    client_         = active_account_->client.get();
    event_handler_  = active_account_->bridge.get();
    account_manager_.set_dedicated(active_account_->user_id, this);
    // Trigger a rooms/UI refresh identical to the account-switch path:
    on_switch_account_ui_();   // call the existing post-switch hook
}
```

- [ ] **Step 2: GTK4**

```cpp
void MainWindow::spawn_main_window_(std::shared_ptr<tesseract::AccountSession> account)
{
    auto w = std::make_unique<gtk4::MainWindow>(account_manager_);
    w->set_initial_account(std::move(account));
    w->present();
    // GTK4 windows manage their own lifetime; if using a smart-ptr vector,
    // transfer ownership to a global list or let GTK ref-count manage it.
    // If using raw GtkWindow*, the window destroys itself on close via
    // gtk_window_set_destroy_with_parent or the delete-event handler.
    w.release();  // GTK ref-count owns the window hereafter
}
```

- [ ] **Step 3: Win32**

```cpp
void MainWindow::spawn_main_window_(std::shared_ptr<tesseract::AccountSession> account)
{
    auto* w = new win32::MainWindow{account_manager_};
    w->set_initial_account(std::move(account));
    ShowWindow(w->hwnd(), SW_SHOW);
    SetForegroundWindow(w->hwnd());
    // Win32 window destroys itself (delete this) in WM_NCDESTROY handler.
}
```

- [ ] **Step 4: macOS**

```objc
void MacShell::spawn_main_window_(std::shared_ptr<tesseract::AccountSession> account)
{
    MainWindowController* ctrl =
        [[MainWindowController alloc] initWithAccountManager:account_manager_];
    [ctrl.shell set_initial_account:std::move(account)];
    [ctrl showWindow:nil];
    [ctrl.window makeKeyAndOrderFront:nil];
    // ARC manages lifetime; NSWindowController is retained by the window.
}
```

---

### Task 17: Update platform close handlers (quit-on-last + clear-dedicated guard)

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`
- Modify: `ui/windows/src/MainWindow.cpp`
- Modify: `ui/macos/src/MainWindowController.mm`

- [ ] **Step 1: Qt6 `closeEvent`**

The current `closeEvent` (line 2788) hides to tray if tray is available. Update it:

```cpp
void MainWindow::closeEvent(QCloseEvent* ev)
{
    if (tray_ && tray_->is_available() && account_manager_.window_count() == 1)
    {
        // Only hide to tray when this is the sole window.
        // (With multiple windows the tray persists anyway.)
        ev->ignore();
        hide();
        return;
    }
    // Real close: clear dedicated registration, unregister, maybe quit.
    if (active_account_ &&
        account_manager_.dedicated_window(active_account_->user_id) == this)
    {
        account_manager_.clear_dedicated(active_account_->user_id);
    }
    account_manager_.unregister_window(this);  // called before QMainWindow::closeEvent
    if (account_manager_.window_count() == 0)
    {
        QApplication::quit();
    }
    QMainWindow::closeEvent(ev);
}
```

> **Note:** `account_manager_.unregister_window(this)` is also called in the destructor (Task 13). Guard against double-unregister by checking `all_windows_` contains `this` before erasing (the `remove` + `erase` idiom is already idempotent).

- [ ] **Step 2: GTK4 close handler**

Find the `delete-event` or `close-request` signal handler. Apply the same pattern:

```cpp
// In the close-request handler (returns true to suppress close, false to allow):
bool MainWindow::on_close_request_()
{
    if (active_account_ &&
        account_manager_.dedicated_window(active_account_->user_id) == this)
    {
        account_manager_.clear_dedicated(active_account_->user_id);
    }
    account_manager_.unregister_window(this);
    if (account_manager_.window_count() == 0)
    {
        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window_))));
    }
    return false;  // allow the window to close
}
```

- [ ] **Step 3: Win32 `WM_CLOSE` handler**

In the `WndProc` (or message map), update the `WM_CLOSE` handler:

```cpp
case WM_CLOSE:
{
    if (active_account_ &&
        account_manager_.dedicated_window(active_account_->user_id) == this)
    {
        account_manager_.clear_dedicated(active_account_->user_id);
    }
    account_manager_.unregister_window(this);
    if (account_manager_.window_count() == 0)
        PostQuitMessage(0);
    else
        DestroyWindow(hwnd_);
    return 0;
}
```

- [ ] **Step 4: macOS `windowWillClose:`**

```objc
- (void)windowWillClose:(NSNotification*)notification
{
    auto& shell  = *controller_.shell;
    auto& mgr    = shell.account_manager();
    auto  active = shell.active_account();
    if (active && mgr.dedicated_window(active->user_id) == &shell)
        mgr.clear_dedicated(active->user_id);
    mgr.unregister_window(&shell);
    if (mgr.window_count() == 0)
        [NSApp terminate:nil];
}
```

---

### Task 18: Add `user_id` to `PopoutEntry` and update persistence

**Files:**
- Modify: `client/include/tesseract/settings.h`
- Modify: `ui/shared/app/ShellBase.cpp` — `populate_pending_restore_popouts_`

- [ ] **Step 1: Add `user_id` to `PopoutEntry` in `settings.h`**

```cpp
struct PopoutEntry
{
    std::string user_id;  // <-- new field
    std::string room_id;
    WindowGeometry geometry;
};
```

- [ ] **Step 2: Update `Settings` serialisation**

Find where `PopoutEntry` is serialised to / deserialised from JSON (likely in `settings.cpp` or the JSON save/load path). Add `"user_id"` to the serialised fields. For backwards compatibility, default to empty string on deserialisation if the field is absent.

- [ ] **Step 3: Update `populate_pending_restore_popouts_` in `ShellBase.cpp`**

The existing implementation (line 50) restores all popout windows regardless of which account is active. After adding `user_id`, filter to the current account:

```cpp
void ShellBase::populate_pending_restore_popouts_()
{
    if (!pending_restore_popouts_.empty())
        return;
    if (!active_account_)
        return;
    const auto& uid = active_account_->user_id;
    for (const auto& e : Settings::instance().popout_windows)
    {
        // Include entries with matching user_id, or legacy entries with empty user_id.
        if (!e.room_id.empty() && (e.user_id.empty() || e.user_id == uid))
            pending_restore_popouts_.push_back(e.room_id);
    }
}
```

- [ ] **Step 4: Update popout persistence (save path)**

Find where `Settings::instance().popout_windows` is written (on popout open/close). Update each `PopoutEntry` being saved to include `active_account_->user_id`:

```cpp
PopoutEntry entry;
entry.user_id  = active_account_ ? active_account_->user_id : "";
entry.room_id  = room_id;
entry.geometry = current_geometry;
Settings::instance().popout_windows.push_back(entry);
Settings::instance().save_to_disk(config_dir());
```

---

### Task 19: Run all tests and commit Phase 3

- [ ] **Step 1: Build and run all tests**

```bash
cmake --build build/linux-qt6-debug
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

Expected: all tests pass, including the new `test_account_manager` tests and all existing tests unchanged.

- [ ] **Step 2: Commit Phase 3**

```bash
git add -u
git commit -m "feat: Ctrl+click spawns new window, quit-on-last-window, per-account popouts (phase 3)"
```

---

## Self-Review Checklist (for the implementer)

Before opening a PR, verify:

- [ ] `ctest` passes on Qt6 and GTK4 (the two test-surface backends in CI)
- [ ] Opening the app with two saved accounts shows both in the picker on each window
- [ ] Regular-click on account with no dedicated window → switches current window ✓
- [ ] Regular-click on account with dedicated window → raises that window, current window unchanged ✓
- [ ] Ctrl+click on account with no dedicated window → new window appears ✓
- [ ] Ctrl+click on account with dedicated window → raises that window (no second window) ✓
- [ ] Closing a non-last window → app stays running, account keeps syncing ✓
- [ ] Closing the last window → app exits ✓
- [ ] Tray menu lists one item per open window by display name ✓
- [ ] Clicking a tray menu item raises that window ✓
- [ ] Tray menu updates when a window is opened or closed ✓
- [ ] Pop-out windows from account A do not restore when account B is active ✓
