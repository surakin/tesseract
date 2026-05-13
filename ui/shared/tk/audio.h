#pragma once

// Per-platform audio playback abstraction. Used by the shared MessageListView
// to play MSC3245 voice messages without the views directly depending on
// QMediaPlayer / GStreamer / AVAudioPlayer / Media Foundation.
//
// Lifetime: caller (the view) owns the player and decides when to destroy it.
// Each `play()` call replaces any currently-playing clip.
//
// Threading: implementations must invoke `on_progress` on the UI thread by
// piping through the parent `tk::Host::post_to_ui` channel. Callers can
// freely mutate widget state from that callback.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace tk {

class AudioPlayer {
public:
    virtual ~AudioPlayer() = default;

    // Begin playback of `data[0..size)`. `mime` is a hint for backends that
    // need format selection (e.g. "audio/ogg"); empty when unknown. The
    // bytes are copied into a backend-owned buffer, so the caller may free
    // them immediately after this returns. Replaces any currently-playing
    // clip with no transition.
    virtual void play  (const std::uint8_t* data,
                        std::size_t          size,
                        std::string_view     mime) = 0;

    virtual void pause () = 0;
    virtual void resume() = 0;
    virtual void stop  () = 0;

    // Jump to `ms` within the currently-loaded clip. No-op when nothing
    // is loaded. Implementations clamp to [0, duration_ms()].
    virtual void seek  (std::uint64_t ms) = 0;

    // Speed control. `1.0` is normal speed; common UI affordances cycle
    // through 1.0 / 1.5 / 2.0. Implementations clamp to a sensible range
    // (typically 0.5..3.0). Applied to the currently-loaded clip and to
    // every subsequent `play()`.
    virtual void set_playback_rate(float rate) = 0;
    virtual float playback_rate() const = 0;

    // Current playback offset in milliseconds. 0 when stopped or before the
    // backend has reported a position.
    virtual std::uint64_t position_ms() const = 0;
    // Duration of the currently-loaded clip in milliseconds, as discovered
    // by the backend. 0 until the backend has decoded the header.
    virtual std::uint64_t duration_ms() const = 0;
    // True while a clip is loaded and actively progressing. False after
    // pause(), stop(), end-of-stream, or a load error.
    virtual bool          is_playing () const = 0;

    // Fired on the UI thread roughly every 60 ms while playing, and once
    // more on completion / error. Implementations marshal through
    // `tk::Host::post_to_ui` before invoking. Safe to read position_ms() /
    // is_playing() inside.
    std::function<void()> on_progress;
};

} // namespace tk
