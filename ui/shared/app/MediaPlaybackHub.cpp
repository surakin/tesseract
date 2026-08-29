#include "app/MediaPlaybackHub.h"

#include <utility>

namespace tesseract
{

namespace
{
// Position ticks alone don't warrant a PropertiesChanged signal (MPRIS
// clients poll Position on demand rather than via the signal, to avoid
// D-Bus traffic on every ~60ms progress tick) — only track/play-state
// changes do.
bool meaningfully_changed(const NowPlaying& a, const NowPlaying& b)
{
    return a.kind != b.kind || a.event_id != b.event_id ||
           a.room_id != b.room_id || a.is_playing != b.is_playing;
}
} // namespace

void MediaPlaybackHub::report(NowPlaying state, Controls controls)
{
    const bool changed = meaningfully_changed(current_, state);
    current_  = std::move(state);
    controls_ = std::move(controls);
    if (changed && on_changed)
        on_changed();
}

void MediaPlaybackHub::report_stopped()
{
    const bool was_playing = current_.kind != NowPlaying::Kind::None;
    current_  = NowPlaying{};
    controls_ = Controls{};
    if (was_playing && on_changed)
        on_changed();
}

} // namespace tesseract
