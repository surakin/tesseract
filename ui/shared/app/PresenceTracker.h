#pragma once

// App-level Matrix presence state machine.
//
// The Matrix spec lets a client publish one of three presence states:
// Online, Unavailable ("away"), or Offline. Tesseract's policy is:
//   • notify_sync_started → Online when starting fresh.
//   • Any pointer/keyboard input into our window → keep / restore Online.
//   • Window focus loss + no input for `idle_threshold()` → Unavailable.
//   • Window focus regained → force Online immediately.
//   • notify_logout → Offline (called before stop_sync).
//
// The tracker is pure state-machine logic — no threading, no OS calls. The
// shell drives it from native input / focus handlers and from a periodic
// `notify_tick()` call (~every 30 s); the shell wires `on_state_change` to a
// fire-and-forget worker that calls `tesseract::Client::set_presence`. Only
// emits `on_state_change` when the *resolved* state actually flips so the
// homeserver doesn't see chatty rewrites of the same state.
//
// Tests inject a fake clock via the constructor's `now_provider` parameter.

#include <chrono>
#include <functional>

namespace tesseract
{

class PresenceTracker
{
public:
    enum class State
    {
        Offline,
        Online,
        Unavailable,
    };

    using Clock = std::chrono::steady_clock;
    using NowProvider = std::function<Clock::time_point()>;

    /// Construct with the default `steady_clock::now` source.
    PresenceTracker();
    /// Construct with a test-injected clock.
    explicit PresenceTracker(NowProvider now);

    // --- Inputs from the shell ---

    /// Pointer / keyboard activity inside our window. Restores Online from
    /// Unavailable and updates the last-activity timestamp.
    void notify_input();

    /// Window focus gained / lost. Focus-gained forces Online immediately;
    /// focus-lost only flips the internal bit (the next `notify_tick` that
    /// finds an expired idle deadline transitions to Unavailable).
    void notify_window_active(bool active);

    /// Sync has reached `RoomListState::Running` for the first time —
    /// publish the initial Online state. Idempotent.
    void notify_sync_started();

    /// User is logging out / quitting — force Offline.
    void notify_logout();

    /// Periodic tick (~30 s). Checks the idle deadline.
    void notify_tick();

    // --- Output ---

    /// Fires exactly once per resolved state transition.
    std::function<void(State)> on_state_change;

    // --- Inspection ---

    State current() const { return current_; }
    std::chrono::minutes idle_threshold() const { return idle_threshold_; }
    bool window_active() const { return window_active_; }

private:
    void transition_to_(State target);

    NowProvider now_;
    State current_ = State::Offline;
    bool window_active_ = true;
    bool sync_started_ = false;
    Clock::time_point last_activity_;
    std::chrono::minutes idle_threshold_{5}; // matches Element's default
};

} // namespace tesseract
