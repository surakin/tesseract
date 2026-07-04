#pragma once

// TimelineVideoPlaylist — inline auto-play video subsystem extracted from
// MessageListView. Owns the pool of live tk::VideoPlayer instances (one per
// autoplay / gif row, keyed by event_id), the player factory + async byte
// fetch provider, the kMaxInlinePlayers cap + eviction policy, and its OWN
// liveness sentinel so the in-flight fetch / on_frame callbacks never touch a
// destroyed playlist after a room switch.
//
// MessageListView holds one of these by value and forwards its public
// set_video_player_factory / set_video_fetch_provider wiring here. set_messages
// / insert_message / update_message call ensure_playing() for animated video
// rows (and clear() on room switch / drop() on removal). The Adapter video
// painter asks live_frame() for the current decoded frame; when it returns
// nullptr the Adapter falls back to the static thumbnail + play-disc (which —
// together with the fullscreen click hit-test — stays in MessageListView).
//
// The fullscreen video click/hit-test (video_geom_ / on_video_clicked /
// video_hit_at) is a SEPARATE concern and stays in MessageListView.

#include "tk/video.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tesseract::views
{

// Source description for one inline-playable row, mirroring the fields
// start_inline_video() historically read off MessageRowData. The caller
// (MessageListView) extracts these from the row and from media_is_hidden_()
// before handing them to ensure_playing(); the playlist stays decoupled from
// the row struct and from view-private visibility state.
struct VideoSourceInfo
{
    std::string event_id;
    std::string source_token; // m.source->fetch_token() (empty when absent)
    std::string mime;         // m.video_mime
    bool autoplay = false;    // m.video_autoplay
    bool loop = false;        // m.video_loop
    bool muted = false;       // m.video_no_audio
};

class TimelineVideoPlaylist
{
public:
    // At most this many inline players run simultaneously. Matches the
    // historical MessageListView::kMaxInlinePlayers.
    static constexpr int kMaxInlinePlayers = 10;

    TimelineVideoPlaylist() = default;
    ~TimelineVideoPlaylist();

    TimelineVideoPlaylist(const TimelineVideoPlaylist&) = delete;
    TimelineVideoPlaylist& operator=(const TimelineVideoPlaylist&) = delete;

    using VideoPlayerFactory =
        std::function<std::unique_ptr<tk::VideoPlayer>()>;
    using VideoFetchProvider = std::function<void(
        const std::string& source_json,
        std::function<void(std::vector<std::uint8_t>)> on_ready)>;

    void set_player_factory(VideoPlayerFactory f)
    {
        player_factory_ = std::move(f);
    }
    void set_fetch_provider(VideoFetchProvider f)
    {
        fetch_provider_ = std::move(f);
    }
    // Repaint requester, invoked when an inline player produces a new frame.
    void set_repaint(std::function<void()> f) { repaint_ = std::move(f); }

    // True when both factory + fetch provider are wired; mirrors the historical
    // guard at the top of start_inline_video().
    bool active() const { return player_factory_ && fetch_provider_; }

    // Number of live players (used by the bounded set_messages bootstrap loop
    // to stop once it reaches the cap).
    int size() const { return static_cast<int>(players_.size()); }
    bool has(const std::string& event_id) const
    {
        return players_.count(event_id) != 0;
    }

    // Create + start an inline player for `info` (the former
    // start_inline_video). No-op when inactive, already playing, or at the cap.
    //
    // Two levels of reuse against the retired pool (see kRetiredPoolCap):
    //  1. Exact event_id match (e.g. switching back to a room whose video was
    //     already loaded): the retired player is still paused-not-stopped, so
    //     it's reclaimed and resumed directly — no fetch, no play(), no
    //     re-probe. This is the only path that actually avoids the cost:
    //     profiling showed the hw decode session (e.g. a CUDA context on the
    //     Qt6/FFmpeg backend) is torn down and rebuilt by play()/
    //     setSourceDevice() itself, not by constructing/destroying the
    //     player object — so replaying the same source through a reused
    //     player pays the same cost as a brand new one.
    //  2. No match: the oldest retired player (if any) is repurposed instead
    //     of asking the factory for a fresh one, which at least avoids the
    //     tk::VideoPlayer construction/destruction overhead. This still goes
    //     through the normal fetch + play() path below.
    void ensure_playing(const VideoSourceInfo& info);

    // Drop the player for a single row (update_message event_id swap / row
    // de-animation / remove_message). Retires it into the pool rather than
    // destroying it outright — see retire_().
    void drop(const std::string& event_id);

    // Drop every player (room switch — set_messages clears unconditionally
    // before re-bootstrapping the tail rows). Retires each into the pool
    // rather than destroying it outright — see retire_().
    void clear();

    // Current decoded frame for `event_id`, or nullptr when there is no live
    // player or it has not yet produced a frame. The Adapter draws the static
    // thumbnail fallback when this returns nullptr.
    const tk::Image* live_frame(const std::string& event_id) const;

private:
    struct InlinePlayer
    {
        std::unique_ptr<tk::VideoPlayer> player;
    };
    std::unordered_map<std::string, InlinePlayer> players_;
    VideoPlayerFactory player_factory_;
    VideoFetchProvider fetch_provider_;
    std::function<void()> repaint_;

    // At most this many paused players are kept warm across room switches /
    // row drops instead of being destroyed. Small on purpose: this only
    // needs to cover the handful of inline videos visible at once, not the
    // full kMaxInlinePlayers concurrency cap.
    static constexpr std::size_t kRetiredPoolCap = 4;

    struct RetiredPlayer
    {
        std::string event_id;
        std::unique_ptr<tk::VideoPlayer> player;
    };

    // Pause (NOT stop — stop() drops the loaded source, which is exactly the
    // state a same-event_id revisit needs to still be there) `player` and
    // stash it under `event_id`, evicting the oldest entry first if already
    // at kRetiredPoolCap. Clears on_frame/on_progress/on_error first so a
    // stale closure never fires against a player that no longer belongs to
    // any row.
    void retire_(const std::string& event_id,
                 std::unique_ptr<tk::VideoPlayer> player);

    // Oldest-first; see retire_().
    std::vector<RetiredPlayer> retired_pool_;

    // Liveness sentinel. The async fetch result + the player's on_frame
    // callback capture a weak_ptr to this and bail if it has been cleared —
    // the playlist is destroyed (and the view torn down) on every room switch
    // while a fetch may still be in flight, so a raw `this` capture would be a
    // use-after-free. The destructor sets *alive_ = false BEFORE players_ is
    // torn down, closing the half-destroyed-object window.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace tesseract::views
