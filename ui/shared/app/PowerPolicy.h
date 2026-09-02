#pragma once

// App-level "low power mode" decision state machine.
//
// Resolves three inputs into a single boolean "low power mode is active":
//   • the user preference (Auto / On / Off),
//   • whether the OS energy-saver / battery-saver profile is on,
//   • whether the machine is on battery and discharging.
//
// Policy:
//   Off → never active.
//   On  → always active.
//   Auto → active whenever (os_power_saver || on_battery).
//
// Setting the preference (an explicit user action) applies immediately.
// Signal-driven changes while in Auto are debounced (~20 s) so a brief unplug
// to move desks, or power-profiles-daemon churn, doesn't thrash the whole
// suspend/resume machinery. If the signal returns to its prior value before the
// debounce settles, the pending flip is dropped with no emit.
//
// Pure state-machine logic — no threading, no OS calls. The shell drives it
// from the IPowerMonitor callbacks and from a periodic `notify_tick()` (~30 s,
// the same tick that drives PresenceTracker); when a debounce is armed the
// shell also schedules a one-shot tick so entering low power doesn't wait for
// the next periodic one. `on_mode_change` fires only when the resolved state
// actually flips.
//
// Tests inject a fake clock via the constructor's `now` parameter.

#include <chrono>
#include <functional>
#include <optional>

namespace tesseract
{

class PowerPolicy
{
public:
    enum class Pref
    {
        Auto,
        On,
        Off,
    };

    using Clock = std::chrono::steady_clock;
    using NowProvider = std::function<Clock::time_point()>;

    PowerPolicy();
    explicit PowerPolicy(NowProvider now);

    // --- Inputs ---

    /// User preference. Applies immediately (clears any armed debounce).
    void set_pref(Pref p);

    /// OS energy-saver / battery-saver profile toggled.
    void notify_os_power_saver(bool active);

    /// On-battery / discharging state changed.
    void notify_on_battery(bool discharging);

    /// Periodic tick (~30 s). Commits a settled debounced flip.
    void notify_tick();

    // --- Output ---

    /// Fires exactly once per resolved transition.
    std::function<void(bool active)> on_mode_change;

    // --- Inspection ---

    bool active() const { return active_; }
    Pref pref() const { return pref_; }
    bool has_pending() const { return pending_.has_value(); }
    std::chrono::seconds debounce() const { return debounce_; }

private:
    bool target_() const;          // pure truth table over pref_ + signals
    void reevaluate_signals_();     // arm / cancel the debounce (Auto only)
    void commit_(bool want);        // set active_ + emit on change

    NowProvider now_;
    Pref pref_ = Pref::Auto;
    bool os_saver_ = false;
    bool on_battery_ = false;
    bool active_ = false;
    std::optional<bool> pending_;
    Clock::time_point pending_since_{};
    std::chrono::seconds debounce_{20};
};

} // namespace tesseract
