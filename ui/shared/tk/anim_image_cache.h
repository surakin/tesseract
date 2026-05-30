#pragma once

#include "canvas.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
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
    void store(const std::string& key,
               std::vector<std::unique_ptr<tk::Image>> frames,
               std::vector<int> delays_ms, std::int64_t now_ms);

    bool has(const std::string& key) const;
    bool empty() const
    {
        return entries_.empty();
    }

    // Return the current frame for `key`, or nullptr if not found / no frames.
    // Calling this marks the entry as visible (it is on the current paint).
    const tk::Image* current_frame(const std::string& key) const;

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

    std::size_t current_bytes() const
    {
        return current_bytes_;
    }
    std::size_t max_bytes() const
    {
        return max_bytes_;
    }
    std::size_t hits() const
    {
        return hits_;
    }
    std::size_t misses() const
    {
        return misses_;
    }

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

    std::int64_t vis_now_() const;

    struct Entry
    {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int> delays_ms;
        std::size_t current = 0;
        std::int64_t next_advance_ms = 0;
        // Visibility-clock timestamp of the last current_frame() call.
        mutable std::int64_t last_seen_ms =
            std::numeric_limits<std::int64_t>::min();
        std::size_t bytes = 0;
    };

    std::unordered_map<std::string, Entry> entries_;
    std::function<std::int64_t()> clock_;
    std::size_t max_bytes_;
    std::size_t current_bytes_ = 0;
    mutable std::size_t hits_   = 0;
    mutable std::size_t misses_ = 0;
    std::int64_t ttl_ms_;
};

} // namespace tk
