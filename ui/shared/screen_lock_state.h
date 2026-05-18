#pragma once
#include <atomic>

namespace tesseract::screenlock
{

// logind session D-Bus identifiers shared by the Qt6/GTK4 probes.
inline constexpr const char* kLogindService = "org.freedesktop.login1";
inline constexpr const char* kSessionIface = "org.freedesktop.login1.Session";
// "auto" resolves to the calling process's own session.
inline constexpr const char* kSessionPath =
    "/org/freedesktop/login1/session/auto";

// Best-effort locked state: defaults to UNLOCKED (false) when the hint is
// unavailable, matching the prior per-shell policy.
class State
{
public:
    bool is_locked() const
    {
        return locked_.load(std::memory_order_relaxed);
    }
    void set_initial(bool v)
    {
        locked_.store(v, std::memory_order_relaxed);
    }
    void on_lock()
    {
        locked_.store(true, std::memory_order_relaxed);
    }
    void on_unlock()
    {
        locked_.store(false, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> locked_{false};
};

} // namespace tesseract::screenlock
