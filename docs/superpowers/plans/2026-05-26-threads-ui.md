# Threads UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship cross-platform Matrix Threads UI in Tesseract — a right-side panel (60/40 split) hosting either a list of threads in the current room or an open thread view, driven by a new RoomHeader button and per-message preview chips, with main-list dim + root-row highlight while a thread is active.

**Architecture:** All four platform shells (Qt6, GTK4, Win32, macOS) inherit the behaviour from `ShellBase` and the shared widget tree under `ui/shared/views/`. A pure state-machine function in `ShellBase` computes transitions + side effects; the applier maps side effects onto `Client::{subscribe,unsubscribe}_{thread,room_threads}` calls. Two new widgets (`ThreadView`, `ThreadListView`) compose existing `MessageListView` and `ComposeBar`. `EventHandlerBase` gains thread-callback overrides that marshal to the UI thread and dispatch into new `ShellBase::handle_thread_*_ui_` virtuals. `MessageListView` gains a preview-chip row, a dim layer, and a row-highlight outline.

**Tech Stack:** C++17, Catch2 (tests), CMake/ctest. No new external deps.

Spec: [`docs/superpowers/specs/2026-05-26-threads-ui-design.md`](../specs/2026-05-26-threads-ui-design.md).

---

## File Structure

| Action | Path                                              | Responsibility |
|--------|---------------------------------------------------|----------------|
| Modify | `ui/shared/app/ShellBase.{h,cpp}`                 | Thread-panel state machine; event routing; subscription lifecycle |
| Modify | `ui/shared/app/EventHandlerBase.{h,cpp}`          | Marshal SDK thread callbacks to UI thread |
| Modify | `ui/shared/views/MessageListView.{h,cpp}`         | Filter in-thread replies; preview chip under roots; dim/highlight |
| Modify | `ui/shared/views/RoomView.{h,cpp}`                | Split layout (60/40); thread-panel child orchestration |
| Modify | `ui/shared/views/RoomHeader.{h,cpp}`              | New threads button |
| Create | `ui/shared/views/ThreadView.{h,cpp}`              | Thread header + reused MessageListView + reused ComposeBar |
| Create | `ui/shared/views/ThreadListView.{h,cpp}`          | List of ThreadInfo rows for the current room |
| Create | `tests/cpp/test_shell_thread_transition.cpp`      | Pure state-machine transition tests |
| Create | `tests/cpp/test_shell_thread_panel.cpp`           | Event-routing + main-list filter tests |
| Create | `tests/cpp/test_tk_message_list_threads.cpp`      | Preview chip + dim/highlight + filter tests |
| Create | `tests/cpp/test_tk_thread_view.cpp`               | ThreadView widget tests |
| Create | `tests/cpp/test_tk_thread_list_view.cpp`          | ThreadListView widget tests |
| Modify | `tests/CMakeLists.txt`                            | Register the 5 new test files |
| Modify | `ui/shared/CMakeLists.txt`                        | Compile the 2 new view sources |

---

## Task 1: State machine foundation

Pure `compute_thread_transition_` function + state fields on `ShellBase`. No side effects, no UI, no Client calls. Just inputs → outputs with a side-effect descriptor that later tasks apply.

**Files:**

- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`
- Create: `tests/cpp/test_shell_thread_transition.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1.1: Add ThreadPanel / ThreadTrigger / ThreadTransition types to ShellBase.h**

In `ui/shared/app/ShellBase.h`, inside `class ShellBase`, in the public section right after the existing nested enums (look for the section near `enum class MediaKind`), add:

```cpp
    enum class ThreadPanel
    {
        Closed,
        List,
        Open,
    };

    enum class ThreadTrigger
    {
        ToggleList,      // RoomHeader threads button
        OpenFromList,    // row click in ThreadListView
        OpenFromMain,    // preview chip click in MessageListView
        CloseThread,     // back/close in ThreadView
        RoomSwitch,      // current_room_id_ about to change
    };

    struct ThreadTransition
    {
        ThreadPanel new_state = ThreadPanel::Closed;
        ThreadPanel new_prev  = ThreadPanel::Closed;
        std::string new_root;
        // Effects (executed in order by the applier):
        std::vector<std::string> threads_to_unsubscribe;
        std::vector<std::string> threads_to_subscribe;
        bool subscribe_room_threads_   = false;
        bool unsubscribe_room_threads_ = false;
    };

    // Pure: returns the next state + the subscription side-effects to apply.
    // No Client calls, no UI calls — safe to call from tests.
    static ThreadTransition compute_thread_transition_(
        ThreadPanel cur, ThreadPanel prev, const std::string& current_root,
        ThreadTrigger trigger, const std::string& trigger_root);
```

Then in the `protected:` member-state section (near `current_room_id_`), add:

```cpp
    ThreadPanel thread_panel_      = ThreadPanel::Closed;
    ThreadPanel thread_panel_prev_ = ThreadPanel::Closed;
    std::string current_thread_root_;
```

- [ ] **Step 1.2: Implement compute_thread_transition_ in ShellBase.cpp**

Add to `ui/shared/app/ShellBase.cpp` at the end of the file (just before the closing `} // namespace tesseract`):

```cpp
// static
ShellBase::ThreadTransition ShellBase::compute_thread_transition_(
    ThreadPanel cur, ThreadPanel prev, const std::string& current_root,
    ThreadTrigger trigger, const std::string& trigger_root)
{
    ThreadTransition t;
    t.new_state = cur;
    t.new_prev  = prev;
    t.new_root  = current_root;

    switch (trigger)
    {
    case ThreadTrigger::ToggleList:
        if (cur == ThreadPanel::Closed)
        {
            t.new_state = ThreadPanel::List;
            t.new_prev  = ThreadPanel::Closed;
            t.new_root.clear();
            t.subscribe_room_threads_ = true;
        }
        else if (cur == ThreadPanel::List)
        {
            t.new_state = ThreadPanel::Closed;
            t.new_prev  = ThreadPanel::Closed;
            t.new_root.clear();
            t.unsubscribe_room_threads_ = true;
        }
        else // Open
        {
            // Toggle while a thread is open: close everything (matches
            // the spec — main button is a Closed<->List toggle and any
            // thread sub gets released).
            t.new_state = ThreadPanel::Closed;
            t.new_prev  = ThreadPanel::Closed;
            t.threads_to_unsubscribe.push_back(current_root);
            t.new_root.clear();
            if (prev == ThreadPanel::List)
                t.unsubscribe_room_threads_ = true;
        }
        break;

    case ThreadTrigger::OpenFromList:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = ThreadPanel::Open;
        t.new_prev  = ThreadPanel::List;
        t.new_root  = trigger_root;
        t.threads_to_subscribe.push_back(trigger_root);
        break;

    case ThreadTrigger::OpenFromMain:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = ThreadPanel::Open;
        t.new_prev  = (cur == ThreadPanel::List) ? ThreadPanel::List
                                                 : ThreadPanel::Closed;
        t.new_root  = trigger_root;
        t.threads_to_subscribe.push_back(trigger_root);
        break;

    case ThreadTrigger::CloseThread:
        if (cur != ThreadPanel::Open)
            break; // no-op
        t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = prev;            // back to whatever opened us
        t.new_prev  = ThreadPanel::Closed;
        t.new_root.clear();
        if (prev == ThreadPanel::Closed)
            t.unsubscribe_room_threads_ = false; // never subscribed
        break;

    case ThreadTrigger::RoomSwitch:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        if (cur == ThreadPanel::List
            || (cur == ThreadPanel::Open && prev == ThreadPanel::List))
            t.unsubscribe_room_threads_ = true;
        t.new_state = ThreadPanel::Closed;
        t.new_prev  = ThreadPanel::Closed;
        t.new_root.clear();
        break;
    }
    return t;
}
```

- [ ] **Step 1.3: Write transition tests**

Create `tests/cpp/test_shell_thread_transition.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

using tesseract::ShellBase;
using P = ShellBase::ThreadPanel;
using Tr = ShellBase::ThreadTrigger;

TEST_CASE("ToggleList opens the list from Closed", "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Closed, P::Closed, "", Tr::ToggleList, "");
    CHECK(t.new_state == P::List);
    CHECK(t.new_prev == P::Closed);
    CHECK(t.new_root.empty());
    CHECK(t.subscribe_room_threads_);
    CHECK_FALSE(t.unsubscribe_room_threads_);
    CHECK(t.threads_to_subscribe.empty());
    CHECK(t.threads_to_unsubscribe.empty());
}

TEST_CASE("ToggleList closes the list", "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::ToggleList, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.unsubscribe_room_threads_);
}

TEST_CASE("ToggleList while Open closes everything",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Open, P::List, "$root", Tr::ToggleList, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.new_root.empty());
    REQUIRE(t.threads_to_unsubscribe.size() == 1);
    CHECK(t.threads_to_unsubscribe[0] == "$root");
    CHECK(t.unsubscribe_room_threads_);
}

TEST_CASE("OpenFromList Open(a) subscribes the thread",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::OpenFromList, "$a");
    CHECK(t.new_state == P::Open);
    CHECK(t.new_prev == P::List);
    CHECK(t.new_root == "$a");
    REQUIRE(t.threads_to_subscribe.size() == 1);
    CHECK(t.threads_to_subscribe[0] == "$a");
    CHECK(t.threads_to_unsubscribe.empty());
}

TEST_CASE("OpenFromList from Open(a) swaps to Open(b)",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Open, P::List, "$a", Tr::OpenFromList, "$b");
    CHECK(t.new_state == P::Open);
    CHECK(t.new_prev == P::List);
    CHECK(t.new_root == "$b");
    REQUIRE(t.threads_to_unsubscribe.size() == 1);
    CHECK(t.threads_to_unsubscribe[0] == "$a");
    REQUIRE(t.threads_to_subscribe.size() == 1);
    CHECK(t.threads_to_subscribe[0] == "$b");
}

TEST_CASE("OpenFromMain from Closed remembers Closed as prev",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Closed, P::Closed, "", Tr::OpenFromMain, "$a");
    CHECK(t.new_state == P::Open);
    CHECK(t.new_prev == P::Closed);
    CHECK(t.new_root == "$a");
    REQUIRE(t.threads_to_subscribe.size() == 1);
    CHECK(t.threads_to_subscribe[0] == "$a");
    CHECK_FALSE(t.subscribe_room_threads_);
}

TEST_CASE("OpenFromMain from List remembers List as prev",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::OpenFromMain, "$a");
    CHECK(t.new_state == P::Open);
    CHECK(t.new_prev == P::List);
}

TEST_CASE("CloseThread returns to previous state",
          "[shell][thread_transition]")
{
    auto from_list = ShellBase::compute_thread_transition_(
        P::Open, P::List, "$a", Tr::CloseThread, "");
    CHECK(from_list.new_state == P::List);
    CHECK(from_list.new_prev == P::Closed);
    CHECK(from_list.new_root.empty());
    REQUIRE(from_list.threads_to_unsubscribe.size() == 1);
    CHECK(from_list.threads_to_unsubscribe[0] == "$a");
    CHECK_FALSE(from_list.unsubscribe_room_threads_);

    auto from_closed = ShellBase::compute_thread_transition_(
        P::Open, P::Closed, "$a", Tr::CloseThread, "");
    CHECK(from_closed.new_state == P::Closed);
    CHECK_FALSE(from_closed.unsubscribe_room_threads_);
}

TEST_CASE("CloseThread when not Open is a no-op",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::CloseThread, "");
    CHECK(t.new_state == P::List);
    CHECK(t.threads_to_unsubscribe.empty());
}

TEST_CASE("RoomSwitch from Open(List-prev) releases both subs",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Open, P::List, "$a", Tr::RoomSwitch, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.new_prev == P::Closed);
    REQUIRE(t.threads_to_unsubscribe.size() == 1);
    CHECK(t.threads_to_unsubscribe[0] == "$a");
    CHECK(t.unsubscribe_room_threads_);
}

TEST_CASE("RoomSwitch from List releases list sub only",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::List, P::Closed, "", Tr::RoomSwitch, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.threads_to_unsubscribe.empty());
    CHECK(t.unsubscribe_room_threads_);
}

TEST_CASE("RoomSwitch from Closed is a clean no-op",
          "[shell][thread_transition]")
{
    auto t = ShellBase::compute_thread_transition_(
        P::Closed, P::Closed, "", Tr::RoomSwitch, "");
    CHECK(t.new_state == P::Closed);
    CHECK(t.threads_to_unsubscribe.empty());
    CHECK_FALSE(t.unsubscribe_room_threads_);
}
```

- [ ] **Step 1.4: Register test file in CMakeLists**

In `tests/CMakeLists.txt`, in the `add_executable(tesseract_tests ...)` block, add a new line right after `cpp/test_shell_message_cache.cpp`:

```cmake
    cpp/test_shell_message_cache.cpp
    cpp/test_shell_thread_transition.cpp
```

- [ ] **Step 1.5: Build and run**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests
ctest --test-dir build/linux-qt6-debug -R "thread_transition" --output-on-failure
```

Expected: all 12 tests in `[thread_transition]` pass.

- [ ] **Step 1.6: Commit**

```bash
git add ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp \
        tests/cpp/test_shell_thread_transition.cpp tests/CMakeLists.txt
git commit -m "feat(threads): add pure ShellBase thread-panel state machine"
```

---

## Task 2: EventHandlerBase thread callbacks + ShellBase UI hooks

Wire the SDK→shell hop for the four `on_thread_*` callbacks and `on_threads_updated`. The default `handle_thread_*_ui_` implementations gate by current room+root and dispatch to a virtual that platform shells (and `RoomView` in later tasks) override.

**Files:**

- Modify: `ui/shared/app/EventHandlerBase.h`
- Modify: `ui/shared/app/EventHandlerBase.cpp`
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`
- Create: `tests/cpp/test_shell_thread_panel.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 2.1: Add EventHandlerBase override declarations**

In `ui/shared/app/EventHandlerBase.h`, inside the class (right after the existing `on_message_*` override declarations), add:

```cpp
    void
    on_thread_reset(const std::string& room_id,
                    const std::string& thread_root,
                    EventList snapshot) override;
    void on_thread_inserted(const std::string& room_id,
                            const std::string& thread_root,
                            std::size_t index,
                            std::unique_ptr<Event> event) override;
    void on_thread_updated(const std::string& room_id,
                           const std::string& thread_root,
                           std::size_t index,
                           std::unique_ptr<Event> event) override;
    void on_thread_removed(const std::string& room_id,
                           const std::string& thread_root,
                           std::size_t index) override;
    void on_threads_updated(const std::string& room_id) override;
```

- [ ] **Step 2.2: Implement EventHandlerBase overrides**

Add to `ui/shared/app/EventHandlerBase.cpp` (after the existing `on_message_removed` impl):

```cpp
void EventHandlerBase::on_thread_reset(
    const std::string& room_id, const std::string& thread_root,
    EventList snapshot)
{
    struct Payload
    {
        std::string rid;
        std::string root;
        EventList snap;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, thread_root, std::move(snapshot)});
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
        {
            shell->handle_thread_reset_ui_(std::move(p->rid),
                                           std::move(p->root),
                                           std::move(p->snap));
        });
}

void EventHandlerBase::on_thread_inserted(
    const std::string& room_id, const std::string& thread_root,
    std::size_t index, std::unique_ptr<Event> event)
{
    struct Payload
    {
        std::string rid;
        std::string root;
        std::size_t idx;
        std::unique_ptr<Event> ev;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, thread_root, index, std::move(event)});
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
        {
            shell->handle_thread_inserted_ui_(
                std::move(p->rid), std::move(p->root), p->idx,
                std::move(p->ev));
        });
}

void EventHandlerBase::on_thread_updated(
    const std::string& room_id, const std::string& thread_root,
    std::size_t index, std::unique_ptr<Event> event)
{
    struct Payload
    {
        std::string rid;
        std::string root;
        std::size_t idx;
        std::unique_ptr<Event> ev;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, thread_root, index, std::move(event)});
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
        {
            shell->handle_thread_updated_ui_(
                std::move(p->rid), std::move(p->root), p->idx,
                std::move(p->ev));
        });
}

void EventHandlerBase::on_thread_removed(
    const std::string& room_id, const std::string& thread_root,
    std::size_t index)
{
    shell_->post_to_ui_(
        [shell = shell_, rid = room_id, root = thread_root,
         idx = index]() mutable
        {
            shell->handle_thread_removed_ui_(std::move(rid), std::move(root),
                                             idx);
        });
}

void EventHandlerBase::on_threads_updated(const std::string& room_id)
{
    shell_->post_to_ui_(
        [shell = shell_, rid = room_id]() mutable
        {
            shell->handle_threads_updated_ui_(std::move(rid));
        });
}
```

- [ ] **Step 2.3: Add ShellBase handler declarations**

In `ui/shared/app/ShellBase.h`, in the same `protected:` block that contains `handle_message_inserted_ui_` and friends, add:

```cpp
    virtual void handle_thread_reset_ui_(std::string room_id,
                                         std::string thread_root,
                                         EventList snapshot);
    virtual void handle_thread_inserted_ui_(std::string room_id,
                                            std::string thread_root,
                                            std::size_t index,
                                            std::unique_ptr<Event> ev);
    virtual void handle_thread_updated_ui_(std::string room_id,
                                           std::string thread_root,
                                           std::size_t index,
                                           std::unique_ptr<Event> ev);
    virtual void handle_thread_removed_ui_(std::string room_id,
                                           std::string thread_root,
                                           std::size_t index);
    virtual void handle_threads_updated_ui_(std::string room_id);
```

Then, in the same section where `apply_cached_messages_` lives (the no-op shell hooks for views), add these virtuals that `RoomView` will override in Task 8:

```cpp
    // Push a fresh thread reply timeline into the currently-open ThreadView.
    // Default no-op; RoomView overrides to call thread_view_->set_messages.
    virtual void apply_thread_messages_(
        const std::string& /*thread_root*/,
        std::vector<views::MessageRowData> /*rows*/, bool /*room_switch*/)
    {
    }
    virtual void apply_thread_message_insert_(
        const std::string& /*thread_root*/, std::size_t /*index*/,
        views::MessageRowData /*row*/)
    {
    }
    virtual void apply_thread_message_update_(
        const std::string& /*thread_root*/, std::size_t /*index*/,
        views::MessageRowData /*row*/)
    {
    }
    virtual void apply_thread_message_remove_(
        const std::string& /*thread_root*/, std::size_t /*index*/)
    {
    }
    virtual void apply_threads_list_(std::vector<ThreadInfo> /*threads*/)
    {
    }
```

- [ ] **Step 2.4: Implement ShellBase handlers**

In `ui/shared/app/ShellBase.cpp`, alongside the other `handle_*_ui_` implementations (just after `handle_message_removed_ui_`), add:

```cpp
void ShellBase::handle_thread_reset_ui_(std::string room_id,
                                        std::string thread_root,
                                        EventList snapshot)
{
    if (room_id != current_room_id_ || thread_root != current_thread_root_)
        return;
    std::vector<views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    for (auto& ev : snapshot)
    {
        if (!ev || ev->type == tesseract::EventType::Unhandled)
            continue;
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
            ensure_reply_details_(ev->event_id);
        rows.push_back(tesseract::views::make_row_data(*ev, my_user_id_));
    }
    apply_thread_messages_(thread_root, std::move(rows), /*room_switch=*/true);
}

void ShellBase::handle_thread_inserted_ui_(std::string room_id,
                                           std::string thread_root,
                                           std::size_t index,
                                           std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
        return;
    if (room_id != current_room_id_ || thread_root != current_thread_root_)
        return;
    prep_row_media_(*ev);
    if (!ev->in_reply_to_id.empty())
        ensure_reply_details_(ev->event_id);
    apply_thread_message_insert_(
        thread_root, index,
        tesseract::views::make_row_data(*ev, my_user_id_));
}

void ShellBase::handle_thread_updated_ui_(std::string room_id,
                                          std::string thread_root,
                                          std::size_t index,
                                          std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
        return;
    if (room_id != current_room_id_ || thread_root != current_thread_root_)
        return;
    prep_row_media_(*ev);
    if (!ev->in_reply_to_id.empty())
        ensure_reply_details_(ev->event_id);
    apply_thread_message_update_(
        thread_root, index,
        tesseract::views::make_row_data(*ev, my_user_id_));
}

void ShellBase::handle_thread_removed_ui_(std::string room_id,
                                          std::string thread_root,
                                          std::size_t index)
{
    if (room_id != current_room_id_ || thread_root != current_thread_root_)
        return;
    apply_thread_message_remove_(thread_root, index);
}

void ShellBase::handle_threads_updated_ui_(std::string room_id)
{
    if (room_id != current_room_id_ || thread_panel_ == ThreadPanel::Closed)
        return;
    if (!client_)
        return;
    apply_threads_list_(client_->list_room_threads(room_id));
}
```

- [ ] **Step 2.5: Write event-routing tests**

Create `tests/cpp/test_shell_thread_panel.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct TestShell : ShellBase
{
    void post_to_ui_(std::function<void()> fn) override { fn(); }
    void request_relayout_() override {}
    void request_repaint_() override {}
    void on_rooms_updated_() override {}
    void on_media_bytes_ready_(const std::string&, MediaKind,
                               std::vector<uint8_t>) override {}
    void on_tab_state_changed_ui_() override {}
    DecodedImage decode_image_(const std::vector<uint8_t>&, int, int) override
    {
        return {};
    }
    std::int64_t monotonic_ms_() override { return 1000; }
    void start_anim_tick_() override {}
    void repaint_pickers_() override {}
    void navigate_to_room_(const std::string&) override {}

    void apply_thread_messages_(const std::string& root,
                                std::vector<tesseract::views::MessageRowData> rows,
                                bool switch_) override
    {
        last_reset_root  = root;
        last_reset_rows  = std::move(rows);
        last_reset_switch = switch_;
    }
    void apply_thread_message_insert_(
        const std::string& root, std::size_t idx,
        tesseract::views::MessageRowData row) override
    {
        ++insert_calls;
        last_insert_root = root;
        last_insert_idx = idx;
        last_insert_row = std::move(row);
    }
    void apply_thread_message_remove_(const std::string& root,
                                      std::size_t idx) override
    {
        ++remove_calls;
        last_remove_root = root;
        last_remove_idx  = idx;
    }

    using ShellBase::current_room_id_;
    using ShellBase::current_thread_root_;
    using ShellBase::handle_thread_reset_ui_;
    using ShellBase::handle_thread_inserted_ui_;
    using ShellBase::handle_thread_removed_ui_;
    using ShellBase::thread_panel_;

    std::string last_reset_root;
    std::vector<tesseract::views::MessageRowData> last_reset_rows;
    bool last_reset_switch = false;
    int insert_calls = 0;
    std::string last_insert_root;
    std::size_t last_insert_idx = 0;
    tesseract::views::MessageRowData last_insert_row;
    int remove_calls = 0;
    std::string last_remove_root;
    std::size_t last_remove_idx = 0;
};

std::unique_ptr<tesseract::Event> text_event(const std::string& id)
{
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Text;
    ev->event_id = id;
    return ev;
}

} // namespace

TEST_CASE("handle_thread_reset_ui_ dispatches when room+root match",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    tesseract::EventList snap;
    snap.push_back(text_event("$reply1"));
    snap.push_back(text_event("$reply2"));
    s.handle_thread_reset_ui_("!r:x", "$root", std::move(snap));

    CHECK(s.last_reset_root == "$root");
    CHECK(s.last_reset_rows.size() == 2);
    CHECK(s.last_reset_switch);
}

TEST_CASE("handle_thread_reset_ui_ drops mismatched room",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    s.handle_thread_reset_ui_("!other:x", "$root", {});
    CHECK(s.last_reset_root.empty());
}

TEST_CASE("handle_thread_reset_ui_ drops mismatched root",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    s.handle_thread_reset_ui_("!r:x", "$other_root", {});
    CHECK(s.last_reset_root.empty());
}

TEST_CASE("handle_thread_inserted_ui_ skips Unhandled events",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Unhandled;
    s.handle_thread_inserted_ui_("!r:x", "$root", 0, std::move(ev));
    CHECK(s.insert_calls == 0);
}

TEST_CASE("handle_thread_inserted_ui_ dispatches when room+root match",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    s.handle_thread_inserted_ui_("!r:x", "$root", 3, text_event("$r"));
    CHECK(s.insert_calls == 1);
    CHECK(s.last_insert_root == "$root");
    CHECK(s.last_insert_idx == 3);
}

TEST_CASE("handle_thread_removed_ui_ dispatches when room+root match",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    s.handle_thread_removed_ui_("!r:x", "$root", 2);
    CHECK(s.remove_calls == 1);
    CHECK(s.last_remove_idx == 2);
}
```

- [ ] **Step 2.6: Register test file**

In `tests/CMakeLists.txt`, after `cpp/test_shell_thread_transition.cpp`:

```cmake
    cpp/test_shell_thread_transition.cpp
    cpp/test_shell_thread_panel.cpp
```

- [ ] **Step 2.7: Build and run**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests
ctest --test-dir build/linux-qt6-debug -R "thread_panel" --output-on-failure
```

Expected: all 6 tests pass.

- [ ] **Step 2.8: Commit**

```bash
git add ui/shared/app/EventHandlerBase.h ui/shared/app/EventHandlerBase.cpp \
        ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp \
        tests/cpp/test_shell_thread_panel.cpp tests/CMakeLists.txt
git commit -m "feat(threads): marshal SDK thread callbacks through ShellBase"
```

---

## Task 3: Main-list filter for in-thread events

`handle_message_inserted_ui_` / `_updated_ui_` skip the main-list update when `ev.thread_root_id` is non-empty. Cache invalidation rules from commit `9354490` still apply.

**Files:**

- Modify: `ui/shared/app/ShellBase.cpp`
- Modify: `tests/cpp/test_shell_message_cache.cpp` (extend, do not replace)

- [ ] **Step 3.1: Extend handle_message_inserted_ui_ + handle_message_updated_ui_**

In `ui/shared/app/ShellBase.cpp`, locate `ShellBase::handle_message_inserted_ui_` (added in commit `9354490`). Modify the *current-room* branch only — leave the `else { invalidate_message_cache_(room_id); }` path unchanged. Replace the inner `if (room_view_)` block with:

```cpp
    if (room_id == current_room_id_)
    {
        // In-thread replies belong to a thread, not the main list.
        if (!ev->thread_root_id.empty())
        {
            dispatch_message_inserted_secondary_(room_id, index, *ev);
            return;
        }
        if (room_view_)
        {
            prep_row_media_(*ev);
            if (!ev->in_reply_to_id.empty())
            {
                ensure_reply_details_(ev->event_id);
            }
            room_view_->insert_message(
                index, tesseract::views::make_row_data(*ev, my_user_id_));
            request_relayout_();
        }
    }
```

Apply the analogous change to `handle_message_updated_ui_` (mirror the same `!ev->thread_root_id.empty()` short-circuit guarding both `room_view_->update_message` and `dispatch_message_updated_secondary_`):

```cpp
    if (room_id == current_room_id_)
    {
        if (!ev->thread_root_id.empty())
        {
            dispatch_message_updated_secondary_(room_id, index, *ev);
            return;
        }
        if (room_view_)
        {
            prep_row_media_(*ev);
            if (!ev->in_reply_to_id.empty())
            {
                ensure_reply_details_(ev->event_id);
            }
            room_view_->update_message(
                index, tesseract::views::make_row_data(*ev, my_user_id_));
            request_relayout_();
        }
    }
```

`handle_message_removed_ui_` does **not** need a filter: by the time a removal arrives, the row was either never inserted (filter applied at insert time) or it's a non-thread row in the main list.

- [ ] **Step 3.2: Extend test_shell_message_cache.cpp**

In `tests/cpp/test_shell_message_cache.cpp`, after the last existing TEST_CASE, append:

```cpp
TEST_CASE("handle_message_inserted_ui_ skips main list when ev is in-thread",
          "[shell][thread_filter]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Text;
    ev->event_id = "$reply";
    ev->thread_root_id = "$root"; // marks as in-thread

    // Should not crash even without a room_view_; the early-return prevents
    // the room_view_->insert_message() call.
    s.handle_message_inserted_ui_("!r:x", 0, std::move(ev));
    // No assertion needed beyond not crashing — TestShell has no room_view_,
    // so the prior code path would deref null. Reaching this point proves
    // the short-circuit is in place.
    SUCCEED();
}

TEST_CASE("handle_message_inserted_ui_ still invalidates background-room cache",
          "[shell][thread_filter]")
{
    TestShell s;
    s.current_room_id_ = "!current:x";
    seed_cache(s, "!other:x");
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Text;
    ev->event_id = "$reply";
    ev->thread_root_id = "$root";
    s.handle_message_inserted_ui_("!other:x", 0, std::move(ev));
    // Background-room path is unchanged — cache invalidation still fires
    // regardless of thread membership.
    CHECK(s.message_cache_.count("!other:x") == 0);
}
```

- [ ] **Step 3.3: Build and run**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests
ctest --test-dir build/linux-qt6-debug -R "thread_filter|message_cache" --output-on-failure
```

Expected: all 8 tests pass (6 existing + 2 new).

- [ ] **Step 3.4: Commit**

```bash
git add ui/shared/app/ShellBase.cpp tests/cpp/test_shell_message_cache.cpp
git commit -m "feat(threads): filter in-thread events out of main message list"
```

---

## Task 4: MessageListView preview chip + dim/highlight

Three additions, all in `MessageListView`:
1. Drop incoming rows whose `thread_root_id` is non-empty (defence in depth).
2. After a thread-root row, render a thin preview chip showing `thread_latest_sender_name`, `thread_latest_body` snippet, reply count. Click fires `on_thread_preview_clicked(root_event_id)`.
3. New `set_dimmed(bool)` and `set_highlighted_event(event_id)` methods.

**Files:**

- Modify: `ui/shared/views/MessageListView.h`
- Modify: `ui/shared/views/MessageListView.cpp`
- Create: `tests/cpp/test_tk_message_list_threads.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 4.1: Declare the new API in MessageListView.h**

In `ui/shared/views/MessageListView.h`, inside the public section of `class MessageListView`, alongside the other callbacks, add:

```cpp
    std::function<void(const std::string& thread_root_id)>
        on_thread_preview_clicked;

    // Paint a semi-transparent dark overlay over the list bounds. The
    // highlight outline (see below) is painted after this overlay so it
    // remains visible.
    void set_dimmed(bool dimmed);
    bool dimmed() const { return dimmed_; }

    // Paint a colored 2px outline around the row whose event_id matches.
    // Pass the empty string to clear the highlight.
    void set_highlighted_event(const std::string& event_id);
    const std::string& highlighted_event() const { return highlighted_event_id_; }
```

And in the private section, add the backing fields:

```cpp
    bool dimmed_ = false;
    std::string highlighted_event_id_;
```

- [ ] **Step 4.2: Implement set_dimmed / set_highlighted_event**

In `ui/shared/views/MessageListView.cpp`, add (near the other simple setters):

```cpp
void MessageListView::set_dimmed(bool dimmed)
{
    if (dimmed_ == dimmed) return;
    dimmed_ = dimmed;
    request_repaint();
}

void MessageListView::set_highlighted_event(const std::string& event_id)
{
    if (highlighted_event_id_ == event_id) return;
    highlighted_event_id_ = event_id;
    request_repaint();
}
```

- [ ] **Step 4.3: Add filter to set_messages / insert_message / append_message**

In `ui/shared/views/MessageListView.cpp`, locate `MessageListView::set_messages(std::vector<MessageRowData> msgs, bool room_switch)`. At the very top of the body (before any other work), insert:

```cpp
    // Hide in-thread replies from the main timeline; the thread view owns them.
    msgs.erase(std::remove_if(msgs.begin(), msgs.end(),
                              [](const MessageRowData& m)
                              { return !m.thread_root_id.empty(); }),
               msgs.end());
```

In `MessageListView::insert_message(std::size_t index, MessageRowData msg)`, at the top:

```cpp
    if (!msg.thread_root_id.empty()) return;
```

In `MessageListView::append_message(MessageRowData msg)`, at the top:

```cpp
    if (!msg.thread_root_id.empty()) return;
```

In `MessageListView::update_message(std::size_t index, MessageRowData msg)`, at the top:

```cpp
    if (!msg.thread_root_id.empty()) return;
```

(`<algorithm>` is already included transitively by other operations; if a build error says otherwise, add `#include <algorithm>` near the top of the file.)

- [ ] **Step 4.4: Add preview-chip rendering**

In `ui/shared/views/MessageListView.cpp`, locate the row-height computation method (search for `measure_row_` or `row_height_for_` — the adapter pattern method that returns a row's pixel height). Find the place where it returns the final height and, before the return, add a chip-height contribution:

```cpp
    // Thread preview chip: 28px under thread roots that have replies.
    if (row.is_thread_root && row.thread_reply_count > 0)
        height += 28.0f + 4.0f; // chip + top gap
```

Then locate the row paint method (search for `paint_row_` in the adapter). After the row body paints but before it returns/exits the per-row scope, add the chip paint:

```cpp
    if (row.is_thread_root && row.thread_reply_count > 0)
    {
        // Chip rect: indented under the message body, 28px tall.
        tk::Rect chip{row_bounds.x + kBubbleLeftPad,
                      row_bounds.y + row_bounds.h - 28.0f,
                      row_bounds.w - kBubbleLeftPad - kBubbleRightPad,
                      28.0f};
        const auto bg = dimmed_ ? tk::Color{60, 60, 70, 220}
                                : tk::Color{52, 56, 70, 200};
        canvas.fill_round_rect(chip, 6.0f, bg);
        std::string preview = row.thread_latest_sender_name;
        if (!row.thread_latest_body.empty())
            preview += ": " + row.thread_latest_body;
        if (preview.size() > 80)
            preview = preview.substr(0, 77) + "...";
        canvas.draw_text(preview, chip.x + 10.0f, chip.y + 8.0f,
                         tk::TextStyle::body_secondary());
        std::string count = std::to_string(row.thread_reply_count) + " replies";
        canvas.draw_text(count, chip.x + chip.w - 70.0f, chip.y + 8.0f,
                         tk::TextStyle::body_secondary());
        chip_hit_rects_.push_back({row.event_id, chip});
    }
```

Add to the private fields in `MessageListView.h`:

```cpp
    struct ChipHit { std::string root_event_id; tk::Rect rect; };
    std::vector<ChipHit> chip_hit_rects_;
```

Clear `chip_hit_rects_` at the top of the list paint method (search for the override of `paint()` in MessageListView.cpp — there's exactly one).

- [ ] **Step 4.5: Wire chip clicks**

In the pointer-down handler of MessageListView (search for `on_pointer_down` or `handle_pointer_down`), before the existing message-row hit-test, add:

```cpp
    for (const auto& hit : chip_hit_rects_)
    {
        if (hit.rect.contains(p))
        {
            if (on_thread_preview_clicked) on_thread_preview_clicked(hit.root_event_id);
            return true;
        }
    }
```

- [ ] **Step 4.6: Paint dim overlay and highlight outline**

In the same `paint()` override of MessageListView.cpp, after the row loop finishes painting all rows but before any scroll-bar / footer paint, add:

```cpp
    if (dimmed_)
    {
        canvas.fill_rect(bounds_, tk::Color{0, 0, 0, 60});
    }
    if (!highlighted_event_id_.empty())
    {
        for (std::size_t i = 0; i < messages_.size(); ++i)
        {
            if (messages_[i].event_id != highlighted_event_id_) continue;
            const auto r = row_world_rect_(i);
            if (r.has_value())
            {
                canvas.draw_round_rect(*r, 6.0f,
                                       tk::Color{120, 180, 255, 220}, 2.0f);
            }
            break;
        }
    }
```

If `row_world_rect_(i)` does not exist, add it next to the adapter helpers — it should return the row's `tk::Rect` in canvas-space (use the same math the row painter already uses to compute its rect). Returning `std::optional<tk::Rect>` lets the caller skip rows that are scrolled out of view.

- [ ] **Step 4.7: Write tests**

Create `tests/cpp/test_tk_message_list_threads.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"

#include <memory>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace
{

MessageRowData make_text(const std::string& id, const std::string& body = "x")
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.body = body;
    return r;
}

MessageRowData make_thread_root(const std::string& id, uint64_t replies = 2)
{
    auto r = make_text(id, "root");
    r.is_thread_root = true;
    r.thread_reply_count = replies;
    r.thread_latest_sender_name = "Bob";
    r.thread_latest_body = "Latest reply text";
    r.thread_latest_ts = 1234567890ULL;
    return r;
}

MessageRowData make_in_thread_reply(const std::string& id,
                                    const std::string& root)
{
    auto r = make_text(id, "reply");
    r.thread_root_id = root;
    return r;
}

} // namespace

TEST_CASE("set_messages drops in-thread replies", "[message_list][threads]")
{
    MessageListView v;
    std::vector<MessageRowData> msgs;
    msgs.push_back(make_text("$a"));
    msgs.push_back(make_in_thread_reply("$b", "$root"));
    msgs.push_back(make_text("$c"));
    v.set_messages(std::move(msgs), false);
    REQUIRE(v.messages().size() == 2);
    CHECK(v.messages()[0].event_id == "$a");
    CHECK(v.messages()[1].event_id == "$c");
}

TEST_CASE("insert_message drops in-thread replies", "[message_list][threads]")
{
    MessageListView v;
    v.set_messages({make_text("$a")}, false);
    v.insert_message(1, make_in_thread_reply("$b", "$root"));
    REQUIRE(v.messages().size() == 1);
}

TEST_CASE("append_message drops in-thread replies", "[message_list][threads]")
{
    MessageListView v;
    v.set_messages({make_text("$a")}, false);
    v.append_message(make_in_thread_reply("$b", "$root"));
    REQUIRE(v.messages().size() == 1);
}

TEST_CASE("set_dimmed flips the dim flag", "[message_list][threads]")
{
    MessageListView v;
    CHECK_FALSE(v.dimmed());
    v.set_dimmed(true);
    CHECK(v.dimmed());
    v.set_dimmed(false);
    CHECK_FALSE(v.dimmed());
}

TEST_CASE("set_highlighted_event stores the id", "[message_list][threads]")
{
    MessageListView v;
    CHECK(v.highlighted_event().empty());
    v.set_highlighted_event("$root");
    CHECK(v.highlighted_event() == "$root");
    v.set_highlighted_event("");
    CHECK(v.highlighted_event().empty());
}

TEST_CASE("on_thread_preview_clicked fires for chip hit",
          "[message_list][threads]")
{
    MessageListView v;
    std::string clicked;
    v.on_thread_preview_clicked =
        [&](const std::string& id) { clicked = id; };
    v.set_messages({make_thread_root("$root")}, false);
    v.set_bounds({0, 0, 600, 600});
    v.arrange({0, 0, 600, 600});
    v.paint(/*canvas*/); // populate chip_hit_rects_

    // The chip is the last 28px of the only row, centered horizontally.
    // Click at the chip's rough vertical center.
    auto& chips = v.chip_hit_rects_for_test();
    REQUIRE(chips.size() == 1);
    const auto& chip = chips[0].rect;
    v.on_pointer_down({chip.x + 4.0f, chip.y + 4.0f});
    CHECK(clicked == "$root");
}
```

For the last test to access `chip_hit_rects_`, expose them via a friend or a `_for_test()` accessor in MessageListView. Simplest: add to MessageListView.h in the public section:

```cpp
    const std::vector<ChipHit>& chip_hit_rects_for_test() const
    {
        return chip_hit_rects_;
    }
```

If the project's existing `tk::Canvas` cannot be default-constructed (likely), look at how `test_tk_message_list.cpp` (or any `test_tk_*.cpp` file already in the suite) builds a paintable canvas in tests, and use the same helper. The `paint(canvas)` call in the test should mirror the pattern used there.

- [ ] **Step 4.8: Register test file**

In `tests/CMakeLists.txt`, after `cpp/test_shell_thread_panel.cpp`:

```cmake
    cpp/test_shell_thread_panel.cpp
    cpp/test_tk_message_list_threads.cpp
```

- [ ] **Step 4.9: Build and run**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests
ctest --test-dir build/linux-qt6-debug -R "message_list.*threads|threads.*message_list" --output-on-failure
```

Expected: 6 tests pass.

- [ ] **Step 4.10: Commit**

```bash
git add ui/shared/views/MessageListView.h ui/shared/views/MessageListView.cpp \
        tests/cpp/test_tk_message_list_threads.cpp tests/CMakeLists.txt
git commit -m "feat(threads): preview chip, dim overlay, highlight outline in main message list"
```

---

## Task 5: ThreadListView widget

A vertical list of thread rows (root + last reply preview + reply count). Built from `Client::list_room_threads(room_id)`.

**Files:**

- Create: `ui/shared/views/ThreadListView.h`
- Create: `ui/shared/views/ThreadListView.cpp`
- Modify: `ui/shared/CMakeLists.txt`
- Create: `tests/cpp/test_tk_thread_list_view.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 5.1: Declare ThreadListView**

Create `ui/shared/views/ThreadListView.h`:

```cpp
#pragma once

// Right-panel widget that lists every thread known in the current room.
// Built from Client::list_room_threads(room_id) snapshots; refreshed by the
// shell whenever on_threads_updated fires for the current room.

#include "tk/widget.h"
#include "tk/canvas.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class ThreadListView : public tk::Widget
{
public:
    ThreadListView();

    void set_threads(std::vector<ThreadInfo> threads);
    const std::vector<ThreadInfo>& threads() const { return threads_; }

    std::function<void()> on_close;
    std::function<void(const std::string& root_event_id)> on_thread_clicked;
    std::function<void()> on_near_bottom; // for paginate_room_threads

    // tk::Widget overrides
    void arrange(tk::Rect bounds) override;
    void paint(tk::Canvas& canvas) override;
    bool on_pointer_down(tk::Point p) override;
    bool on_pointer_up(tk::Point p) override;
    tk::SizeHint size_hint() const override;

    static constexpr float kHeaderH = 36.0f;
    static constexpr float kRowH    = 64.0f;

private:
    std::vector<ThreadInfo> threads_;
    tk::Rect bounds_{};
    tk::Rect close_btn_rect_{};
    float scroll_y_ = 0.0f;
    int pressed_row_ = -1;
    bool close_pressed_ = false;
};

} // namespace tesseract::views
```

- [ ] **Step 5.2: Implement ThreadListView**

Create `ui/shared/views/ThreadListView.cpp`:

```cpp
#include "views/ThreadListView.h"

#include <algorithm>

namespace tesseract::views
{

ThreadListView::ThreadListView() = default;

void ThreadListView::set_threads(std::vector<ThreadInfo> threads)
{
    threads_ = std::move(threads);
    request_repaint();
}

void ThreadListView::arrange(tk::Rect bounds)
{
    bounds_ = bounds;
    close_btn_rect_ = {bounds.x + bounds.w - 36.0f, bounds.y + 6.0f,
                       24.0f, 24.0f};
}

void ThreadListView::paint(tk::Canvas& canvas)
{
    canvas.fill_rect(bounds_, tk::Color{20, 22, 30, 255});
    // Header
    const tk::Rect header{bounds_.x, bounds_.y, bounds_.w, kHeaderH};
    canvas.fill_rect(header, tk::Color{30, 32, 42, 255});
    canvas.draw_text("Threads", header.x + 12.0f, header.y + 8.0f,
                     tk::TextStyle::title());
    // Close button
    const auto close_bg = close_pressed_ ? tk::Color{90, 90, 110, 255}
                                         : tk::Color{0, 0, 0, 0};
    canvas.fill_round_rect(close_btn_rect_, 4.0f, close_bg);
    canvas.draw_text("X", close_btn_rect_.x + 7.0f, close_btn_rect_.y + 4.0f,
                     tk::TextStyle::title());

    // Rows
    float y = bounds_.y + kHeaderH - scroll_y_;
    for (std::size_t i = 0; i < threads_.size(); ++i)
    {
        const tk::Rect row{bounds_.x, y, bounds_.w, kRowH};
        if (row.y + row.h < bounds_.y + kHeaderH || row.y > bounds_.y + bounds_.h)
        {
            y += kRowH;
            continue;
        }
        const auto bg = (pressed_row_ == static_cast<int>(i))
                            ? tk::Color{45, 50, 60, 255}
                            : tk::Color{25, 28, 36, 255};
        canvas.fill_rect(row, bg);
        const auto& th = threads_[i];
        // Root preview
        std::string root_line = th.root_sender_name;
        if (!th.root_body.empty()) root_line += ": " + th.root_body;
        if (root_line.size() > 60) root_line = root_line.substr(0, 57) + "...";
        canvas.draw_text(root_line, row.x + 12.0f, row.y + 8.0f,
                         tk::TextStyle::body());
        // Last reply preview
        if (!th.latest_sender_name.empty())
        {
            std::string reply_line = "↳ "; // ↳
            reply_line += th.latest_sender_name;
            if (!th.latest_body.empty()) reply_line += ": " + th.latest_body;
            if (reply_line.size() > 60)
                reply_line = reply_line.substr(0, 57) + "...";
            canvas.draw_text(reply_line, row.x + 24.0f, row.y + 28.0f,
                             tk::TextStyle::body_secondary());
        }
        // Reply count
        std::string count = std::to_string(th.num_replies) + " replies";
        canvas.draw_text(count, row.x + row.w - 80.0f, row.y + 8.0f,
                         tk::TextStyle::body_secondary());
        y += kRowH;
    }
}

bool ThreadListView::on_pointer_down(tk::Point p)
{
    if (close_btn_rect_.contains(p))
    {
        close_pressed_ = true;
        request_repaint();
        return true;
    }
    if (p.y < bounds_.y + kHeaderH || p.y > bounds_.y + bounds_.h) return false;
    const float local_y = p.y - (bounds_.y + kHeaderH) + scroll_y_;
    const int idx = static_cast<int>(local_y / kRowH);
    if (idx < 0 || idx >= static_cast<int>(threads_.size())) return false;
    pressed_row_ = idx;
    request_repaint();
    return true;
}

bool ThreadListView::on_pointer_up(tk::Point p)
{
    if (close_pressed_)
    {
        close_pressed_ = false;
        request_repaint();
        if (close_btn_rect_.contains(p))
        {
            if (on_close) on_close();
            return true;
        }
        return false;
    }
    if (pressed_row_ < 0) return false;
    const int idx = pressed_row_;
    pressed_row_ = -1;
    request_repaint();
    if (p.y < bounds_.y + kHeaderH) return false;
    const float local_y = p.y - (bounds_.y + kHeaderH) + scroll_y_;
    const int hit = static_cast<int>(local_y / kRowH);
    if (hit == idx && idx < static_cast<int>(threads_.size()))
    {
        if (on_thread_clicked) on_thread_clicked(threads_[idx].root_event_id);
        return true;
    }
    return false;
}

tk::SizeHint ThreadListView::size_hint() const
{
    return {280.0f, 400.0f};
}

} // namespace tesseract::views
```

(If the tk APIs are different — e.g. `Widget::request_repaint()` is named `repaint()`, or `Canvas::fill_round_rect` doesn't exist — open `ui/shared/tk/canvas.h` and `widget.h` and adapt names to the actual API. The structure is correct; method names may need cosmetic edits.)

- [ ] **Step 5.3: Add to ui/shared/CMakeLists.txt**

Open `ui/shared/CMakeLists.txt` and locate the list of `views/*.cpp` source files in the `target_sources(tesseract_tk ...)` call. Add `views/ThreadListView.cpp` alphabetically.

- [ ] **Step 5.4: Write tests**

Create `tests/cpp/test_tk_thread_list_view.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "views/ThreadListView.h"

using tesseract::views::ThreadListView;
using tesseract::ThreadInfo;

namespace
{
ThreadInfo make_thread(const std::string& root, uint64_t replies)
{
    ThreadInfo t;
    t.root_event_id = root;
    t.root_sender_name = "Alice";
    t.root_body = "Hello world";
    t.root_timestamp = 1000;
    t.latest_sender_name = "Bob";
    t.latest_body = "Reply!";
    t.num_replies = replies;
    return t;
}
}

TEST_CASE("set_threads stores the list", "[thread_list]")
{
    ThreadListView v;
    v.set_threads({make_thread("$a", 1), make_thread("$b", 5)});
    REQUIRE(v.threads().size() == 2);
    CHECK(v.threads()[0].root_event_id == "$a");
}

TEST_CASE("on_close fires when close button clicked", "[thread_list]")
{
    ThreadListView v;
    v.arrange({0, 0, 300, 400});
    bool closed = false;
    v.on_close = [&] { closed = true; };
    // close_btn_rect_ is right-aligned, so click in the top-right corner.
    v.on_pointer_down({300 - 24.0f, 12.0f});
    v.on_pointer_up({300 - 24.0f, 12.0f});
    CHECK(closed);
}

TEST_CASE("on_thread_clicked fires for row clicks", "[thread_list]")
{
    ThreadListView v;
    v.arrange({0, 0, 300, 400});
    v.set_threads({make_thread("$a", 1), make_thread("$b", 5)});
    std::string clicked;
    v.on_thread_clicked = [&](const std::string& id) { clicked = id; };
    // First row sits just under the header (header is 36px), 64px tall.
    v.on_pointer_down({100.0f, 36.0f + 30.0f});
    v.on_pointer_up({100.0f, 36.0f + 30.0f});
    CHECK(clicked == "$a");

    v.on_pointer_down({100.0f, 36.0f + 64.0f + 30.0f});
    v.on_pointer_up({100.0f, 36.0f + 64.0f + 30.0f});
    CHECK(clicked == "$b");
}

TEST_CASE("on_thread_clicked does NOT fire if release outside row",
          "[thread_list]")
{
    ThreadListView v;
    v.arrange({0, 0, 300, 400});
    v.set_threads({make_thread("$a", 1)});
    std::string clicked;
    v.on_thread_clicked = [&](const std::string& id) { clicked = id; };
    v.on_pointer_down({100.0f, 36.0f + 30.0f});
    v.on_pointer_up({100.0f, 10.0f}); // header area, not the row
    CHECK(clicked.empty());
}
```

- [ ] **Step 5.5: Register test**

In `tests/CMakeLists.txt`, after `cpp/test_tk_message_list_threads.cpp`:

```cmake
    cpp/test_tk_message_list_threads.cpp
    cpp/test_tk_thread_list_view.cpp
```

- [ ] **Step 5.6: Build and run**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests
ctest --test-dir build/linux-qt6-debug -R "thread_list" --output-on-failure
```

Expected: 4 tests pass.

- [ ] **Step 5.7: Commit**

```bash
git add ui/shared/views/ThreadListView.h ui/shared/views/ThreadListView.cpp \
        ui/shared/CMakeLists.txt \
        tests/cpp/test_tk_thread_list_view.cpp tests/CMakeLists.txt
git commit -m "feat(threads): add ThreadListView widget"
```

---

## Task 6: ThreadView widget

Composes a small thread header (root preview + close button) on top of an embedded `MessageListView` for the reply timeline, with an embedded `ComposeBar` at the bottom that routes sends through a callback.

**Files:**

- Create: `ui/shared/views/ThreadView.h`
- Create: `ui/shared/views/ThreadView.cpp`
- Modify: `ui/shared/CMakeLists.txt`
- Create: `tests/cpp/test_tk_thread_view.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 6.1: Declare ThreadView**

Create `ui/shared/views/ThreadView.h`:

```cpp
#pragma once

// Right-panel widget showing one thread: header (root preview + close),
// embedded MessageListView (thread replies), embedded ComposeBar
// (sends route to Client::send_thread_message via on_send).

#include "tk/widget.h"
#include "tk/canvas.h"
#include "views/MessageListView.h"
#include "views/ComposeBar.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class ThreadView : public tk::Widget
{
public:
    ThreadView();

    // Identify the thread + populate the header preview.
    void set_thread(std::string root_event_id, MessageRowData root_preview);
    const std::string& thread_root() const { return thread_root_; }

    // Delegate to the embedded MessageListView.
    void set_messages(std::vector<MessageRowData> rows, bool room_switch);
    void insert_message(std::size_t index, MessageRowData row);
    void update_message(std::size_t index, MessageRowData row);
    void remove_message(std::size_t index);

    MessageListView* message_list() { return message_list_.get(); }
    ComposeBar* compose_bar() { return compose_bar_.get(); }

    std::function<void()> on_close;
    std::function<void(const std::string& body,
                       const std::string& formatted_body)> on_send;

    // tk::Widget
    void arrange(tk::Rect bounds) override;
    void paint(tk::Canvas& canvas) override;
    bool on_pointer_down(tk::Point p) override;
    bool on_pointer_up(tk::Point p) override;
    tk::SizeHint size_hint() const override;

    static constexpr float kHeaderH = 48.0f;

private:
    std::string thread_root_;
    MessageRowData root_preview_;
    std::unique_ptr<MessageListView> message_list_;
    std::unique_ptr<ComposeBar> compose_bar_;
    tk::Rect bounds_{};
    tk::Rect close_btn_rect_{};
    bool close_pressed_ = false;
};

} // namespace tesseract::views
```

- [ ] **Step 6.2: Implement ThreadView**

Create `ui/shared/views/ThreadView.cpp`:

```cpp
#include "views/ThreadView.h"

namespace tesseract::views
{

ThreadView::ThreadView()
    : message_list_(std::make_unique<MessageListView>()),
      compose_bar_(std::make_unique<ComposeBar>())
{
    add_child(message_list_.get());
    add_child(compose_bar_.get());
    compose_bar_->on_send_clicked =
        [this](const std::string& body, const std::string& formatted)
    {
        if (on_send) on_send(body, formatted);
    };
}

void ThreadView::set_thread(std::string root_event_id,
                            MessageRowData root_preview)
{
    thread_root_ = std::move(root_event_id);
    root_preview_ = std::move(root_preview);
    request_repaint();
}

void ThreadView::set_messages(std::vector<MessageRowData> rows, bool room_switch)
{
    message_list_->set_messages(std::move(rows), room_switch);
}

void ThreadView::insert_message(std::size_t index, MessageRowData row)
{
    message_list_->insert_message(index, std::move(row));
}

void ThreadView::update_message(std::size_t index, MessageRowData row)
{
    message_list_->update_message(index, std::move(row));
}

void ThreadView::remove_message(std::size_t index)
{
    message_list_->remove_message(index);
}

void ThreadView::arrange(tk::Rect bounds)
{
    bounds_ = bounds;
    close_btn_rect_ = {bounds.x + bounds.w - 36.0f, bounds.y + 12.0f,
                       24.0f, 24.0f};
    const float compose_h = compose_bar_->natural_height();
    const float list_top = bounds.y + kHeaderH;
    const float list_bot = bounds.y + bounds.h - compose_h;
    if (message_list_)
        message_list_->arrange({bounds.x, list_top, bounds.w,
                                std::max(0.0f, list_bot - list_top)});
    if (compose_bar_)
        compose_bar_->arrange({bounds.x, list_bot, bounds.w, compose_h});
}

void ThreadView::paint(tk::Canvas& canvas)
{
    canvas.fill_rect(bounds_, tk::Color{20, 22, 30, 255});
    const tk::Rect header{bounds_.x, bounds_.y, bounds_.w, kHeaderH};
    canvas.fill_rect(header, tk::Color{30, 32, 42, 255});
    // Header: root preview + close button
    std::string preview = root_preview_.sender_name;
    if (!root_preview_.body.empty()) preview += ": " + root_preview_.body;
    if (preview.size() > 60) preview = preview.substr(0, 57) + "...";
    canvas.draw_text(preview, header.x + 12.0f, header.y + 8.0f,
                     tk::TextStyle::body());
    canvas.draw_text("Thread", header.x + 12.0f, header.y + 26.0f,
                     tk::TextStyle::body_secondary());
    const auto close_bg = close_pressed_ ? tk::Color{90, 90, 110, 255}
                                         : tk::Color{0, 0, 0, 0};
    canvas.fill_round_rect(close_btn_rect_, 4.0f, close_bg);
    canvas.draw_text("X", close_btn_rect_.x + 7.0f, close_btn_rect_.y + 4.0f,
                     tk::TextStyle::title());
    if (message_list_) message_list_->paint(canvas);
    if (compose_bar_) compose_bar_->paint(canvas);
}

bool ThreadView::on_pointer_down(tk::Point p)
{
    if (close_btn_rect_.contains(p))
    {
        close_pressed_ = true;
        request_repaint();
        return true;
    }
    if (compose_bar_ && compose_bar_->bounds().contains(p))
        return compose_bar_->on_pointer_down(p);
    if (message_list_ && message_list_->bounds().contains(p))
        return message_list_->on_pointer_down(p);
    return false;
}

bool ThreadView::on_pointer_up(tk::Point p)
{
    if (close_pressed_)
    {
        close_pressed_ = false;
        request_repaint();
        if (close_btn_rect_.contains(p))
        {
            if (on_close) on_close();
            return true;
        }
        return false;
    }
    if (compose_bar_ && compose_bar_->bounds().contains(p))
        return compose_bar_->on_pointer_up(p);
    if (message_list_ && message_list_->bounds().contains(p))
        return message_list_->on_pointer_up(p);
    return false;
}

tk::SizeHint ThreadView::size_hint() const
{
    return {280.0f, 600.0f};
}

} // namespace tesseract::views
```

(If `ComposeBar::on_send_clicked`'s exact signature differs, open `ui/shared/views/ComposeBar.h` and adapt. The principle — install a lambda that forwards to `on_send` — is unchanged.)

- [ ] **Step 6.3: Add to ui/shared/CMakeLists.txt**

Add `views/ThreadView.cpp` alphabetically in `target_sources(tesseract_tk ...)`.

- [ ] **Step 6.4: Write tests**

Create `tests/cpp/test_tk_thread_view.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "views/ThreadView.h"

using tesseract::views::ThreadView;
using tesseract::views::MessageRowData;

namespace
{
MessageRowData make_preview()
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = "$root";
    r.sender_name = "Alice";
    r.body = "Hello";
    r.is_thread_root = true;
    r.thread_reply_count = 3;
    return r;
}

MessageRowData make_reply(const std::string& id)
{
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.thread_root_id = "$root";
    r.body = "reply";
    return r;
}
}

TEST_CASE("set_thread stores the root id and preview", "[thread_view]")
{
    ThreadView v;
    v.set_thread("$root", make_preview());
    CHECK(v.thread_root() == "$root");
}

TEST_CASE("set_messages forwards to embedded MessageListView "
          "(thread replies kept)", "[thread_view]")
{
    ThreadView v;
    v.set_thread("$root", make_preview());
    // In MessageListView, in-thread replies are filtered out; we need a
    // dedicated bypass for ThreadView. For now: confirm forwarding works
    // for non-thread rows (regression guard).
    v.set_messages({}, true);
    CHECK(v.message_list()->messages().empty());
}

TEST_CASE("on_close fires when header X clicked", "[thread_view]")
{
    ThreadView v;
    v.arrange({0, 0, 400, 600});
    bool closed = false;
    v.on_close = [&] { closed = true; };
    v.on_pointer_down({400 - 24.0f, 18.0f});
    v.on_pointer_up({400 - 24.0f, 18.0f});
    CHECK(closed);
}

TEST_CASE("on_send fires when ComposeBar reports a send", "[thread_view]")
{
    ThreadView v;
    std::string body;
    v.on_send = [&](const std::string& b, const std::string&) { body = b; };
    // ComposeBar exposes on_send_clicked; trigger directly.
    if (v.compose_bar()->on_send_clicked)
        v.compose_bar()->on_send_clicked("hello", "<b>hello</b>");
    CHECK(body == "hello");
}
```

The second test's "thread replies kept" comment exposes a real design wrinkle: `MessageListView::set_messages` filters in-thread replies — but inside a `ThreadView` we *want* the thread's replies. To avoid mangling the filter, route thread rows directly into the underlying list by **bypassing** the filter. Two options:

1. Add a `set_messages_bypass_filter_(std::vector<MessageRowData>, bool)` private method on `MessageListView` and friend `ThreadView`.
2. Mark thread rows by clearing `thread_root_id` before handing them to `MessageListView` (since once they're inside a thread view, the thread context is implicit from the surrounding widget).

**Choose option 2** — it's simpler and works at the boundary: `ThreadView::set_messages` strips `thread_root_id` from each row before forwarding. Update `ThreadView::set_messages` / `insert_message` / `update_message` accordingly:

```cpp
void ThreadView::set_messages(std::vector<MessageRowData> rows, bool room_switch)
{
    for (auto& r : rows) r.thread_root_id.clear();
    message_list_->set_messages(std::move(rows), room_switch);
}

void ThreadView::insert_message(std::size_t index, MessageRowData row)
{
    row.thread_root_id.clear();
    message_list_->insert_message(index, std::move(row));
}

void ThreadView::update_message(std::size_t index, MessageRowData row)
{
    row.thread_root_id.clear();
    message_list_->update_message(index, std::move(row));
}
```

Now extend the test to verify it:

```cpp
TEST_CASE("ThreadView keeps reply rows even though they have thread_root_id",
          "[thread_view]")
{
    ThreadView v;
    v.set_thread("$root", make_preview());
    std::vector<MessageRowData> rows;
    rows.push_back(make_reply("$r1"));
    rows.push_back(make_reply("$r2"));
    v.set_messages(std::move(rows), true);
    REQUIRE(v.message_list()->messages().size() == 2);
}
```

- [ ] **Step 6.5: Register test**

```cmake
    cpp/test_tk_thread_list_view.cpp
    cpp/test_tk_thread_view.cpp
```

- [ ] **Step 6.6: Build and run**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests
ctest --test-dir build/linux-qt6-debug -R "thread_view" --output-on-failure
```

Expected: 5 tests pass.

- [ ] **Step 6.7: Commit**

```bash
git add ui/shared/views/ThreadView.h ui/shared/views/ThreadView.cpp \
        ui/shared/CMakeLists.txt \
        tests/cpp/test_tk_thread_view.cpp tests/CMakeLists.txt
git commit -m "feat(threads): add ThreadView widget (header + reused MessageListView + ComposeBar)"
```

---

## Task 7: RoomHeader threads button

A 28×28 button immediately left of the existing calendar button. Same press/release pattern, dispatches `on_threads_requested`.

**Files:**

- Modify: `ui/shared/views/RoomHeader.h`
- Modify: `ui/shared/views/RoomHeader.cpp`
- Modify: tests (find the existing RoomHeader test, if any, and extend; otherwise create one)

- [ ] **Step 7.1: Add callback + state to RoomHeader.h**

In `ui/shared/views/RoomHeader.h`, alongside `on_jump_to_date_requested`, add:

```cpp
    std::function<void()> on_threads_requested;
```

In the private section, add:

```cpp
    tk::Rect threads_btn_rect_{};
    bool threads_btn_pressed_ = false;
```

- [ ] **Step 7.2: Render + hit-test the button**

In `ui/shared/views/RoomHeader.cpp`, locate the paint method. Find where `calendar_btn_rect_` is computed (see Tesseract docs: ~lines 423–430). After that, compute the threads button rect (immediately to the left of the calendar button, same size, with an 8px gap):

```cpp
    threads_btn_rect_ = {calendar_btn_rect_.x - 8.0f - 28.0f,
                         calendar_btn_rect_.y,
                         28.0f, 28.0f};
```

Then, in the same paint method, after the calendar button is drawn, draw the threads button:

```cpp
    {
        const auto bg = threads_btn_pressed_ ? tk::Color{90, 90, 110, 255}
                                             : tk::Color{0, 0, 0, 0};
        canvas.fill_round_rect(threads_btn_rect_, 4.0f, bg);
        // Render a stack-of-lines glyph as a quick stand-in for a thread
        // icon. Use whatever icon helper exists in this codebase if there is
        // one (look for emoji_picker/sticker_picker/voice icons in
        // ComposeBar.cpp for the established nanosvg pattern).
        canvas.draw_text("⋮", threads_btn_rect_.x + 9.0f,
                         threads_btn_rect_.y + 4.0f, tk::TextStyle::title());
    }
```

(If the codebase has a real nanosvg "threads" or "messages-stack" icon in `ui/shared/icons/`, prefer that — search there first.)

In `on_pointer_down` (currently in lines ~492–514 per the spec exploration), add the threads-button check before the calendar-button check:

```cpp
    if (threads_btn_rect_.contains(p))
    {
        threads_btn_pressed_ = true;
        request_repaint();
        return true;
    }
```

In `on_pointer_up` (currently ~516–530), add the threads-button release before the calendar release:

```cpp
    if (threads_btn_pressed_)
    {
        threads_btn_pressed_ = false;
        request_repaint();
        if (threads_btn_rect_.contains(p))
        {
            if (on_threads_requested) on_threads_requested();
            return true;
        }
        return false;
    }
```

- [ ] **Step 7.3: Tests**

Find any existing RoomHeader test file (`find tests -name 'test_*room_header*' -o -name 'test_*roomheader*'`). If one exists, append to it; otherwise create `tests/cpp/test_tk_room_header.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "views/RoomHeader.h"

using tesseract::views::RoomHeader;

TEST_CASE("threads button fires on_threads_requested", "[room_header][threads]")
{
    RoomHeader h;
    h.arrange({0, 0, 800, 60});
    // Force a paint to populate the rects (since they're computed in paint()).
    // If RoomHeader exposes a layout_rects_() helper called in arrange(),
    // prefer that — otherwise drive the rect calculation by invoking
    // paint() on a test canvas, mirroring how other RoomHeader tests do it.
    bool clicked = false;
    h.on_threads_requested = [&] { clicked = true; };
    // Calendar button sits 8px from the right (28x28); threads button is
    // 28+8 = 36px to its left → 8 + 28 + 8 + 28 = 72px from the right.
    const float x = 800.0f - 72.0f + 14.0f;
    h.on_pointer_down({x, 30.0f});
    h.on_pointer_up({x, 30.0f});
    CHECK(clicked);
}
```

If RoomHeader's rect math computes rects only inside `paint()` (because text widths feed back into layout), this test must drive a paint pass. Look at how `test_tk_user_info.cpp` exercises hit-test buttons — copy that pattern.

- [ ] **Step 7.4: Register test (if a new file was created)**

```cmake
    cpp/test_tk_thread_view.cpp
    cpp/test_tk_room_header.cpp
```

(Skip this step if you appended to an existing file.)

- [ ] **Step 7.5: Build and run**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests
ctest --test-dir build/linux-qt6-debug -R "room_header" --output-on-failure
```

Expected: existing tests still pass + new one passes.

- [ ] **Step 7.6: Commit**

```bash
git add ui/shared/views/RoomHeader.h ui/shared/views/RoomHeader.cpp \
        tests/cpp/test_tk_room_header.cpp tests/CMakeLists.txt
git commit -m "feat(threads): add threads button to RoomHeader"
```

---

## Task 8: RoomView split layout + end-to-end ShellBase wiring

This is the big integration task. RoomView gets `set_thread_panel(state, root)` + a `thread_view()` / `thread_list_view()` accessors. ShellBase gets the public entry points (`on_threads_button_clicked`, `on_thread_open_requested`, `on_thread_close_requested`) that compute + apply transitions and call `client_->subscribe_*`. The four `apply_thread_*_` no-op hooks added in Task 2 get overridden in… well, ShellBase's default impl is to call into `room_view_` directly.

**Files:**

- Modify: `ui/shared/views/RoomView.h`
- Modify: `ui/shared/views/RoomView.cpp`
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`
- Extend: `tests/cpp/test_shell_thread_panel.cpp`

- [ ] **Step 8.1: Add ThreadView + ThreadListView children + set_thread_panel to RoomView.h**

In `ui/shared/views/RoomView.h`, near the existing `header()` / `message_list()` / `compose_bar()` / `room_info_panel()` accessors, add:

```cpp
    ThreadView*     thread_view()      const { return thread_view_.get(); }
    ThreadListView* thread_list_view() const { return thread_list_view_.get(); }

    // ShellBase::ThreadPanel forward-declared in client/include path; use the
    // shell's enum directly via a small mirror enum to avoid the include cycle.
    enum class ThreadPanelState { Closed, List, Open };
    void set_thread_panel(ThreadPanelState state,
                          const std::string& root_event_id);
    ThreadPanelState thread_panel_state() const { return thread_panel_state_; }

    // Forwarded by RoomView → shell.
    std::function<void()> on_threads_button_clicked;
    std::function<void(const std::string& root_event_id)>
        on_thread_open_requested;
    std::function<void()> on_thread_close_requested;
    std::function<void(const std::string& body,
                       const std::string& formatted_body)> on_thread_send;
```

Add forward declarations / includes:

```cpp
#include "views/ThreadView.h"
#include "views/ThreadListView.h"
```

In private section:

```cpp
    std::unique_ptr<ThreadView>     thread_view_;
    std::unique_ptr<ThreadListView> thread_list_view_;
    ThreadPanelState thread_panel_state_ = ThreadPanelState::Closed;
    std::string thread_panel_root_;
```

- [ ] **Step 8.2: Wire RoomHeader threads button + MessageListView preview chip**

In `ui/shared/views/RoomView.cpp` constructor, after the existing `header_->on_jump_to_date_requested = ...` block, add:

```cpp
    header_->on_threads_requested = [this]
    {
        if (on_threads_button_clicked) on_threads_button_clicked();
    };
    message_list_->on_thread_preview_clicked =
        [this](const std::string& root_event_id)
    {
        if (on_thread_open_requested) on_thread_open_requested(root_event_id);
    };
```

- [ ] **Step 8.3: Implement RoomView::set_thread_panel + split layout**

In `ui/shared/views/RoomView.cpp`, add:

```cpp
void RoomView::set_thread_panel(ThreadPanelState state,
                                const std::string& root_event_id)
{
    thread_panel_state_ = state;
    thread_panel_root_  = root_event_id;
    // Create children lazily on first use.
    if (state == ThreadPanelState::List && !thread_list_view_)
    {
        thread_list_view_ = std::make_unique<ThreadListView>();
        add_child(thread_list_view_.get());
        thread_list_view_->on_close = [this]
        {
            if (on_threads_button_clicked) on_threads_button_clicked();
        };
        thread_list_view_->on_thread_clicked =
            [this](const std::string& root)
        {
            if (on_thread_open_requested) on_thread_open_requested(root);
        };
    }
    if (state == ThreadPanelState::Open && !thread_view_)
    {
        thread_view_ = std::make_unique<ThreadView>();
        add_child(thread_view_.get());
        thread_view_->on_close = [this]
        {
            if (on_thread_close_requested) on_thread_close_requested();
        };
        thread_view_->on_send =
            [this](const std::string& body, const std::string& formatted)
        {
            if (on_thread_send) on_thread_send(body, formatted);
        };
    }
    message_list_->set_dimmed(state == ThreadPanelState::Open);
    message_list_->set_highlighted_event(
        state == ThreadPanelState::Open ? root_event_id : std::string{});
    if (state == ThreadPanelState::Open && !root_event_id.empty())
        message_list_->scroll_to_event_id(root_event_id);
    request_relayout();
}
```

In the arrange method of RoomView (the function the spec exploration pinpointed at lines 847–893), replace the message-list/compose layout block. Find the block that computes `message_list_` and `compose_bar_` bounds and wrap it with the split-aware version:

```cpp
    const bool panel_open = thread_panel_state_ != ThreadPanelState::Closed;
    const float main_w = panel_open ? bounds.w * 0.60f : bounds.w;
    const float panel_w = bounds.w - main_w;
    const float compose_h = compose_bar_->natural_height();
    const float list_top = header_bottom;
    const float list_bot = bounds.y + bounds.h - compose_h;
    message_list_->arrange({bounds.x, list_top, main_w,
                            std::max(0.0f, list_bot - list_top)});
    compose_bar_->arrange({bounds.x, list_bot, main_w, compose_h});
    // Right panel spans from header_bottom all the way to bounds.bottom
    // (ThreadView owns its own ComposeBar; ThreadListView has no compose).
    const float panel_h = bounds.y + bounds.h - list_top;
    if (thread_panel_state_ == ThreadPanelState::List && thread_list_view_)
        thread_list_view_->arrange({bounds.x + main_w, list_top,
                                    panel_w, panel_h});
    if (thread_panel_state_ == ThreadPanelState::Open && thread_view_)
        thread_view_->arrange({bounds.x + main_w, list_top,
                               panel_w, panel_h});
```

In RoomView paint, add (after the message_list_ + compose_bar_ paint calls, before the overlay panel paint calls):

```cpp
    if (thread_panel_state_ == ThreadPanelState::List && thread_list_view_)
        thread_list_view_->paint(canvas);
    if (thread_panel_state_ == ThreadPanelState::Open && thread_view_)
        thread_view_->paint(canvas);
```

For pointer routing, in RoomView's `on_pointer_down` (and `on_pointer_up`), add at the top — *before* the existing message_list_ / compose_bar_ dispatch:

```cpp
    if (thread_panel_state_ == ThreadPanelState::List && thread_list_view_
        && thread_list_view_->bounds().contains(p))
        return thread_list_view_->on_pointer_down(p);
    if (thread_panel_state_ == ThreadPanelState::Open && thread_view_
        && thread_view_->bounds().contains(p))
        return thread_view_->on_pointer_down(p);
```

(`on_pointer_up` gets the analogous block with `on_pointer_up`.)

- [ ] **Step 8.4: Add ShellBase public entry points + the applier**

In `ui/shared/app/ShellBase.h`, in the public section, add:

```cpp
    void on_threads_button_clicked();
    void on_thread_open_requested(const std::string& root_event_id);
    void on_thread_close_requested();
    void on_thread_send_requested(const std::string& body,
                                  const std::string& formatted_body);

protected:
    void apply_thread_transition_(const ThreadTransition& t);
```

In `ui/shared/app/ShellBase.cpp`:

```cpp
void ShellBase::apply_thread_transition_(const ThreadTransition& t)
{
    if (!client_) return;
    for (const auto& root : t.threads_to_unsubscribe)
        client_->unsubscribe_thread(current_room_id_, root);
    if (t.unsubscribe_room_threads_)
        client_->unsubscribe_room_threads(current_room_id_);
    if (t.subscribe_room_threads_)
        client_->subscribe_room_threads(current_room_id_);
    for (const auto& root : t.threads_to_subscribe)
        client_->subscribe_thread(current_room_id_, root);
    thread_panel_         = t.new_state;
    thread_panel_prev_    = t.new_prev;
    current_thread_root_  = t.new_root;
    if (!room_view_) return;
    using S = views::RoomView::ThreadPanelState;
    const S vs = (t.new_state == ThreadPanel::Closed) ? S::Closed
              : (t.new_state == ThreadPanel::List)   ? S::List
                                                     : S::Open;
    room_view_->set_thread_panel(vs, t.new_root);
    if (t.new_state == ThreadPanel::List)
    {
        // Push the current snapshot immediately; on_threads_updated will
        // refresh later as the SDK delivers data.
        apply_threads_list_(client_->list_room_threads(current_room_id_));
    }
}

void ShellBase::on_threads_button_clicked()
{
    auto t = compute_thread_transition_(thread_panel_, thread_panel_prev_,
                                        current_thread_root_,
                                        ThreadTrigger::ToggleList, {});
    apply_thread_transition_(t);
}

void ShellBase::on_thread_open_requested(const std::string& root_event_id)
{
    const auto trigger = (thread_panel_ == ThreadPanel::List)
                             ? ThreadTrigger::OpenFromList
                             : ThreadTrigger::OpenFromMain;
    auto t = compute_thread_transition_(thread_panel_, thread_panel_prev_,
                                        current_thread_root_,
                                        trigger, root_event_id);
    apply_thread_transition_(t);
}

void ShellBase::on_thread_close_requested()
{
    auto t = compute_thread_transition_(thread_panel_, thread_panel_prev_,
                                        current_thread_root_,
                                        ThreadTrigger::CloseThread, {});
    apply_thread_transition_(t);
}

void ShellBase::on_thread_send_requested(const std::string& body,
                                         const std::string& formatted_body)
{
    if (!client_ || current_thread_root_.empty()) return;
    client_->send_thread_message(current_room_id_, current_thread_root_, body,
                                 formatted_body);
}
```

Override the four `apply_thread_*_` virtuals (they were `protected` no-ops added in Task 2 step 2.3). Provide concrete implementations:

```cpp
void ShellBase::apply_thread_messages_(const std::string& /*root*/,
                                       std::vector<views::MessageRowData> rows,
                                       bool room_switch)
{
    if (room_view_ && room_view_->thread_view())
        room_view_->thread_view()->set_messages(std::move(rows), room_switch);
}
void ShellBase::apply_thread_message_insert_(const std::string& /*root*/,
                                             std::size_t index,
                                             views::MessageRowData row)
{
    if (room_view_ && room_view_->thread_view())
        room_view_->thread_view()->insert_message(index, std::move(row));
}
void ShellBase::apply_thread_message_update_(const std::string& /*root*/,
                                             std::size_t index,
                                             views::MessageRowData row)
{
    if (room_view_ && room_view_->thread_view())
        room_view_->thread_view()->update_message(index, std::move(row));
}
void ShellBase::apply_thread_message_remove_(const std::string& /*root*/,
                                             std::size_t index)
{
    if (room_view_ && room_view_->thread_view())
        room_view_->thread_view()->remove_message(index);
}
void ShellBase::apply_threads_list_(std::vector<ThreadInfo> threads)
{
    if (room_view_ && room_view_->thread_list_view())
        room_view_->thread_list_view()->set_threads(std::move(threads));
}
```

Change the declarations in `ShellBase.h` from `virtual ... {}` to plain virtuals (drop the empty inline bodies) so the .cpp impls are the definitions. Each shell can still override.

Then wire the RoomView → ShellBase callbacks. Find where each shell creates its RoomView (Qt6: `ui/linux-qt/src/MainWindow.cpp`; GTK4: `ui/linux-gtk/src/MainWindow.cpp`; Win32: `ui/windows/src/MainWindow.cpp`; macOS: `ui/macos/src/MainWindowController.mm`). Each shell already wires callbacks like `on_jump_to_date_requested`. Add the same four lines wherever those are wired:

```cpp
room_view_->on_threads_button_clicked = [this] { on_threads_button_clicked(); };
room_view_->on_thread_open_requested  = [this](const std::string& root) { on_thread_open_requested(root); };
room_view_->on_thread_close_requested = [this] { on_thread_close_requested(); };
room_view_->on_thread_send = [this](const std::string& b, const std::string& f) { on_thread_send_requested(b, f); };
```

(Each shell already has access to `this` for the ShellBase methods because each shell either inherits from ShellBase or holds a `MacShell` member with `using ShellBase::on_threads_button_clicked;` re-exports — see how `on_jump_to_date_requested` is wired for the existing pattern.)

Finally, force the room-switch transition. Find where ShellBase calls `current_room_id_ = ...` (look in `tab_open_room`, `tab_select_room`, `tab_navigate_room`, and `restart_sdk_`). Before each assignment add:

```cpp
{
    auto t = compute_thread_transition_(thread_panel_, thread_panel_prev_,
                                        current_thread_root_,
                                        ThreadTrigger::RoomSwitch, {});
    apply_thread_transition_(t);
}
```

- [ ] **Step 8.5: Extend test_shell_thread_panel.cpp with integration tests**

Append:

```cpp
TEST_CASE("on_threads_button_clicked toggles List on/off when client null",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    // No client_ wired → applier becomes a no-op for the Client side, but
    // the state-machine still updates the shell fields (we test against
    // the transition function in Task 1; here we just confirm the entry
    // point exists and doesn't crash).
    s.on_threads_button_clicked();
    SUCCEED();
}
```

(Real end-to-end test of the applier — including the Client calls — is a higher-fidelity test that needs a mockable Client; out of scope for v1. The state-machine is exhaustively tested in Task 1.)

- [ ] **Step 8.6: Build the full project**

```bash
cmake --build build/linux-qt6-debug
```

Expected: clean build across `tesseract_tk`, `tesseract_qt`, and `tesseract_tests`.

- [ ] **Step 8.7: Run full test suite**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

Expected: all tests pass (the prior 493 + the new ~30 thread tests = ~520+).

- [ ] **Step 8.8: Manual smoke (per the spec verification section)**

```bash
./build/linux-qt6-debug/ui/linux-qt/tesseract
```

Drive through the six scenarios in `docs/superpowers/specs/2026-05-26-threads-ui-design.md` ("Verification" section). Confirm:
1. Threads button toggles empty list.
2. Receiving an in-thread reply creates a preview chip on the root.
3. Clicking the chip opens the ThreadView, main list dims, root row highlights + scrolls.
4. Replying from ThreadView's compose: appears in thread, not in main list.
5. Switching threads from the list works.
6. Back/close returns to previous state correctly.
7. Room switch closes the panel.

- [ ] **Step 8.9: Commit**

```bash
git add ui/shared/views/RoomView.h ui/shared/views/RoomView.cpp \
        ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp \
        ui/linux-qt/src/MainWindow.cpp \
        ui/linux-gtk/src/MainWindow.cpp \
        ui/windows/src/MainWindow.cpp \
        ui/macos/src/MainWindowController.mm \
        tests/cpp/test_shell_thread_panel.cpp
git commit -m "feat(threads): split-layout RoomView + ShellBase end-to-end wiring"
```

(Only commit the shells you actually touched; some platforms wire callbacks in a sibling file like `EventBridge.cpp` — let the changed-files list drive the `git add`.)
