#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace tk
{

// Eases a scalar value toward a target over a fixed duration, entirely in
// terms of wall-clock time — callers don't thread "now"/"dt" through paint
// code themselves. Call step() once per paint to advance by the time
// elapsed since the previous call and read the current eased value; keep
// requesting repaints (via whatever repaint hook the caller has) while
// still_animating() is true. Mirrors the elapsed-time-per-paint idiom the
// loading spinners already use (see draw_spinner_dots in loading_spinner.h)
// for the analogous "looping" side of animation — this is the equivalent
// building block for short one-shot transitions (hover/selection fades,
// etc).
//
// Not thread-safe; an instance is owned by (and stepped from) one widget's
// UI-thread paint path, the same assumption every other tk:: paint helper
// makes.
class FloatTween
{
public:
    explicit FloatTween(float initial = 0.0f) : current_(initial), target_(initial)
    {
    }

    // Retarget the animation; a no-op if `target` already matches the
    // current target. Safe to call every paint with the caller's desired
    // steady-state value (e.g. `hovered ? 1.0f : 0.0f`) — only an actual
    // change starts easing, so callers don't need to track "did it change"
    // themselves, and retargeting mid-flight blends smoothly from the
    // current value rather than snapping.
    void set_target(float target)
    {
        target_ = target;
    }

    // Snaps both the current value and the target to `value` and forgets
    // the elapsed-time clock, so the next step() starts a fresh ease from
    // `value` instead of continuing whatever was in flight. Used to
    // restart a one-shot entrance animation (e.g. a popup reopening) —
    // set_target() alone won't do this since it deliberately blends from
    // wherever the value currently sits.
    void reset(float value)
    {
        current_ = value;
        target_  = value;
        last_step_.reset();
    }

    float target() const
    {
        return target_;
    }

    float value() const
    {
        return current_;
    }

    bool still_animating() const
    {
        return current_ != target_;
    }

    // Advances by the time elapsed since the previous step() call (measured
    // internally via steady_clock) and returns the new current value.
    // `duration_ms` is how long a full 0<->1 excursion takes; a partial
    // excursion (already close to the target) takes proportionally less
    // time, matching how real easing settles. The first call after
    // construction — or after a long gap where step() wasn't called, e.g.
    // the widget stopped being painted — never jumps: it seeds the clock
    // and returns the value unchanged, so the very next call starts a
    // normal, visible ease instead of a single huge step from a stale
    // timestamp.
    float step(float duration_ms = 120.0f)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!last_step_)
        {
            last_step_ = now;
            return current_;
        }
        const float dt_ms =
            std::chrono::duration<float, std::milli>(now - *last_step_).count();
        last_step_ = now;
        if (current_ == target_ || duration_ms <= 0.0f)
        {
            current_ = target_;
            return current_;
        }
        // Clamp dt so a long pause since the last step (widget not
        // repainted for a while) can't produce one huge jump.
        const float delta = std::clamp(dt_ms, 0.0f, 48.0f) / duration_ms;
        current_ = current_ < target_ ? std::min(target_, current_ + delta)
                                       : std::max(target_, current_ - delta);
        return current_;
    }

private:
    float current_;
    float target_;
    std::optional<std::chrono::steady_clock::time_point> last_step_;
};

} // namespace tk
