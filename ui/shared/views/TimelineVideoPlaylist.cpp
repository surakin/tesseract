#include "views/TimelineVideoPlaylist.h"

namespace tesseract::views
{

TimelineVideoPlaylist::~TimelineVideoPlaylist()
{
    // Invalidate the liveness sentinel BEFORE players_ is torn down. Deferred
    // UI-thread callbacks (the video-fetch result, the player's on_frame)
    // capture a weak handle and bail before touching `this`. Invalidating
    // here — rather than relying on the sentinel's own destruction at the end
    // of member teardown — closes the window in which a callback could
    // observe a half-destroyed playlist.
    invalidate_weak_self();
}

void TimelineVideoPlaylist::retire_(const std::string& event_id,
                                    std::unique_ptr<tk::VideoPlayer> player)
{
    if (!player)
    {
        return;
    }
    // Pause, not stop: stop() drops the loaded source (video_qt.cpp's
    // QtVideoPlayer::stop() closes its buffer), which is exactly the state a
    // same-event_id revisit needs to still be there so ensure_playing() can
    // resume() without going through play()/setSourceDevice() again.
    player->pause();
    player->on_frame = nullptr;
    player->on_progress = nullptr;
    player->on_error = nullptr;
    if (retired_pool_.size() >= kRetiredPoolCap)
    {
        // Evict the oldest-retired entry to make room. Destroying it here
        // (rather than the newly-retired one) keeps the pool weighted
        // towards whichever players were freed up most recently.
        retired_pool_.erase(retired_pool_.begin());
    }
    retired_pool_.push_back({event_id, std::move(player), now_()});
}

void TimelineVideoPlaylist::drop(const std::string& event_id)
{
    gc_sleeping_.erase(event_id);
    auto it = players_.find(event_id);
    if (it == players_.end())
    {
        return;
    }
    retire_(event_id, std::move(it->second.player));
    players_.erase(it);
}

void TimelineVideoPlaylist::clear()
{
    gc_sleeping_.clear();
    for (auto& [eid, entry] : players_)
    {
        retire_(eid, std::move(entry.player));
    }
    players_.clear();
}

void TimelineVideoPlaylist::ensure_playing(const VideoSourceInfo& info)
{
    if (!player_factory_ || !fetch_provider_)
    {
        return;
    }
    if (players_.count(info.event_id))
    {
        return;
    }
    if (static_cast<int>(players_.size()) >= kMaxInlinePlayers)
    {
        return;
    }

    // guarded() is called once per player, synchronously here (this is a
    // normal, directly-invoked member function — `this` is definitely
    // alive). invalidate_weak_self() runs as the first statement of
    // ~TimelineVideoPlaylist, before any member teardown, so a frame landing
    // after that point bails here instead of dereferencing a half-destroyed
    // `this` via repaint_.
    auto wire_on_frame = [this](tk::VideoPlayer& player)
    {
        player.on_frame = guarded([this]
        {
            if (repaint_)
            {
                repaint_();
            }
        });
    };

    // Fast path: this exact video was already loaded and only paused (not
    // stopped) when its row was last dropped — resume it directly instead of
    // re-fetching and re-play()ing, which is what actually stands up a new hw
    // decode session on the Qt6/FFmpeg backend regardless of whether the
    // player object itself is fresh or reused.
    for (std::size_t i = 0; i < retired_pool_.size(); ++i)
    {
        if (retired_pool_[i].event_id != info.event_id)
        {
            continue;
        }
        auto player = std::move(retired_pool_[i].player);
        retired_pool_.erase(retired_pool_.begin() + static_cast<std::ptrdiff_t>(i));
        player->set_loop(info.loop);
        player->set_muted(info.muted);
        wire_on_frame(*player);
        if (info.autoplay)
        {
            player->resume();
        }
        players_[info.event_id] = {std::move(player), false, info, gen_};
        return;
    }

    std::unique_ptr<tk::VideoPlayer> player;
    if (!retired_pool_.empty())
    {
        // No exact match: repurpose the oldest retired player rather than
        // constructing a fresh one. This still goes through fetch + play()
        // below (so it doesn't avoid the hw-decode-session cost), but it does
        // avoid the tk::VideoPlayer construction/destruction overhead.
        player = std::move(retired_pool_.front().player);
        retired_pool_.erase(retired_pool_.begin());
        // retire_() paused (not stopped) this player, so it's still holding
        // its PREVIOUS event's last decoded frame. Since this branch always
        // re-fetches and re-play()s fresh bytes below anyway, drop that
        // stale frame now — otherwise live_frame() would show the wrong
        // event's video until the new content's first frame arrives.
        player->stop();
    }
    else
    {
        player = player_factory_();
    }
    if (!player)
    {
        return;
    }
    player->set_loop(info.loop);
    player->set_muted(info.muted);
    wire_on_frame(*player);
    players_[info.event_id] = {std::move(player), false, info, gen_};

    const std::string eid = info.event_id;
    const std::string src = info.source_token;
    const std::string mime = info.mime;
    const bool autoplay = info.autoplay;

    // guarded() is called once here, synchronously — the playlist is
    // destroyed on every room switch while this fetch may still be in
    // flight, so the wrapped body bails if we've been torn down by then.
    fetch_provider_(
        src,
        guarded([this, eid, mime, autoplay](std::vector<std::uint8_t> bytes)
        {
            auto it = players_.find(eid);
            if (it == players_.end() || !it->second.player)
            {
                return;
            }
            if (bytes.empty())
            {
                players_.erase(eid);
                return;
            }
            auto& p = *it->second.player;
            p.play(bytes.data(), bytes.size(), mime);
            if (!autoplay)
            {
                p.pause();
            }
        }));
}

const tk::Image* TimelineVideoPlaylist::live_frame(
    const std::string& event_id) const
{
    auto it = players_.find(event_id);
    if (it != players_.end() && it->second.player)
    {
        return it->second.player->current_frame();
    }
    return nullptr;
}

void TimelineVideoPlaylist::touch(const std::string& event_id)
{
    if (auto it = players_.find(event_id); it != players_.end())
    {
        it->second.last_seen_gen = gen_;
        return;
    }
    auto sleeping = gc_sleeping_.find(event_id);
    if (sleeping == gc_sleeping_.end())
    {
        return;
    }
    // Copy first: ensure_playing() may run arbitrary callbacks.
    const VideoSourceInfo info = sleeping->second;
    ensure_playing(info);
    if (players_.count(event_id))
    {
        gc_sleeping_.erase(event_id);
    }
}

void TimelineVideoPlaylist::retire_unseen(unsigned keep)
{
    std::vector<std::string> stale;
    for (const auto& [eid, entry] : players_)
    {
        if (gen_ - entry.last_seen_gen >= keep)
        {
            stale.push_back(eid);
        }
    }
    for (const auto& eid : stale)
    {
        auto it = players_.find(eid);
        gc_sleeping_[eid] = it->second.info;
        retire_(eid, std::move(it->second.player));
        players_.erase(it);
    }
}

std::size_t TimelineVideoPlaylist::memory_bytes() const
{
    std::size_t total = 0;
    for (const auto& [_, entry] : players_)
        if (entry.player)
            total += entry.player->memory_bytes();
    for (const auto& r : retired_pool_)
        if (r.player)
            total += r.player->memory_bytes();
    return total;
}

void TimelineVideoPlaylist::release_idle_retired(std::chrono::milliseconds ttl)
{
    const auto now = now_();
    for (auto it = retired_pool_.begin(); it != retired_pool_.end();)
    {
        if (now - it->retired_at > ttl)
        {
            it = retired_pool_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void TimelineVideoPlaylist::pause_all()
{
    for (auto& [eid, entry] : players_)
    {
        if (!entry.player)
        {
            continue;
        }
        entry.was_playing_before_suspend = entry.player->is_playing();
        if (entry.was_playing_before_suspend)
        {
            entry.player->pause();
        }
    }
}

void TimelineVideoPlaylist::resume_all(
    const std::function<bool(const std::string& event_id)>& is_row_visible)
{
    if (!is_row_visible)
    {
        return;
    }
    for (auto& [eid, entry] : players_)
    {
        if (!entry.player || !entry.was_playing_before_suspend)
        {
            continue;
        }
        if (is_row_visible(eid))
        {
            entry.player->resume();
            entry.was_playing_before_suspend = false;
        }
    }
}

} // namespace tesseract::views
