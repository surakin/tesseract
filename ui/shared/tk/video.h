#pragma once

// Per-platform video playback abstraction. Used by VideoViewerOverlay to
// play m.video events without depending directly on platform media APIs.
//
// Lifetime: caller owns the player. Each play() call replaces any current clip.
//
// Threading: on_frame and on_progress must be invoked on the UI thread.
// Implementations marshal through tk::Host::post_to_ui (or the platform
// equivalent — QTimer on Qt6, g_idle_add on GTK4, etc.).
//
// current_frame() is only called from the UI thread during paint(). Backends
// protect the underlying frame buffer with a mutex or atomic swap so the
// decode thread can write while paint reads.

#include "canvas.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace tk {

class VideoPlayer {
public:
    virtual ~VideoPlayer() = default;

    // Load and begin playback of `data[0..size)`. `mime` is a format hint
    // ("video/mp4", "video/webm", …). Bytes are copied into a backend-owned
    // buffer. Replaces any currently-playing clip with no transition.
    virtual void play  (const std::uint8_t* data,
                        std::size_t          size,
                        std::string_view     mime) = 0;

    virtual void pause () = 0;
    virtual void resume() = 0;
    virtual void stop  () = 0;

    // Jump to `ms` within the currently-loaded clip. Implementations clamp
    // to [0, duration_ms()].
    virtual void seek  (std::uint64_t ms) = 0;

    // Playback speed. Common affordance cycles 1.0 / 1.5 / 2.0.
    // Implementations clamp to a sensible range.
    virtual void  set_playback_rate(float rate) = 0;
    virtual float playback_rate() const = 0;

    // Loop: restart the clip at end-of-stream when true.
    // Default no-op — backends that can't loop degrade gracefully.
    virtual void set_loop (bool) {}

    // Muted: silence the audio track when true.
    // Default no-op — backends that can't mute degrade gracefully.
    virtual void set_muted(bool) {}

    virtual std::uint64_t position_ms () const = 0;
    virtual std::uint64_t duration_ms () const = 0;
    virtual bool          is_playing  () const = 0;

    // Current decoded video frame, or nullptr before the first frame has
    // been produced. Only call from the UI thread (inside paint()).
    virtual const tk::Image* current_frame() const = 0;

    // Fired on the UI thread when a new video frame is ready. The overlay
    // calls request_repaint() inside.
    std::function<void()> on_frame;

    // Fired on the UI thread roughly every 60 ms while playing, plus once
    // on end-of-stream / error. Same contract as AudioPlayer::on_progress.
    std::function<void()> on_progress;
};

} // namespace tk
