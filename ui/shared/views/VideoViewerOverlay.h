#pragma once

#include "tk/canvas.h"
#include "tk/video.h"
#include "tk/widget.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
class VideoViewerOverlay : public tk::Widget
{
public:
    // Show the overlay for the given video. Transitions to the loading state
    // while waiting for load_bytes(). The thumbnail is shown immediately.
    // The fi.mau.* params default to false so existing callers need no changes.
    void open(std::string source_json, std::string thumb_url,
              std::string mime_type, std::uint64_t duration_ms, int natural_w,
              int natural_h, bool loop = false, bool no_audio = false,
              bool hide_controls = false);

    // Hide the overlay. Stops playback and fires on_close.
    void close();
    bool is_open() const
    {
        return is_open_;
    }
    bool is_loading() const
    {
        return is_loading_;
    }

    // Called on the UI thread once the async byte fetch completes.
    // Starts playback immediately.
    void load_bytes(const std::uint8_t* data, std::size_t size);

    void set_video_player(std::unique_ptr<tk::VideoPlayer> player);

    // Same provider lambda used by MessageListView.
    void
    set_image_provider(std::function<const tk::Image*(const std::string&)> fn);

    // Must be set by the shell so on_frame / on_progress can trigger a
    // repaint of the hosting surface. Typically: [this]{ surface_->relayout(); }
    void set_repaint_requester(std::function<void()> fn);

    // Fires when the overlay should be dismissed (× button, outside click).
    std::function<void()> on_close;

    /// Fires when the user clicks the ⬇ save button.
    /// `source_json` is either a plain mxc:// URI or a JSON MediaSource blob;
    /// pass to `Client::fetch_source_bytes`. `mime_type` is e.g. "video/mp4".
    std::function<void(std::string source_json, std::string mime_type)> on_save;

    // Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;

private:
    void do_play_or_pause();
    void cycle_speed();
    void recompute_layout();

    bool is_open_ = false;
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
    tk::Rect play_btn_{};
    tk::Rect scrub_bar_{};
    tk::Rect speed_pill_{};
    tk::Rect close_btn_{};
    tk::Rect save_btn_{};

    bool has_error_ = false;

    bool press_play_ = false;
    bool press_scrub_ = false;
    bool press_speed_ = false;
    bool press_close_ = false;
    bool press_save_ = false;
    bool press_outside_ = false;
};

} // namespace tesseract::views
