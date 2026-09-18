#pragma once

#include "canvas.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace tk
{

// Shared tuning constants for animated-image (GIF/WebP/APNG) decode, used by
// every platform's whole-batch and windowed decode paths alike. Centralized
// here (rather than redeclared per-platform) per CLAUDE.md's "Shared vs
// Platform Code" rule — none of these values are platform-dependent.

// Hard cap on frames decoded for any single animation, windowed or not —
// guards against runaway/never-ending encodes blowing memory.
inline constexpr int kAnimDecodeMaxFrames = 200;

// Animations with more than this many frames get a windowed decode session
// instead of being decoded whole up front. GTK4's gdk-pixbuf exposes no
// upfront total-frame-count API, so its windowed path applies unconditionally
// rather than consulting this threshold — see GtkAnimSession's doc comment.
inline constexpr int kAnimDecodeWindowThresholdFrames = 40;

// Frame count for a windowed session's initial (synchronous) decode batch,
// before the rest stream in via on-demand top-ups.
inline constexpr int kAnimDecodeInitialBatchFrames = 8;

// Normalizes a codec-reported per-frame delay (ms): non-positive values
// (missing/zero delay) fall back to 100ms; values under 20ms are floored to
// 20ms, matching what browsers do to keep tight-loop GIFs from burning CPU
// on encoders that wrote 0ms.
inline int normalize_frame_delay_ms(int raw_delay_ms)
{
    if (raw_delay_ms <= 0)
        return 100;
    if (raw_delay_ms < 20)
        return 20;
    return raw_delay_ms;
}

// Resumable, per-animation decode session backing a windowed
// AnimImageCache entry. Each platform backend implements this over its own
// native incremental image decoder (WIC / ImageIO / QImageReader /
// gdk-pixbuf) so a long animation's frames can be decoded a few at a time —
// "current frame + a short lookahead" — instead of the whole animation being
// decoded up front and held in memory for its entire lifetime.
//
// Ownership: AnimImageCache::Entry holds this via shared_ptr, not
// unique_ptr. AnimImageCache::collect_topups() hands out a *copy* of that
// shared_ptr to the caller (ShellBase), which runs decode_next_batch() on a
// worker thread — so the session instance, and the native decoder handle it
// wraps, must stay alive and safely callable even if the owning cache entry
// is evicted (sweep()/clear()) while a batch is in flight. Implementations
// must not reach back into AnimImageCache or touch anything UI-thread-owned;
// results are delivered purely through the on_frame callback, which the
// caller (not the session) is responsible for marshalling back to the UI
// thread and feeding to AnimImageCache::append_frame().
//
// Thread confinement, not thread safety: implementations do NOT need to
// guard their own state (cursor, decoder handles, ...) against concurrent
// calls — AnimImageCache never calls exhausted()/restart() while a
// decode_next_batch() for the same session is in flight (it uses its own
// mutex-guarded `topping_up` flag to serialize this, see
// AnimImageCache::collect_topups()/finish_topup()). All three methods can
// still each land on a *different* thread over the session's lifetime
// (decode_next_batch() on whichever worker thread ran it, exhausted() and
// restart() on the UI thread), so plain (non-atomic) member state is fine as
// long as nothing is cached in thread-local/TLS form across calls.
class AnimDecodeSession
{
public:
    virtual ~AnimDecodeSession() = default;

    // Decode up to `n` more frames starting at the session's internal
    // cursor, invoking on_frame(absolute_frame_index, image, delay_ms) for
    // each frame as it is produced (in order). Returns the number of frames
    // actually produced, which is < n once the session is exhausted.
    // MUST be safe to call on a worker thread: no UI-thread or
    // device-context access.
    virtual int decode_next_batch(
        int n,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>&
            on_frame) = 0;

    // True once decode_next_batch() can produce no further frames (the
    // session has reached the end of the animation).
    virtual bool exhausted() const = 0;

    // Rewind to frame 0 and clear exhausted(), so the next
    // decode_next_batch() call produces frame 0 again. Sequential decoders
    // (GIF disposal compositing, forward-only readers) re-parse from
    // retained source bytes; random-access decoders just reset the cursor.
    // Safe to call on a worker thread, same contract as decode_next_batch().
    virtual void restart() = 0;

    // Approximate resident size of state the session retains for the life of
    // its cache entry (the encoded source bytes it re-parses on restart(),
    // compositing canvases, decoder buffers) — NOT the decoded frames, which
    // AnimImageCache counts itself. Called on the UI thread.
    virtual std::size_t memory_bytes() const { return 0; }
};

} // namespace tk
