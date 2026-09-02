#include "PowerPolicy.h"

#include <utility>

namespace tesseract
{

PowerPolicy::PowerPolicy()
    : now_([] { return Clock::now(); })
{
}

PowerPolicy::PowerPolicy(NowProvider now)
    : now_(std::move(now))
{
}

bool PowerPolicy::target_() const
{
    switch (pref_)
    {
    case Pref::Off:
        return false;
    case Pref::On:
        return true;
    case Pref::Auto:
        return os_saver_ || on_battery_;
    }
    return false;
}

void PowerPolicy::set_pref(Pref p)
{
    pref_ = p;
    // Explicit user action — no debounce, and any armed one is abandoned.
    pending_.reset();
    commit_(target_());
}

void PowerPolicy::notify_os_power_saver(bool active)
{
    os_saver_ = active;
    reevaluate_signals_();
}

void PowerPolicy::notify_on_battery(bool discharging)
{
    on_battery_ = discharging;
    reevaluate_signals_();
}

void PowerPolicy::reevaluate_signals_()
{
    if (pref_ != Pref::Auto)
    {
        // Signals are irrelevant under an explicit On/Off preference.
        pending_.reset();
        return;
    }

    const bool want = target_();
    if (want == active_)
    {
        // Already where we want to be — cancel any pending flip (this is the
        // "signal reverted before the debounce settled" path).
        pending_.reset();
        return;
    }

    // Arm (or re-arm, if the target changed) the debounce.
    if (!pending_ || *pending_ != want)
    {
        pending_ = want;
        pending_since_ = now_();
    }
}

void PowerPolicy::notify_tick()
{
    if (!pending_)
    {
        return;
    }
    if ((now_() - pending_since_) >= debounce_)
    {
        const bool want = *pending_;
        pending_.reset();
        commit_(want);
    }
}

void PowerPolicy::commit_(bool want)
{
    if (want == active_)
    {
        return;
    }
    active_ = want;
    if (on_mode_change)
    {
        on_mode_change(active_);
    }
}

} // namespace tesseract
