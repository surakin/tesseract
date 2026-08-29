#pragma once

#include "MediaOverlayBase.h"

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

// Full-window video lightbox overlay. Parallel to ImageViewerOverlay.
//
// Usage:
//   1. Call set_video_player() with a platform-created tk::VideoPlayer.
//   2. Set the image provider (same lambda as MessageListView).
//   3. Wire on_close to hide the host surface/window.
//   4. Call open() when the user clicks a video thumbnail.
//   5. Call load_bytes() once the async byte fetch completes.
//   6. The shell handles Escape natively by calling close() when is_open().
class VideoViewerOverlay : public MediaOverlayBase
{
public:
    VideoViewerOverlay();

    // Show the overlay for the given video. Transitions to the loading state
    // while waiting for load_bytes(). The thumbnail is shown immediately.
    // The fi.mau.* params default to false so existing callers need no changes.
    void open(std::string source_json, std::string thumb_url,
              std::string mime_type, std::uint64_t duration_ms, int natural_w,
              int natural_h, bool loop = false, bool no_audio = false,
              bool hide_controls = false);

    // Hide the overlay. Stops playback and fires on_close.
    void close();
    bool is_loading() const
    {
        return is_loading_;
    }

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

    // Same provider lambda used by MessageListView.
    void
    set_image_provider(std::function<const tk::Image*(const std::string&)> fn);

    // Must be set by the shell so on_frame / on_progress can trigger a
    // repaint of the hosting surface. Typically: [this]{ surface_->relayout(); }
    void set_repaint_requester(std::function<void()> fn);

    // on_close / on_save are inherited from MediaOverlayBase. For video the
    // save callback receives (source_json_, mime_type_).

    // Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    // Continues a scrub-bar drag: the host forwards every pointer-move here
    // after on_pointer_down claimed the press (see press_scrub_), until the
    // matching pointer-up. Lets the user drag the playhead instead of only
    // click-to-seek.
    void on_pointer_drag(tk::Point local) override;
    // Swallow wheel input whenever the lightbox is open so it never falls
    // through the overlay stack to the room timeline underneath and drives
    // an invisible backward-pagination scroll (see ImageViewerOverlay's
    // identical override and RoomView::MessageBlocker for the same hazard).
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

protected:
    bool on_content_pointer_down_(tk::Point world, tk::Point local) override;
    bool on_content_pointer_up_(tk::Point world, tk::Point local,
                                bool inside_self) override;
    void fire_save_() override;
    void dismiss_() override;

private:
    void do_play_or_pause();
    void cycle_speed();
    void recompute_layout(tk::LayoutCtx& lc);
    // Shared by the initial scrub-bar press and every drag move after it:
    // maps world-space x to a fraction of scrub_bar_'s width and seeks
    // there. No-op if the duration isn't known yet or scrub_bar_ is empty.
    void seek_from_scrub_x_(float world_x);

    bool is_loading_ = false;
    std::chrono::steady_clock::time_point loading_start_{};
    std::string source_json_;
    std::string thumb_url_;
    std::string mime_type_;
    std::uint64_t duration_ms_ = 0;
    int natural_w_ = 0;
    int natural_h_ = 0;
    float rate_ = 1.0f;
    // fi.mau.* playback hints — reset on each open().
    bool loop_ = false;
    bool no_audio_ = false;
    bool hide_controls_ = false;

    std::unique_ptr<tk::VideoPlayer> video_player_;
    std::function<const tk::Image*(const std::string&)> image_provider_;
    std::function<void()> request_repaint_;

    tk::Rect video_rect_{};
    tk::Rect controls_bar_{};
    tk::Rect scrub_bar_{};

    // Real tk::Button children (Icon variant — hover/press/keyboard-activation,
    // fill/hover-cross-fade, and the play glyph itself all come from Button).
    // Positioned by recompute_layout(); paint() still draws the rate text /
    // pause bars over speed_btn_ / play_btn_ since those aren't plain icons.
    tk::Button* play_btn_ = nullptr;
    tk::Button* speed_btn_ = nullptr;

    bool has_error_ = false;

    bool press_scrub_ = false;

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
    // apply loop/mute and call video_player_->play(). Guards on is_open_.
    void finish_load_(const std::uint8_t* data, std::size_t size);
};

} // namespace tesseract::views
