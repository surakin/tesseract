#pragma once

#include "canvas.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tk
{

// Per-URL cache of decoded animation frames (GIF / APNG / animated WebP).
// Each entry holds the full decoded frame list and a monotonic deadline for
// the next frame advance. The caller drives timing by calling advance() from
// a ~60 Hz platform timer and passing the current time in milliseconds from
// any consistent epoch (wall clock, boot clock, etc. — as long as the same
// source is used for store() and advance()).
class AnimImageCache
{
public:
    // Add or replace an animated entry. `now_ms` is used to set the initial
    // frame-advance deadline to `now_ms + delays_ms[0]`.
    void store(const std::string& key,
               std::vector<std::unique_ptr<tk::Image>> frames,
               std::vector<int> delays_ms, std::int64_t now_ms);

    bool has(const std::string& key) const;
    bool empty() const
    {
        return entries_.empty();
    }

    // Return the current frame for `key`, or nullptr if not found / no frames.
    const tk::Image* current_frame(const std::string& key) const;

    // Advance all entries whose deadline has passed. Returns true when at
    // least one frame index changed (the caller should repaint).
    bool advance(std::int64_t now_ms);

private:
    struct Entry
    {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int> delays_ms;
        std::size_t current = 0;
        std::int64_t next_advance_ms = 0;
    };

    std::unordered_map<std::string, Entry> entries_;
};

} // namespace tk
