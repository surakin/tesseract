#pragma once

#include "canvas.h"
#include "tk/anim_decode_session.h"
#include "tk/cache_key.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tk
{

// Per-URL cache of decoded animation frames (GIF / APNG / animated WebP).
// Each entry holds the full decoded frame list and a monotonic deadline for
// the next frame advance. The caller drives frame timing by calling advance()
// from a ~60 Hz platform timer and passing the current time in milliseconds
// from any consistent epoch (wall clock, boot clock, etc. — as long as the
// same source is used for store() and advance()).
//
// Visibility gating: only entries that were actually painted recently (i.e.
// current_frame() was called within the visibility grace window) drive frame
// advances and repaints. This lets the platform timer go idle once no animated
// image is on-screen instead of repainting the whole window forever for media
// that has scrolled off or sits in a different room. Visibility uses a separate
// wall-clock source (steady_clock by default; overridable for tests) so it is
// independent of the frame-timing epoch passed to store()/advance().
//
// Internally synchronised (mirrors tk::CompressedImageCache / tk::PixmapCache):
// every public method locks a private mutex. As with PixmapCache::peek(),
// current_frame() returns a raw, non-owning `const Image*` valid only until
// the next store()/sweep() on this cache — the lock guards the cache's own
// bookkeeping during the call, not that pointer's lifetime afterward. Every
// current_frame()/store() caller in this codebase runs on the UI thread today,
// so this lock is defense in depth rather than a fix for a live race.
//
// For a windowed entry (see store()'s `session` parameter), advance()'s
// internal trim/restart bookkeeping can also drop the exact frame a prior
// current_frame() call returned — the same "re-query every paint, never hold
// a pointer across a tick" usage pattern that already makes store()/sweep()
// safe covers this too.
class AnimImageCache
{
public:
    // `max_bytes` caps total decoded-frame memory; `ttl_ms` is how long an
    // entry survives after it was last painted before sweep() may reclaim it.
    explicit AnimImageCache(std::size_t max_bytes = 64u * 1024u * 1024u,
                            std::int64_t ttl_ms = 30000);

    // Add or replace an animated entry. `now_ms` is used to set the initial
    // frame-advance deadline to `now_ms + delays_ms[0]`. The entry starts out
    // visible so the timer keeps running until its first paint.
    //
    // `session` and `total_frames` opt the entry into windowed playback: when
    // `session` is non-null, `frames` is treated as just the first resident
    // batch of a longer animation rather than the whole thing. advance()
    // then keeps only a short window of frames resident (topping up ahead of
    // the playback cursor via collect_topups(), trimming behind it, and
    // restarting the session to loop back to frame 0) instead of retaining
    // every decoded frame for the entry's lifetime. Leave `session` null (the
    // default) for the existing unwindowed behavior — decode everything up
    // front, retain it all — which remains correct for short animations
    // where windowing isn't worth the complexity.
    void store(const CacheKey& key,
               std::vector<std::unique_ptr<tk::Image>> frames,
               std::vector<int> delays_ms, std::int64_t now_ms,
               std::shared_ptr<AnimDecodeSession> session = nullptr,
               std::size_t total_frames = 0);

    // Append one more frame + delay to an already-stored entry, for streamed
    // decode (see ShellBase::decode_image_streamed_ /
    // make_streamed_decode_callbacks_): the caller store()s frame 0 as soon
    // as it's decoded so something paints
    // immediately, then append_frame()s each subsequent frame as it arrives.
    // Safe no-op if `key` isn't present (the entry was evicted/cleared, e.g.
    // by sweep() or clear(), while the remaining frames were still
    // decoding — those frames are simply dropped on arrival). Safe to call
    // while the entry is mid-playback: advance()/current_frame() re-read
    // frames.size() on every call, so growing the vector never invalidates
    // the current index.
    //
    // Only checks that `key` exists — not that this frame actually came
    // from whatever decode currently owns the entry. That's fine for the
    // plain (unwindowed) streaming path, where store()'s callers already
    // gate the *first* frame on has(key) so a losing redundant decode's
    // frame 0 never overwrites a winning one's entry — but a windowed
    // entry's later frames (top-ups, or extra frames from the initial
    // batch) need the stronger check append_frame_from_session() below
    // provides instead.
    void append_frame(const CacheKey& key, std::unique_ptr<tk::Image> frame,
                      int delay_ms);

    // Like append_frame(), but for a frame produced by a specific
    // AnimDecodeSession's decode_next_batch() call: only appends if
    // `session` is still the entry's *current* session — i.e. no other
    // decode (e.g. a redundant concurrent fetch/decode race for the same
    // key) has since replaced this entry via a fresh store(). Without this,
    // a losing decode's on_extra/top-up frames — which, unlike its frame 0,
    // have no has(key) gate of their own — would silently splice a second,
    // independently re-decoded copy of the animation into the winning
    // entry's frame sequence: visible as the animation appearing to jump
    // backward mid-playback. Safe no-op if the entry is gone or belongs to
    // a different session.
    void append_frame_from_session(const CacheKey& key,
                                   const std::shared_ptr<AnimDecodeSession>& session,
                                   std::unique_ptr<tk::Image> frame,
                                   int delay_ms);

    bool has(const CacheKey& key) const;
    bool empty() const;

    // A pending decode-ahead request for one windowed entry, handed out by
    // collect_topups(). `session` is a copy of the entry's shared_ptr, so it
    // stays valid (and its decode_next_batch()/restart() calls remain safe)
    // even if the entry itself is evicted while the caller's worker-thread
    // decode is in flight.
    struct TopupRequest
    {
        CacheKey key;
        std::shared_ptr<AnimDecodeSession> session;
        int batch_size = 0;
    };

    // Scan visible, windowed entries (session != nullptr) whose resident
    // frame window is running low ahead of the playback cursor — or that
    // just restarted and have no resident frames at all — and are not
    // already topping up. For each, mark it "topping up" (so it isn't
    // requested again until finish_topup()) and return a request the caller
    // should run session->decode_next_batch() on, off the UI thread. Meant
    // to be called once per tick, alongside advance().
    std::vector<TopupRequest> collect_topups();

    // Clear the "topping up" flag for `key` once its decode_next_batch()
    // call has completed (whether it produced any frames or not), so the
    // next collect_topups() can request more. Safe no-op if the entry was
    // evicted while the batch was in flight.
    void finish_topup(const CacheKey& key);

    // Return the current frame for `key`, or nullptr if not found / no frames.
    // Calling this marks the entry as visible (it is on the current paint).
    const tk::Image* current_frame(const CacheKey& key) const;

    // Freeze/unfreeze `key` at its current frame: while paused, advance()
    // leaves the entry's frame index (and, for windowed entries, its
    // resident window) untouched instead of ticking it forward — the entry
    // stays visible (current_frame() still marks it seen, so TTL/sweep()
    // won't evict a frozen-but-displayed image) but simply stops animating
    // until unpaused. Safe no-op if `key` isn't present.
    void set_paused(const CacheKey& key, bool paused);

    // Advance the deadline-expired frames of currently-visible entries. Returns
    // true when at least one *visible* entry's frame index changed (the caller
    // should repaint). Off-screen entries are left untouched and never request
    // a repaint.
    bool advance(std::int64_t now_ms);

    // True when at least one entry was painted within the visibility grace
    // window. Shells use this to decide whether to keep the animation timer
    // running; once it returns false the timer can stop.
    bool any_visible() const;

    // Reclaim memory: drop entries not painted within the TTL window, then —
    // if still over budget — evict the least-recently-seen off-screen entries
    // oldest-first until under budget. Currently-visible entries are kept.
    void sweep();

    // Drop every entry and reset hit/miss accounting. Unlike sweep(),
    // unconditional — used to fully reset the cache (e.g. "clear all media").
    void clear();

    // Sum of memory_bytes() over every resident frame, computed fresh on
    // each call (see entry_bytes_locked_'s doc comment) rather than an
    // incrementally-tracked running total.
    std::size_t current_bytes() const;
    std::size_t max_bytes() const
    {
        return max_bytes_; // immutable after construction — no lock needed
    }
    std::size_t hits() const;
    std::size_t misses() const;

    // Test seam: override the visibility clock (milliseconds). Defaults to a
    // steady_clock source in production.
    void set_clock_for_testing(std::function<std::int64_t()> clock)
    {
        clock_ = std::move(clock);
    }

private:
    // An entry is considered visible if it was painted within this many
    // milliseconds. Generous enough to span the gap between frames of a slow
    // animation (so a visible-but-rarely-repainted GIF is not treated as
    // hidden) while still letting the timer idle ~2s after content scrolls off.
    static constexpr std::int64_t kVisibilityGraceMs = 2000;

    // Windowed-playback tuning: how many already-shown frames to keep behind
    // the cursor (repaint-race safety margin — a paint that read the index
    // just before a trim shouldn't dereference a dropped frame next tick),
    // how many frames of resident lookahead to require before requesting a
    // top-up, and how many frames a single top-up batch decodes.
    static constexpr std::size_t kKeepBehindFrames = 2;
    static constexpr std::size_t kLookaheadFrames = 4;
    static constexpr int kTopupBatchFrames = 8;

    std::int64_t vis_now_() const;

    mutable std::mutex mu_;

    struct Entry
    {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int> delays_ms;
        // Absolute frame index that frames[0] corresponds to. Always 0 for
        // unwindowed entries (session == nullptr); advances as a windowed
        // entry's resident frames are trimmed from behind the cursor.
        std::size_t window_start = 0;
        std::size_t current = 0;
        std::int64_t next_advance_ms = 0;
        // Visibility-clock timestamp of the last current_frame() call.
        mutable std::int64_t last_seen_ms =
            std::numeric_limits<std::int64_t>::min();
        // Non-null opts this entry into windowed playback — see store()'s
        // doc comment.
        std::shared_ptr<AnimDecodeSession> session;
        // Total frame count of the animation, if known (0 = unknown). Purely
        // informational today; not required for correctness.
        std::size_t total_frames = 0;
        // True while a collect_topups() request for this entry is in
        // flight, so it isn't requested again until finish_topup().
        bool topping_up = false;
        // True from restart_loop_locked_() until the new loop's frame 0
        // arrives via append_frame()/append_frame_from_session(): `frames`
        // holds exactly one entry during this window — the last frame
        // actually shown before the restart, kept resident purely so
        // current_frame() keeps returning something (not nullptr / a blank
        // paint) while the session re-decodes frame 0.
        // append_frame_locked_() checks this flag to REPLACE that
        // placeholder instead of appending after it.
        bool restarting = false;
        // True while playback is frozen (e.g. low-power mode, not
        // hovered): advance() skips this entry entirely — no deadline
        // check, no window trim/restart — so current_frame() keeps
        // returning the same frame until unpaused.
        bool paused = false;
    };

    // Sum of memory_bytes() over `entry`'s currently resident frames,
    // computed fresh on every call rather than tracked incrementally: some
    // backends' Image::memory_bytes() is not stable over the object's
    // lifetime (e.g. QtImage memoizes additional pre-scaled copies as the
    // same image is painted at different target sizes elsewhere, so its
    // reported size grows). A running total nudged by +=/-= at add/remove
    // time would drift from — and could even underflow past — what's
    // actually resident once frames start reporting a different size than
    // they did when added. Caller holds mu_.
    static std::size_t entry_bytes_locked_(const Entry& entry);

    // Shared by append_frame() and the internal top-up-delivery path: push
    // one frame onto `entry`. Caller holds mu_.
    void push_frame_locked_(Entry& entry, std::unique_ptr<tk::Image> frame,
                            int delay_ms);

    // Shared by append_frame() and append_frame_from_session(): handles the
    // `restarting` placeholder-replacement case, else defers to
    // push_frame_locked_(). Caller holds mu_ and has already resolved
    // `entry` (including, for append_frame_from_session(), verified the
    // session identity).
    void append_frame_locked_(Entry& entry, std::unique_ptr<tk::Image> frame,
                              int delay_ms);

    // Windowed-entry bookkeeping run from advance(), caller holds mu_:
    // drop frames more than kKeepBehindFrames behind `entry.current`,
    // shifting window_start/current to match.
    void trim_window_locked_(Entry& entry);

    // Windowed-entry bookkeeping run from advance(), caller holds mu_: when
    // playback has reached the last resident frame and the session is
    // exhausted with frame 0 no longer resident (window_start != 0), restart
    // the session and drop all resident frames so collect_topups() re-fills
    // starting at frame 0 on the next tick.
    void restart_loop_locked_(Entry& entry);

    std::unordered_map<CacheKey, Entry, CacheKeyHash> entries_;
    std::function<std::int64_t()> clock_;
    std::size_t max_bytes_;
    mutable std::size_t hits_   = 0;
    mutable std::size_t misses_ = 0;
    std::int64_t ttl_ms_;
};

} // namespace tk
