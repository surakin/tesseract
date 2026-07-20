#pragma once

#include <chrono>
#include <optional>

namespace tk
{

// Trackpad momentum ("kinetic") scrolling. Feed it every wheel sample as it
// arrives via on_wheel_delta(); call step() once per paint afterwards (the
// caller is responsible for keeping paint() ticking — via request_repaint()
// — for as long as active() reports true, the same self-driven idiom
// tk::FloatTween uses for hover/selection fades).
//
// There is no platform gesture-end signal fed in anywhere — this class
// detects "the finger lifted" purely from an idle gap in incoming samples,
// so it behaves identically on every platform regardless of whether the OS
// exposes a native end-of-gesture event.
//
// Only touchpad samples (is_touchpad=true) ever arm a fling. A physical
// mouse-wheel notch (is_touchpad=false) cancels any in-flight fling and
// resets tracking, so mouse-wheel scrolling stays exactly what it is today:
// an instant per-notch jump with no coast.
//
// Headless — no widget/canvas/host dependency — so it's unit-testable in
// isolation and reusable by any scrollable container. ScrollableBase is the
// primary integration point (on_wheel_scroll()); SettingsPage/
// KnownPacksList wire an instance by hand since they don't derive from it.
class KineticScroller
{
public:
    // Record a wheel sample as it arrives.
    void on_wheel_delta(float dy, bool is_touchpad);

    // Advance by the time elapsed since the last step() call. Returns the
    // scroll delta to apply this frame (0 when idle). Apply it exactly like
    // a manual wheel delta (through the same clamping path); if applying it
    // didn't move anything (a scroll bound was hit), call cancel() so the
    // fling stops dead instead of pinning against the bound.
    float step();

    // True once a fling has actually armed (post-release coast in
    // progress) — as opposed to merely watching live samples for a
    // gesture-end.
    bool is_flinging() const
    {
        return flinging_;
    }

    // True while the caller should keep the paint loop alive (watching for
    // gesture-end, or actively flinging).
    bool active() const
    {
        return watching_ || flinging_;
    }

    // Cancel any in-flight fling / live-gesture tracking — new pointer-down,
    // a non-touchpad sample arriving, or the caller hit a clamp bound.
    void cancel();

private:
    struct Sample
    {
        float dy;
        std::chrono::steady_clock::time_point t;
    };

    static constexpr int kMaxSamples = 6;
    // No new sample within this long after the last one means the gesture
    // has ended (finger lifted). This whole window produces zero motion —
    // the wheel has already stopped and the fling hasn't been declared yet
    // — so it directly determines how long the "dead pause" before a coast
    // feels; keep it just above a touchpad's per-event cadence (comfortably
    // below the gap a genuine mouse-wheel spin leaves between notches), not
    // some larger "be extra sure" margin.
    static constexpr float kIdleGapMs = 32.0f;
    // Below this release speed, don't bother — avoids a spurious
    // twitch-fling when a scroll ends with a slow, deliberate lift.
    static constexpr float kMinFlingVelocity = 0.15f; // px/ms
    // Cap so a hard flick can't send content flying absurdly far.
    static constexpr float kMaxFlingVelocity = 4.5f; // px/ms
    // Per-ms exponential decay factor — velocity *= kFriction^dt_ms. Tuned
    // to coast to a stop in roughly half a second to ~2s depending on
    // release speed (feel-tuned, same spirit as FloatTween's durations).
    static constexpr float kFriction = 0.996f;
    // Velocity magnitude below which the fling is considered settled.
    static constexpr float kStopVelocity = 0.02f; // px/ms

    Sample samples_[kMaxSamples];
    int sample_count_ = 0;

    bool watching_ = false;
    bool flinging_ = false;
    float velocity_ = 0.0f; // px/ms
    std::optional<std::chrono::steady_clock::time_point> last_sample_time_;
    std::optional<std::chrono::steady_clock::time_point> last_step_time_;

    void push_sample(float dy, std::chrono::steady_clock::time_point now);
    float estimate_velocity() const;
    void reset_tracking();
};

} // namespace tk
