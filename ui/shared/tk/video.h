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

namespace tk
{

class VideoPlayer
{
public:
    virtual ~VideoPlayer() = default;

    // Load and begin playback of `data[0..size)`. `mime` is a format hint
    // ("video/mp4", "video/webm", …). Bytes are copied into a backend-owned
    // buffer. Replaces any currently-playing clip with no transition.
    virtual void play(const std::uint8_t* data, std::size_t size,
                      std::string_view mime) = 0;

    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;

    // Jump to `ms` within the currently-loaded clip. Implementations clamp
    // to [0, duration_ms()].
    virtual void seek(std::uint64_t ms) = 0;

    // Playback speed. Common affordance cycles 1.0 / 1.5 / 2.0.
    // Implementations clamp to a sensible range.
    virtual void set_playback_rate(float rate) = 0;
    virtual float playback_rate() const = 0;

    // Loop: restart the clip at end-of-stream when true.
    // Default no-op — backends that can't loop degrade gracefully.
    virtual void set_loop(bool)
    {
    }

    // Muted: silence the audio track when true.
    // Default no-op — backends that can't mute degrade gracefully.
    virtual void set_muted(bool)
    {
    }

    virtual std::uint64_t position_ms() const = 0;
    virtual std::uint64_t duration_ms() const = 0;
    virtual bool is_playing() const = 0;

    // ── Progressive/streaming playback ──────────────────────────────────
    // Alternative to play() for bytes that arrive incrementally instead of
    // all at once. Replaces any currently-playing clip, same as play().
    // Contract:
    //   1. begin_stream(mime, total_size_hint) — call once. total_size_hint
    //      is the declared content length if known, 0 otherwise. Returns
    //      true if the backend accepted streaming mode; false means the
    //      backend has NO streaming support (the default) and the caller
    //      must buffer bytes itself and call play() once complete instead.
    //   2. feed_chunk(data, size) — zero or more times, in arrival order,
    //      UI thread only. Must return quickly (append-only); never blocks
    //      on decode/network.
    //   3. Exactly one terminator: end_stream() (all bytes delivered) or
    //      fail_stream(reason) (the fetch itself failed).
    // A backend that accepted streaming (returned true) may still discover
    // mid-stream that progressive playback won't work for this particular
    // file (e.g. a non-fast-start MP4 whose index box is at the end). That
    // fallback must be internal and transparent: keep every fed chunk in a
    // growable buffer regardless, and if the live/pipeline path is given up
    // on, re-drive through the equivalent of play() once enough bytes exist.
    // on_frame/on_progress/on_error keep their existing meaning; only a
    // genuine decode/stream error raises on_error — "still waiting for more
    // bytes" must not.
    //
    // Threading: feed_chunk/end_stream/fail_stream are called on the UI
    // thread, same as play(). Backends marshal to their own decode/network
    // threads exactly as play() already does.
    virtual bool begin_stream(std::string_view /*mime*/,
                              std::uint64_t /*total_size_hint*/)
    {
        return false;
    }
    virtual void feed_chunk(const std::uint8_t*, std::size_t)
    {
    }
    virtual void end_stream()
    {
    }
    virtual void fail_stream(std::string_view /*reason*/)
    {
        if (on_error)
        {
            on_error();
        }
    }

    // Update the stream's known total size once the fetch layer learns it
    // (e.g. an HTTP Content-Length header) — distinct from begin_stream()'s
    // total_size_hint because the real length is typically not known until
    // after the fetch has already started delivering chunks. Backends that
    // report a growing/partial length in the meantime (rather than this
    // real final one) risk their decoder concluding it has caught up to
    // EOF and giving up early. Safe to call repeatedly with the same value;
    // a no-op default for backends without streaming support.
    virtual void set_stream_length(std::uint64_t /*total_size*/)
    {
    }

    // Current decoded video frame, or nullptr before the first frame has
    // been produced. Only call from the UI thread (inside paint()).
    virtual const tk::Image* current_frame() const = 0;

    // Fired on the UI thread when a new video frame is ready. The overlay
    // calls request_repaint() inside.
    std::function<void()> on_frame;

    // Fired on the UI thread roughly every 60 ms while playing, plus once
    // on end-of-stream / error. Same contract as AudioPlayer::on_progress.
    std::function<void()> on_progress;

    // Fired on the UI thread when the backend encounters a fatal decode or
    // format error and no frames will be produced. After this fires,
    // current_frame() stays nullptr.
    std::function<void()> on_error;
};

} // namespace tk
