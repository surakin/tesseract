#pragma once

// ThreadPanelController — the thread-panel state machine extracted from
// ShellBase. It owns:
//   * the panel-mode / trigger enums and the ThreadTransition value type,
//   * the PURE `compute_transition()` function (no Client / UI calls — safe to
//     unit-test directly), and
//   * the thread-list backfill pagination guards (`reached_start_` /
//     `paginating_`) plus the `paginate()` driver, whose side-effects
//     (the background paginate_room_threads call + the re-arm decision) are
//     injected as std::function wiring so timing/behaviour are preserved
//     exactly.
//
// ShellBase still owns `thread_panel_` / `thread_panel_prev_` /
// `current_thread_root_` (the four native shells read `thread_panel_` and
// `current_thread_root_` directly, and macOS pulls them in via `using`), so
// those live on ShellBase unchanged. ShellBase exposes `ShellBase::ThreadPanel`
// / `ThreadTrigger` / `ThreadTransition` as `using` aliases of the types here
// and keeps a thin static `compute_thread_transition_` forwarder so existing
// shells and tests compile untouched.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tesseract
{

class ThreadPanelController
{
public:
    enum class ThreadPanel
    {
        Closed,
        List,
        Open,
    };

    enum class ThreadTrigger
    {
        ToggleList,   // RoomHeader threads button
        OpenFromList, // row click in ThreadListView
        OpenFromMain, // preview chip click in MessageListView
        CloseThread,  // back/close in ThreadView
        RoomSwitch,   // current_room_id_ about to change
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
    static ThreadTransition compute_transition(ThreadPanel cur, ThreadPanel prev,
                                               const std::string& current_root,
                                               ThreadTrigger trigger,
                                               const std::string& trigger_root);

    // ── Thread-list backfill pagination ──────────────────────────────────────
    // Injected: kicks paginate_room_threads() on the background thread and,
    // once it returns, reports reached_start back into on_paginate_result().
    // ShellBase wires this through run_async_mut_ + post_to_ui_alive_, guarding
    // on the live client/room so a stale continuation no-ops.
    void set_run_paginate(std::function<void()> f)
    {
        run_paginate_ = std::move(f);
    }

    bool reached_start() const { return reached_start_; }
    bool paginating() const { return paginating_; }

    // Reset the backfill guards (called on every room switch: each new room
    // starts with an unknown thread history, so pagination is allowed again).
    void reset_backfill()
    {
        reached_start_ = false;
        paginating_    = false;
    }

    // Re-arm backfill without clearing the in-flight guard. Called when the
    // List panel (re)opens so a shrunken service window can re-paginate.
    void rearm_backfill() { reached_start_ = false; }

    // Begin a pagination pass: returns false (and does nothing) when one is
    // already in flight, the history start was reached, or there is no work to
    // do per the caller's precondition. On true, marks paginating and fires the
    // injected runner.
    bool begin_paginate(bool can_paginate);

    // Called on the UI thread after a paginate pass completes. `reached` is the
    // SDK's reached_start flag; `want_more` is true when the panel is still in
    // List mode (so we should keep backfilling). Returns true when another
    // pagination pass should be kicked.
    bool on_paginate_result(bool reached, bool want_more);

private:
    bool reached_start_ = false; // SDK reported no older threads remain
    bool paginating_    = false; // a paginate pass is in flight
    std::function<void()> run_paginate_;
};

} // namespace tesseract
