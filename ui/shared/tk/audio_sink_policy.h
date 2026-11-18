#pragma once

#include <chrono>

namespace tk
{

/// When a playback backend should rebuild an audio sink the system has dropped (the output device
/// went away, the audio server restarted the stream). A healthy sink is never rebuilt; a dead one is
/// rebuilt at once the first time and then at most once per `backoff`, so a missing device does not
/// make every 10 ms frame retry.
class SinkRebuildPolicy
{
public:
    using Clock = std::chrono::steady_clock;

    explicit SinkRebuildPolicy(std::chrono::milliseconds backoff = std::chrono::seconds(1))
        : backoff_(backoff)
    {
    }

    /// True when the caller should try to rebuild the sink now; the attempt is counted.
    bool should_rebuild(bool healthy, Clock::time_point now)
    {
        if (healthy)
        {
            tried_ = false;
            return false;
        }
        if (tried_ && now - last_attempt_ < backoff_)
        {
            return false;
        }
        tried_ = true;
        last_attempt_ = now;
        return true;
    }

private:
    std::chrono::milliseconds backoff_;
    Clock::time_point last_attempt_{};
    bool tried_ = false;
};

} // namespace tk
