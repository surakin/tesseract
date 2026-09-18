#include "anim_image_cache.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace tk
{

AnimImageCache::AnimImageCache(std::size_t max_bytes, std::int64_t ttl_ms)
    : max_bytes_(max_bytes), ttl_ms_(ttl_ms)
{
}

std::int64_t AnimImageCache::vis_now_() const
{
    if (clock_)
    {
        return clock_();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void AnimImageCache::store(const CacheKey& key,
                           std::vector<std::unique_ptr<tk::Image>> frames,
                           std::vector<int> delays_ms, std::int64_t now_ms,
                           std::shared_ptr<AnimDecodeSession> session,
                           std::size_t total_frames)
{
    if (frames.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);

    // Refuse to clobber an entry that already has real decode progress
    // (more than just frame 0). store() only legitimately runs once per
    // decode (each decode's frame 0), so a second store() call for a key
    // that's already progressed beyond frame 0 means a redundant, separate
    // decode is racing the first one for the same key — accepting it would
    // silently reset playback back near the start, repeatedly, for as long
    // as the redundant decode keeps recurring. A fresh single-frame entry
    // (frames.size() <= 1, i.e. nothing has appended onto it yet) is still
    // fair game to replace, same as an absent one.
    if (auto existing = entries_.find(key);
        existing != entries_.end() && existing->second.frames.size() > 1)
    {
        return;
    }

    Entry entry;
    entry.frames = std::move(frames);
    entry.delays_ms = std::move(delays_ms);
    entry.window_start = 0;
    entry.current = 0;
    entry.next_advance_ms =
        now_ms + (entry.delays_ms.empty() ? 100 : entry.delays_ms[0]);
    // Treat a freshly stored entry as visible so the timer keeps running until
    // the first paint refreshes (or fails to refresh) this stamp.
    entry.last_seen_ms = vis_now_();
    entry.session = std::move(session);
    entry.total_frames = total_frames;

    entries_.insert_or_assign(key, std::move(entry));
}

std::size_t AnimImageCache::entry_bytes_locked_(const Entry& entry)
{
    std::size_t total = 0;
    for (const auto& f : entry.frames)
    {
        total += f ? f->memory_bytes() : 0;
    }
    if (entry.session)
    {
        total += entry.session->memory_bytes();
    }
    return total;
}

void AnimImageCache::push_frame_locked_(Entry& entry,
                                        std::unique_ptr<tk::Image> frame,
                                        int delay_ms)
{
    if (!frame)
    {
        return;
    }
    entry.frames.push_back(std::move(frame));
    entry.delays_ms.push_back(delay_ms);
}

void AnimImageCache::append_frame_locked_(Entry& entry,
                                          std::unique_ptr<tk::Image> frame,
                                          int delay_ms)
{
    if (entry.restarting)
    {
        // This is the new loop's frame 0, replacing the single placeholder
        // frame restart_loop_locked_() left resident — not a normal
        // mid-window top-up frame, so it replaces rather than appends.
        entry.frames.clear();
        entry.delays_ms.clear();
        push_frame_locked_(entry, std::move(frame), delay_ms);
        entry.window_start = 0;
        entry.current = 0;
        entry.restarting = false;
        return;
    }
    push_frame_locked_(entry, std::move(frame), delay_ms);
}

void AnimImageCache::append_frame(const CacheKey& key,
                                  std::unique_ptr<tk::Image> frame,
                                  int delay_ms)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end() || !frame)
    {
        return;
    }
    append_frame_locked_(it->second, std::move(frame), delay_ms);
}

void AnimImageCache::append_frame_from_session(
    const CacheKey& key, const std::shared_ptr<AnimDecodeSession>& session,
    std::unique_ptr<tk::Image> frame, int delay_ms)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    // The session-identity check is what append_frame() alone can't do: a
    // losing decode in a redundant-fetch race (two independent decodes for
    // the same key — the second one's on_first correctly bails out once it
    // sees has(key) already true, but that check doesn't exist for
    // subsequent frames) would otherwise splice its own, independently
    // re-decoded frames into the winning entry's sequence — visibly the
    // animation appearing to jump backward mid-playback, since the losing
    // decode is effectively replaying the same content from its own
    // frame 0. Dropping frames whose session no longer matches the entry's
    // current one closes that gap.
    if (it == entries_.end() || !frame || it->second.session != session)
    {
        return;
    }
    append_frame_locked_(it->second, std::move(frame), delay_ms);
}

std::vector<AnimImageCache::TopupRequest> AnimImageCache::collect_topups()
{
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t vis_now = vis_now_();
    std::vector<TopupRequest> out;
    for (auto& [key, entry] : entries_)
    {
        if (!entry.session || entry.topping_up || entry.paused)
        {
            continue;
        }
        if (vis_now - entry.last_seen_ms > kVisibilityGraceMs)
        {
            continue; // off-screen: don't burn CPU decoding ahead for it
        }
        if (entry.session->exhausted())
        {
            continue;
        }
        const bool low_on_resident_frames =
            entry.frames.empty() ||
            (entry.frames.size() - 1 - entry.current) <= kLookaheadFrames;
        if (!low_on_resident_frames)
        {
            continue;
        }
        entry.topping_up = true;
        out.push_back(TopupRequest{key, entry.session, kTopupBatchFrames});
    }
    return out;
}

void AnimImageCache::finish_topup(const CacheKey& key)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return;
    }
    it->second.topping_up = false;
}

void AnimImageCache::trim_window_locked_(Entry& entry)
{
    if (!entry.session || entry.current <= kKeepBehindFrames)
    {
        return;
    }
    const std::size_t drop = entry.current - kKeepBehindFrames;
    entry.frames.erase(entry.frames.begin(),
                       entry.frames.begin() + static_cast<std::ptrdiff_t>(drop));
    entry.delays_ms.erase(
        entry.delays_ms.begin(),
        entry.delays_ms.begin() + static_cast<std::ptrdiff_t>(drop));
    entry.window_start += drop;
    entry.current -= drop;
}

void AnimImageCache::restart_loop_locked_(Entry& entry)
{
    // Drop every resident frame except the one currently being displayed
    // (entry.current) — that one is kept, moved to index 0, so
    // current_frame() keeps returning a valid (if momentarily stale)
    // pointer instead of nullptr until append_frame() replaces it with the
    // new loop's real frame 0 (see the `restarting` flag's doc comment).
    if (!entry.frames.empty())
    {
        if (entry.current != 0)
        {
            entry.frames[0] = std::move(entry.frames[entry.current]);
            entry.delays_ms[0] = entry.delays_ms[entry.current];
        }
        entry.frames.resize(1);
        entry.delays_ms.resize(1);
    }
    entry.window_start = 0; // meaningless during the gap; append_frame() resets it
    entry.current = 0;
    entry.restarting = true;
    entry.session->restart();
}

bool AnimImageCache::has(const CacheKey& key) const
{
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.count(key) > 0;
}

const tk::Image* AnimImageCache::current_frame(const CacheKey& key) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.frames.empty())
    {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    it->second.last_seen_ms = vis_now_();
    return it->second.frames[it->second.current].get();
}

void AnimImageCache::set_paused(const CacheKey& key, bool paused)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return;
    }
    it->second.paused = paused;
}

bool AnimImageCache::advance(std::int64_t now_ms)
{
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t vis_now = vis_now_();
    bool any = false;
    for (auto& [_, entry] : entries_)
    {
        if (entry.paused)
        {
            continue;
        }
        if (entry.frames.empty() ||
            entry.delays_ms.size() != entry.frames.size())
        {
            continue;
        }
        // Skip entries that have not been painted recently — they are off-
        // screen (scrolled away or in another room) and must not drive
        // repaints. Their deadline is left as-is and resynced on return.
        if (vis_now - entry.last_seen_ms > kVisibilityGraceMs)
        {
            continue;
        }
        // Just came back into view after being skipped for a while: drop the
        // stale backlog so we resume from the current frame instead of fast-
        // forwarding through every frame missed while hidden.
        if (now_ms - entry.next_advance_ms > kVisibilityGraceMs)
        {
            entry.next_advance_ms = now_ms + entry.delays_ms[entry.current];
            continue;
        }
        while (now_ms >= entry.next_advance_ms)
        {
            const bool at_resident_end =
                (entry.current == entry.frames.size() - 1);
            if (entry.session && at_resident_end)
            {
                // entry.topping_up is checked (and this branch bails) before
                // ever calling session->exhausted()/restart(): those run on
                // whatever thread called decode_next_batch() for this
                // session, with no internal synchronization of their own —
                // topping_up is this cache's own mutex-guarded signal that
                // such a call is currently in flight, so treating it as "not
                // safe to touch the session right now" keeps every session
                // method call confined to whichever single thread is active
                // (this one, when topping_up is false) at a time.
                if (entry.topping_up || !entry.session->exhausted())
                {
                    // Either a top-up is currently decoding this session on
                    // another thread, or (topping_up == false) more frames
                    // are simply still to come — either way, stall on the
                    // last resident frame instead of wrapping prematurely.
                    // collect_topups() will (re)request more as needed.
                    break;
                }
                if (entry.window_start != 0)
                {
                    // True end of the animation, but frame 0 has been
                    // trimmed out of the window — loop back by restarting
                    // the session. Frames are now empty; wait for
                    // collect_topups() to refill starting at frame 0.
                    restart_loop_locked_(entry);
                    any = true;
                    break;
                }
                // Exhausted, but the window was never trimmed (the whole
                // animation fit inside it) — falls through to the ordinary
                // wrap below, identical to an unwindowed entry.
            }
            entry.current = (entry.current + 1) % entry.frames.size();
            // Clamp to >=1ms: 0ms frame delays (some encoders emit them) leave
            // next_advance_ms perpetually due. The at_resident_end break caps
            // it at one cycle per tick, but frames still flash past and
            // waste CPU.
            int delay = entry.delays_ms[entry.current];
            if (delay < 1)
            {
                delay = 1;
            }
            entry.next_advance_ms += delay;
            any = true;
            if (at_resident_end)
            {
                break; // loop-point frame gets one rendered pass before catch-up continues
            }
        }
        if (entry.session)
        {
            trim_window_locked_(entry);
        }
    }
    return any;
}

bool AnimImageCache::any_visible() const
{
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t vis_now = vis_now_();
    for (const auto& [_, entry] : entries_)
    {
        if (vis_now - entry.last_seen_ms <= kVisibilityGraceMs)
        {
            return true;
        }
    }
    return false;
}

void AnimImageCache::sweep()
{
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t vis_now = vis_now_();

    // 1) Drop entries not painted within the TTL window.
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (vis_now - it->second.last_seen_ms > ttl_ms_)
        {
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Recomputed fresh rather than tracked incrementally: memory_bytes() is
    // not guaranteed stable for a given Image over its lifetime (some
    // backends memoize additional pre-scaled copies as the image is painted
    // at different target sizes), so any running total kept across calls
    // could drift from what's actually resident — see entry_bytes_locked_'s
    // doc comment.
    std::size_t total = 0;
    for (const auto& [_, entry] : entries_)
    {
        total += entry_bytes_locked_(entry);
    }
    if (total <= max_bytes_)
    {
        return;
    }

    // 2) Still over budget: evict least-recently-seen off-screen entries
    //    oldest-first until under budget. Visible entries are kept.
    std::vector<std::unordered_map<CacheKey, Entry, CacheKeyHash>::iterator> evictable;
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        if (vis_now - it->second.last_seen_ms > kVisibilityGraceMs)
        {
            evictable.push_back(it);
        }
    }
    std::sort(evictable.begin(), evictable.end(),
              [](const auto& a, const auto& b)
              { return a->second.last_seen_ms < b->second.last_seen_ms; });

    for (auto it : evictable)
    {
        if (total <= max_bytes_)
        {
            break;
        }
        total -= entry_bytes_locked_(it->second);
        entries_.erase(it);
    }
}

void AnimImageCache::clear()
{
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
    hits_           = 0;
    misses_         = 0;
}

bool AnimImageCache::empty() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.empty();
}

std::size_t AnimImageCache::current_bytes() const
{
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t total = 0;
    for (const auto& [_, entry] : entries_)
    {
        total += entry_bytes_locked_(entry);
    }
    return total;
}

std::size_t AnimImageCache::hits() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
}

std::size_t AnimImageCache::misses() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return misses_;
}

} // namespace tk
