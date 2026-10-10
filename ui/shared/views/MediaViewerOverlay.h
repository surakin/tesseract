#pragma once

#include "MediaOverlayBase.h"
#include "MediaViewerItem.h"
#include "MediaViewerPage.h"

#include "tk/audio.h"
#include "tk/canvas.h"
#include "tk/video.h"
#include "tk/widget.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class ImageViewerPage;
class VideoViewerPage;
class AudioViewerPage;
class FileViewerPage;

// The one full-window media lightbox. Owns the scrim, the shared chrome (close,
// save, copy, full-screen), outside-tap dismiss and pointer routing (all from
// MediaOverlayBase), and shows the current MediaViewerItem through a page
// strategy object per kind (ImageViewerPage, VideoViewerPage). One instance of
// each page is built once; switching items deactivates the old page and
// activates the new one, so players are never recreated.
//
// Usage:
//   1. set_video_player() / set_audio_player() with platform-created players; set the
//      image provider; wire on_close / on_save / on_copy / on_item_shown.
//   2. open(item) (or open_sequence) when the user clicks media. on_item_shown
//      fires with the item and a load token — the single place callers start
//      their byte fetches.
//   3. Feed video bytes back through load_bytes / the stream calls, always with
//      that token. Calls carrying a stale token (the item changed or the viewer
//      closed since the fetch started) are dropped.
//   4. The shell handles Escape by calling close() when is_open().
class MediaViewerOverlay : public MediaOverlayBase, private MediaViewerPageHost
{
public:
    MediaViewerOverlay();
    ~MediaViewerOverlay() override;

    // ── opening and state ───────────────────────────────────────────────
    // Show a single item. Resets full-screen (only open()/open_sequence() do).
    void open(MediaViewerItem item);
    void open_sequence(std::vector<MediaViewerItem> items, std::size_t index);
    // Hide the overlay (stops playback, bumps the load token, fires on_close).
    void close();

    const MediaViewerItem& current_item() const;
    // The i-th item of the open sequence (nullptr when out of range).
    const MediaViewerItem* item_at(std::size_t i) const
    {
        return i < items_.size() ? &items_[i] : nullptr;
    }
    std::size_t index() const
    {
        return index_;
    }
    std::size_t count() const
    {
        return items_.size();
    }
    // Monotonic token identifying the current item's load. Bumped on every item
    // switch and on close.
    std::uint64_t load_token() const
    {
        return load_token_;
    }

    // On-screen media bounds (valid once arrange/paint has run while open).
    tk::Rect image_rect() const;
    tk::Rect video_rect() const;
    bool is_loading() const; // video page loading state

    // ── video delivery (all token-checked) ──────────────────────────────
    // Starts playback once the whole file has arrived.
    void load_bytes(std::uint64_t token, const std::uint8_t* data, std::size_t size);
    // Progressive alternative: begin_stream_or_buffer() once, feed_stream_chunk()
    // per chunk, then exactly one of end_stream() / fail_stream(). Falls back to
    // buffering transparently when the backend can't stream.
    void begin_stream_or_buffer(std::uint64_t token);
    void feed_stream_chunk(std::uint64_t token, const std::uint8_t* data,
                           std::size_t size);
    void set_stream_length(std::uint64_t token, std::uint64_t total_size);
    void end_stream(std::uint64_t token);
    void fail_stream(std::uint64_t token);

    // ── audio delivery (token-checked) ──────────────────────────────────
    // Hands the audio page the clip's bytes and starts playback. An empty
    // vector reports a failed fetch.
    void load_audio_bytes(std::uint64_t token, std::vector<std::uint8_t> bytes);

    // ── players and providers ───────────────────────────────────────────
    void set_video_player(std::unique_ptr<tk::VideoPlayer> player);
    void set_audio_player(std::unique_ptr<tk::AudioPlayer> player);
    bool has_audio_player() const;
    // Resident bytes held for the loaded clip (player buffers + pre-roll queue).
    std::size_t memory_bytes() const;
    // Same provider lambda used by MessageListView — returns a cached tk::Image*.
    // Called during paint; may return nullptr while bytes are still loading.
    void set_image_provider(std::function<const tk::Image*(const std::string&)> fn);

    // ── callbacks ───────────────────────────────────────────────────────
    // ⬇ / ⧉ chrome buttons, with the item currently shown.
    std::function<void(const MediaViewerItem&)> on_save;
    std::function<void(const MediaViewerItem&)> on_copy;
    // A new item became current (open / sequence step). Start its fetch here and
    // hand the token back with the bytes.
    std::function<void(const MediaViewerItem&, std::uint64_t)> on_item_shown;
    // The video or audio page just started playing (after a load or a resume).
    std::function<void()> on_playback_started;

    // ── test accessors ──────────────────────────────────────────────────
    tk::Button* play_btn_for_test() const;
    tk::Button* speed_btn_for_test() const;
    tk::Button* audio_play_btn_for_test() const;
    tk::Button* audio_speed_btn_for_test() const;
    tk::Button* file_save_btn_for_test() const;
    tk::AudioPlayer* audio_player_for_test() const;
    bool audio_loading_for_test() const;
    bool audio_error_for_test() const;
    tk::Button* prev_btn_for_test() const
    {
        return prev_btn_;
    }
    tk::Button* next_btn_for_test() const
    {
        return next_btn_;
    }
    // "n / m" while a multi-item sequence is shown; empty otherwise.
    std::string counter_text_for_test() const;

    // Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    // Continues a page drag (image pan, video scrub) after on_pointer_down
    // claimed the press.
    void on_pointer_drag(tk::Point local) override;
    // Swallows wheel input whenever the lightbox is open so it never falls
    // through the overlay stack to the room timeline underneath (see
    // RoomView::MessageBlocker for the same hazard).
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

protected:
    bool on_content_pointer_down_(tk::Point world, tk::Point local) override;
    bool on_content_key_(const tk::KeyEvent& e) override;
    bool on_content_pointer_up_(tk::Point world, tk::Point local,
                                bool inside_self) override;
    void fire_save_() override;
    void fire_copy_() override;
    bool wants_copy_button_() const override;
    bool extra_chrome_hovered_() const override;
    void dismiss_() override;
    void on_fullscreen_changed_(bool fullscreen) override;
    void on_icon_scale_changed_() override;

private:
    // MediaViewerPageHost
    tk::Rect host_bounds() const override;
    bool host_fullscreen() const override;
    bool host_chrome_shown() const override;
    void host_refresh_chrome_autohide() override;
    void host_request_repaint() override;
    const tk::Image* host_image(const std::string& key) const override;
    tk::Button* host_add_icon_button() override;
    tk::Button* host_add_label_button(std::string label) override;
    void host_playback_started() override;

    // Deactivate the current page (if any), bump the token, activate the page
    // for items_[i] and fire on_item_shown. Never touches full-screen.
    void show_index_(std::size_t i);
    MediaViewerPage* page_for_(MediaViewerItem::Kind kind) const;
    // Move by `delta` (wrapping) through the sequence.
    void step_(int delta);
    // Horizontal inset that keeps page content clear of the nav buttons.
    float content_inset_x_() const;
    bool nav_shown_() const;
    void layout_nav_(tk::LayoutCtx& lc, tk::Rect b);
    void paint_nav_(tk::PaintCtx& ctx);
    std::string counter_text_() const;
    // Cached counter-pill layout, rebuilt only when the text changes.
    std::unique_ptr<tk::TextLayout> counter_layout_;
    std::string counter_layout_text_;

    std::unique_ptr<ImageViewerPage> image_page_;
    std::unique_ptr<VideoViewerPage> video_page_;
    std::unique_ptr<AudioViewerPage> audio_page_;
    std::unique_ptr<FileViewerPage> file_page_;
    MediaViewerPage* active_ = nullptr;

    std::vector<MediaViewerItem> items_;
    std::size_t index_ = 0;
    std::uint64_t load_token_ = 0;

    std::function<const tk::Image*(const std::string&)> image_provider_;

    // Previous / next buttons (real tk::Button children, vertically centred at
    // the left / right edges) and the counter pill; only shown for sequences.
    tk::Button* prev_btn_ = nullptr;
    tk::Button* next_btn_ = nullptr;
    std::unique_ptr<tk::Image> prev_icon_;
    std::unique_ptr<tk::Image> next_icon_;
};

} // namespace tesseract::views
