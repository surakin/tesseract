#include "views/TimelineVideoPlaylist.h"

namespace tesseract::views
{

TimelineVideoPlaylist::~TimelineVideoPlaylist()
{
    // Invalidate the liveness sentinel BEFORE players_ is torn down. Deferred
    // UI-thread callbacks (the video-fetch result, the player's on_frame)
    // capture a weak_ptr to `alive_` and check `*alive_` before touching
    // `this`. Clearing the flag here — rather than relying on `alive_`'s own
    // destruction at the end of member teardown — closes the window in which a
    // callback could observe a half-destroyed playlist.
    *alive_ = false;
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

    auto player = player_factory_();
    if (!player)
    {
        return;
    }
    player->set_loop(info.loop);
    player->set_muted(info.muted);
    std::weak_ptr<bool> walive = alive_;
    player->on_frame = [this, walive]
    {
        // Lock the weak_ptr and check the flag instead of expired(): during
        // ~TimelineVideoPlaylist the destructor sets *alive_ = false before
        // alive_ is destroyed, and expired() stays false across that window —
        // a frame landing there would dereference a half-destroyed `this` via
        // repaint_.
        auto live = walive.lock();
        if (!live || !*live)
        {
            return;
        }
        if (repaint_)
        {
            repaint_();
        }
    };
    players_[info.event_id] = {std::move(player)};

    const std::string eid = info.event_id;
    const std::string src = info.source_token;
    const std::string mime = info.mime;
    const bool autoplay = info.autoplay;

    fetch_provider_(
        src,
        [this, walive, eid, mime, autoplay](std::vector<std::uint8_t> bytes)
        {
            // The playlist is destroyed on every room switch; the fetch may
            // still be in flight. Bail if we've been torn down.
            auto live = walive.lock();
            if (!live || !*live)
            {
                return;
            }
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
        });
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

} // namespace tesseract::views
