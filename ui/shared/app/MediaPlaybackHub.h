#pragma once

// MediaPlaybackHub — the "now playing" surface MPRIS needs. tk::AudioPlayer
// (via TimelineMediaController) already exposes Play/Pause/Seek/Position/
// Duration for the in-timeline voice-message/audio player, but nothing above
// it exposes "what event/room is currently loaded, with what title/artist"
// in a row-agnostic way — that's what this class adds.
//
// Process-wide (owned by AccountManager, one instance per process, like
// SearchBackend): only one clip can be "the" MPRIS track at a time, so state
// and its controls always travel together in one report() call — whichever
// ShellBase most recently reported "owns" Play/Pause/Seek until either
// another ShellBase reports, or report_stopped() clears it. This is a
// deliberate v1 simplification: two pop-out windows playing simultaneously
// is rare, and "last one to tick wins" is far simpler than arbitrating
// ownership.
//
// Harmlessly inert on Windows/macOS: nothing there ever calls report()
// (Win32 has no audio backend at all — see MessageListView.h — and macOS/
// Windows have no MPRIS adapter to consume current()).

#include <cstdint>
#include <functional>
#include <string>

namespace tesseract
{

struct NowPlaying
{
    enum class Kind
    {
        None,
        Voice,
        Audio,
        Video
    };
    Kind          kind = Kind::None;
    std::string   room_id;
    std::string   event_id;  // doubles as the MPRIS trackid seed
    std::string   title;     // body/filename, falls back to "Voice message"
    std::string   artist;    // sender display name
    std::uint64_t position_ms = 0;
    std::uint64_t duration_ms = 0;
    bool          is_playing  = false;
};

class MediaPlaybackHub
{
public:
    using Control = std::function<void()>;
    using SeekFn  = std::function<void(std::int64_t offset_ms)>;

    // Bundled with the NowPlaying report they act on, so Play/Pause/Seek
    // always target whichever ShellBase most recently reported — never a
    // stale controller from a window that has since switched rooms or
    // stopped playback.
    struct Controls
    {
        Control play;
        Control pause;
        Control play_pause;
        Control stop;
        SeekFn  seek; // relative, per MPRIS Player.Seek's offset semantics
    };

    // Called by ShellBase on every track-start/play/pause/seek/progress tick.
    // `controls` defaults to empty for callers (tests) that only care about
    // current() state, not about dispatching Play/Pause/Seek.
    void report(NowPlaying state, Controls controls = {});
    // Clears current()/controls (nothing playing app-wide).
    void report_stopped();

    void play() const
    {
        if (controls_.play)
            controls_.play();
    }
    void pause() const
    {
        if (controls_.pause)
            controls_.pause();
    }
    void play_pause() const
    {
        if (controls_.play_pause)
            controls_.play_pause();
    }
    void stop() const
    {
        if (controls_.stop)
            controls_.stop();
    }
    void seek(std::int64_t offset_ms) const
    {
        if (controls_.seek)
            controls_.seek(offset_ms);
    }

    const NowPlaying& current() const
    {
        return current_;
    }

    // Fired (UI-thread) after every report()/report_stopped() call whose
    // NowPlaying differs from the previous one. MPRIS adapters subscribe
    // here to emit PropertiesChanged.
    std::function<void()> on_changed;

private:
    NowPlaying current_;
    Controls   controls_;
};

} // namespace tesseract
