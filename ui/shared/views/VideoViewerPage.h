#pragma once

#include "MediaTransportBar.h"
#include "MediaViewerPage.h"

#include "tk/canvas.h"
#include "tk/video.h"
#include "tk/widget.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

// Video page of MediaViewerOverlay: video surface (or thumbnail / placeholder /
// error), loading spinner and the transport controls (play, scrub, speed).
// Non-widget — the overlay owns the scrim, chrome buttons and pointer routing,
// and forwards to this page; the play and speed buttons are real tk::Button
// children of the overlay, hidden while the page is inactive.
//
// Delivery (load_bytes / the stream calls) is driven by MediaViewerOverlay,
// which drops calls carrying a stale load token before they reach here.
class VideoViewerPage : public MediaViewerPage
{
public:
    explicit VideoViewerPage(MediaViewerPageHost& host);

    void activate(const MediaViewerItem& item) override;
    // Stops playback and drops any buffered stream data.
    void deactivate() override;
    void arrange(tk::LayoutCtx& lc, tk::Rect bounds) override;
    void paint_content(tk::PaintCtx&) override;
    void paint_controls(tk::PaintCtx&) override;
    bool on_content_pointer_down(tk::Point world, tk::Point local) override;
    bool on_content_pointer_up(tk::Point world, tk::Point local,
                               bool inside_self) override;
    // Continues a scrub-bar drag: the host forwards every pointer-move here
    // after on_content_pointer_down claimed the press (see press_scrub_),
    // until the matching pointer-up.
    void on_pointer_drag(tk::Point local) override;
    // Space / K play-pause, Left/Right seek +-5 s, Home restarts.
    bool on_key(const tk::KeyEvent& e) override;
    bool controls_hovered() const override;

    bool is_loading() const
    {
        return is_loading_;
    }

    // On-screen video bounds (valid once arrange/paint has run while active).
    tk::Rect video_rect() const
    {
        return video_rect_;
    }

    tk::Button* play_btn() const { return bar_.play_btn(); }
    tk::Button* speed_btn() const { return bar_.speed_btn(); }

    // Called on the UI thread once the async byte fetch completes.
    // Starts playback immediately.
    void load_bytes(const std::uint8_t* data, std::size_t size);

    // ── Progressive/streaming fetch delivery ────────────────────────────
    // Alternative to load_bytes() for a fetch whose bytes arrive
    // incrementally (Client::fetch_source_stream_async). Call
    // begin_stream_or_buffer() once when the fetch starts, then
    // feed_stream_chunk() as each chunk arrives, then exactly one of
    // end_stream() / fail_stream(). Internally tries
    // tk::VideoPlayer::begin_stream(); if the backend has no streaming
    // support, falls back transparently to accumulating chunks and calling
    // load_bytes() once complete — RoomPane's call site is identical either
    // way.
    void begin_stream_or_buffer();
    void feed_stream_chunk(const std::uint8_t* data, std::size_t size);
    // Forwards the fetch layer's real total size (e.g. HTTP Content-Length,
    // 0 if unknown) to tk::VideoPlayer::set_stream_length() once it's known
    // — typically well after begin_stream_or_buffer(), since the real
    // length usually isn't known until the response headers arrive. Safe to
    // call repeatedly; no-op in buffering mode (no player call needed there
    // since load_bytes() already hands over the complete buffer at once).
    void set_stream_length(std::uint64_t total_size);
    void end_stream();
    void fail_stream();

    void set_video_player(std::unique_ptr<tk::VideoPlayer> player);

    // Resident bytes held for the loaded clip: the player's buffers/frame plus
    // the pre-roll queue.
    std::size_t memory_bytes() const
    {
        return (video_player_ ? video_player_->memory_bytes() : 0) +
               stream_buffer_.capacity();
    }

private:
    void do_play_or_pause();
    void cycle_speed();
    void recompute_layout(tk::LayoutCtx& lc);
    // Relative seek, clamped to [0, duration] and (while streaming) to what's
    // downloaded so far — the same limit seek_from_scrub_x_ applies.
    void seek_by_ms_(std::int64_t delta_ms);
    // Scrub target from the transport bar (already clamped to what's
    // downloaded): seeks to that fraction of the duration. No-op if the
    // duration isn't known yet.
    void seek_to_fraction_(float frac);
    MediaTransportBar::State transport_state_() const;

    // True once a seek would do something (player loaded, no error, duration known).
    bool seekable_() const;

    bool is_loading_ = false;
    std::chrono::steady_clock::time_point loading_start_{};
    std::string source_json_;
    std::string thumb_url_;
    std::string mime_type_;
    std::uint64_t duration_ms_ = 0;
    int natural_w_ = 0;
    int natural_h_ = 0;
    float rate_ = 1.0f;
    // fi.mau.* playback hints — reset on each activate().
    bool loop_ = false;
    bool no_audio_ = false;
    bool hide_controls_ = false;

    MediaViewerPageHost& host_;
    bool active_ = false;

    std::unique_ptr<tk::VideoPlayer> video_player_;

    tk::Rect video_rect_{};
    tk::Rect controls_bar_{};

    // Play / scrub / speed strip (its buttons are real tk::Button children of
    // the overlay, owned through the bar).
    MediaTransportBar bar_;

    bool has_error_ = false;

    // Set by begin_stream_or_buffer() from tk::VideoPlayer::begin_stream()'s
    // return value: true if the player is consuming feed_stream_chunk()
    // directly, false if this overlay is accumulating chunks itself into
    // stream_buffer_ to hand the player as one buffer via load_bytes() (its
    // shared implementation, finish_load_()) once end_stream() fires.
    bool is_streaming_ = false;
    std::vector<std::uint8_t> stream_buffer_;

    // In streaming mode, stream_buffer_ doubles as a pre-roll queue: the
    // first kStreamPrerollBytes fed are held here and flushed to the player
    // in one feed_chunk() call instead of trickling in one small chunk at a
    // time. begin_stream() hands the audio engine an empty byte stream and
    // starts it immediately — feeding it from zero bytes risks the engine's
    // first read racing ahead of any data, which intermittently drops audio
    // init. 256 KiB matches the read size Media Foundation itself requests
    // per call (observed via tracing), so the very first read it issues can
    // be satisfied immediately without blocking from empty. Reset (false)
    // on open()/begin_stream_or_buffer(); end_stream() flushes whatever's
    // left even if the threshold was never reached (a short clip).
    static constexpr std::size_t kStreamPrerollBytes = 256 * 1024;
    bool preroll_flushed_ = false;

    // Drive the scrub bar's "streamed in so far" band (see paint()) while
    // is_streaming_ is true. stream_total_size_ comes from set_stream_length
    // (the fetch layer's HTTP Content-Length, 0 until known);
    // stream_bytes_fed_ is the running total handed to feed_stream_chunk().
    // Both reset on open()/begin_stream_or_buffer().
    std::uint64_t stream_total_size_ = 0;
    std::uint64_t stream_bytes_fed_ = 0;

    // Shared body of load_bytes() / the buffering-mode path of end_stream():
    // apply loop/mute and call video_player_->play(). Guards on active_.
    void finish_load_(const std::uint8_t* data, std::size_t size);
};

} // namespace tesseract::views
