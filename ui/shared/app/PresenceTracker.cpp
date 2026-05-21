#include "PresenceTracker.h"

#include <utility>

namespace tesseract
{

PresenceTracker::PresenceTracker()
    : now_([] { return Clock::now(); })
    , last_activity_(now_())
{
}

PresenceTracker::PresenceTracker(NowProvider now)
    : now_(std::move(now))
    , last_activity_(now_())
{
}

void PresenceTracker::notify_input()
{
    last_activity_ = now_();
    // If we'd decayed to Unavailable, fresh input bumps us back to Online.
    // Window-active state is irrelevant — keyboard input through some clever
    // accessibility tool while the window is technically inactive still means
    // the user is engaging with our app.
    if (current_ == State::Unavailable)
    {
        transition_to_(State::Online);
    }
}

void PresenceTracker::notify_window_active(bool active)
{
    window_active_ = active;
    if (active)
    {
        last_activity_ = now_();
        if (sync_started_ && current_ != State::Offline)
        {
            transition_to_(State::Online);
        }
    }
    // When focus is lost we leave `current_` untouched. The next
    // notify_tick() that finds an expired idle deadline does the decay.
}

void PresenceTracker::notify_sync_started()
{
    sync_started_ = true;
    last_activity_ = now_();
    transition_to_(State::Online);
}

void PresenceTracker::notify_logout()
{
    sync_started_ = false;
    transition_to_(State::Offline);
}

void PresenceTracker::notify_tick()
{
    if (!sync_started_ || current_ == State::Offline)
    {
        return;
    }
    if (window_active_)
    {
        // Foregrounded means the user is around — never decay.
        return;
    }
    if ((now_() - last_activity_) >= idle_threshold_)
    {
        transition_to_(State::Unavailable);
    }
}

void PresenceTracker::transition_to_(State target)
{
    if (current_ == target)
    {
        return;
    }
    current_ = target;
    if (on_state_change)
    {
        on_state_change(target);
    }
}

} // namespace tesseract
