#include "VideoViewerPage.h"
#include "shortcut_registry.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/loading_spinner.h"
#include "tk/svg.h"
#include "tk/theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace tesseract::views
{

namespace
{
constexpr float kVideoViewerMarginX = 64.0f;
constexpr float kVideoViewerMarginY = 72.0f;  // room for controls bar
constexpr float kCtrlBarH = MediaTransportBar::kHeight; // controls bar height
} // namespace

// ── public API ───────────────────────────────────────────────────────────

VideoViewerPage::VideoViewerPage(MediaViewerPageHost& host)
    : host_(host), bar_(host, /*with_speed=*/true)
{
    bar_.on_toggle = [this] { do_play_or_pause(); };
    bar_.on_cycle_speed = [this] { cycle_speed(); };
    bar_.on_seek = [this](float frac) { seek_to_fraction_(frac); };
}

void VideoViewerPage::activate(const MediaViewerItem& item)
{
    source_json_ = item.source;
    thumb_url_ = item.thumbnail;
    mime_type_ = item.mime_type;
    duration_ms_ = item.duration_ms;
    natural_w_ = item.width;
    natural_h_ = item.height;
    rate_ = 1.0f;
    loop_ = item.loop;
    no_audio_ = item.no_audio;
    hide_controls_ = item.hide_controls;
    is_loading_    = true;
    loading_start_ = std::chrono::steady_clock::now();
    active_        = true;
    has_error_     = false;
    is_streaming_  = false;
    stream_buffer_.clear();
    preroll_flushed_ = false;
    stream_total_size_ = 0;
    stream_bytes_fed_  = 0;

    if (video_player_)
    {
        video_player_->stop();
        video_player_->set_loop(loop_);
        video_player_->set_muted(no_audio_);
        video_player_->set_playback_rate(1.0f);
    }
}

void VideoViewerPage::deactivate()
{
    if (video_player_)
    {
        video_player_->stop();
    }
    active_ = false;
    is_loading_ = false;
    is_streaming_ = false;
    stream_buffer_.clear();
    stream_buffer_.shrink_to_fit();
    bar_.deactivate();
}

void VideoViewerPage::load_bytes(const std::uint8_t* data, std::size_t size)
{
    is_loading_ = false;
    has_error_  = false;
    finish_load_(data, size);
}

void VideoViewerPage::finish_load_(const std::uint8_t* data, std::size_t size)
{
    // The overlay may have been closed while the fetch was still in flight;
    // don't let a late arrival start playback (with audio) against a hidden
    // overlay.
    if (!active_ || !data || size == 0 || !video_player_)
    {
        return;
    }
    // Re-apply in case the player was replaced between open() and now.
    video_player_->set_loop(loop_);
    video_player_->set_muted(no_audio_);
    video_player_->play(data, size, mime_type_);
    host_.host_playback_started();
}

void VideoViewerPage::begin_stream_or_buffer()
{
    stream_buffer_.clear();
    preroll_flushed_ = false;
    stream_total_size_ = 0;
    stream_bytes_fed_  = 0;
    if (!active_ || !video_player_)
    {
        is_streaming_ = false;
        return;
    }
    video_player_->set_loop(loop_);
    video_player_->set_muted(no_audio_);
    is_streaming_ = video_player_->begin_stream(mime_type_, 0);
    // A streaming-capable backend starts producing frames on its own
    // schedule from here; a buffering backend keeps showing the thumbnail
    // (is_loading_ stays true) until end_stream() hands it the full buffer,
    // same as the non-streaming load_bytes() path today.
    if (is_streaming_)
    {
        is_loading_ = false;
        host_.host_playback_started();
    }
}

void VideoViewerPage::feed_stream_chunk(const std::uint8_t* data,
                                           std::size_t size)
{
    if (!active_ || !data || size == 0)
    {
        return;
    }
    if (is_streaming_)
    {
        stream_bytes_fed_ += size;
        if (!preroll_flushed_)
        {
            // Still accumulating the initial pre-roll — hold it in
            // stream_buffer_ rather than trickling single chunks into a
            // freshly-started, empty-at-the-time player (see
            // kStreamPrerollBytes's doc comment).
            stream_buffer_.insert(stream_buffer_.end(), data, data + size);
            if (stream_buffer_.size() < kStreamPrerollBytes)
            {
                return;
            }
            preroll_flushed_ = true;
            if (video_player_)
            {
                video_player_->feed_chunk(stream_buffer_.data(),
                                          stream_buffer_.size());
            }
            stream_buffer_.clear();
            stream_buffer_.shrink_to_fit();
            return;
        }
        if (video_player_)
        {
            video_player_->feed_chunk(data, size);
        }
        return;
    }
    stream_buffer_.insert(stream_buffer_.end(), data, data + size);
}

void VideoViewerPage::set_stream_length(std::uint64_t total_size)
{
    if (!is_streaming_)
    {
        return;
    }
    stream_total_size_ = total_size;
    if (video_player_)
    {
        video_player_->set_stream_length(total_size);
    }
}

void VideoViewerPage::end_stream()
{
    if (is_streaming_)
    {
        is_loading_ = false;
        has_error_ = false;
        if (active_ && video_player_)
        {
            // A clip shorter than kStreamPrerollBytes never crosses the
            // pre-roll threshold in feed_stream_chunk(); flush whatever we
            // held back now instead of ending the stream having fed the
            // player nothing at all.
            if (!preroll_flushed_ && !stream_buffer_.empty())
            {
                preroll_flushed_ = true;
                video_player_->feed_chunk(stream_buffer_.data(),
                                          stream_buffer_.size());
                stream_buffer_.clear();
                stream_buffer_.shrink_to_fit();
            }
            video_player_->end_stream();
        }
        return;
    }
    is_loading_ = false;
    has_error_ = false;
    finish_load_(stream_buffer_.data(), stream_buffer_.size());
    stream_buffer_.clear();
    stream_buffer_.shrink_to_fit();
}

void VideoViewerPage::fail_stream()
{
    stream_buffer_.clear();
    stream_buffer_.shrink_to_fit();
    is_loading_ = false;
    if (is_streaming_ && active_ && video_player_)
    {
        // Default tk::VideoPlayer::fail_stream() raises on_error, which sets
        // has_error_ via the callback wired in set_video_player() below.
        video_player_->fail_stream("fetch failed");
        return;
    }
    // Buffering mode (or already closed): no player call to raise on_error
    // through, so set it directly — closes the existing gap where a failed
    // fetch left the thumbnail showing forever with no visible error.
    has_error_ = true;
    host_.host_request_repaint();
}

void VideoViewerPage::set_video_player(
    std::unique_ptr<tk::VideoPlayer> player)
{
    video_player_ = std::move(player);
    if (!video_player_)
    {
        return;
    }

    video_player_->on_frame = [this]() { host_.host_request_repaint(); };
    video_player_->on_progress = [this]() { host_.host_request_repaint(); };
    video_player_->on_error = [this]()
    {
        has_error_ = true;
        host_.host_request_repaint();
    };
}

// ── layout ───────────────────────────────────────────────────────────────

void VideoViewerPage::arrange(tk::LayoutCtx& lc, tk::Rect /*b*/)
{
    recompute_layout(lc);
}

void VideoViewerPage::recompute_layout(tk::LayoutCtx& lc)
{
    const tk::Rect b = host_.host_bounds();
    if (b.w <= 0 || b.h <= 0)
    {
        return;
    }

    host_.host_refresh_chrome_autohide();

    // In full-screen the video fills the window edge-to-edge and the controls
    // bar floats over the bottom of it (auto-hiding with the chrome) rather
    // than reserving a strip of layout below the frame.
    const bool immersive = host_.host_fullscreen();
    const float margin_x = immersive ? 0.0f : kVideoViewerMarginX;
    const float margin_y = immersive ? 0.0f : kVideoViewerMarginY;
    const bool reserve_ctrl = !immersive && !hide_controls_;
    const float ctrl_h = reserve_ctrl ? kCtrlBarH : 0.0f;
    const float avail_h = b.h - margin_y - ctrl_h - (immersive ? 0.0f : 8.0f);
    const float avail_w = b.w - margin_x;

    // Prefer explicit metadata dimensions; when absent, fall back to the
    // decoded frame dimensions so the display rect always has the right
    // aspect ratio even if the Matrix event omitted w/h.
    int use_w = natural_w_, use_h = natural_h_;
    if ((use_w <= 0 || use_h <= 0) && video_player_)
    {
        const tk::Image* f = video_player_->current_frame();
        if (f && f->width() > 0 && f->height() > 0)
        {
            use_w = f->width();
            use_h = f->height();
        }
    }
    tk::Size vs = fit_media(static_cast<float>(use_w),
                            static_cast<float>(use_h), avail_w,
                            std::max(avail_h, 1.0f));
    const float vx = b.x + (b.w - vs.w) * 0.5f;
    const float vy =
        immersive ? b.y + (b.h - vs.h) * 0.5f
                  : b.y + (b.h - vs.h - ctrl_h - 16.0f) * 0.5f;
    video_rect_ = {vx, vy, vs.w, vs.h};

    if (immersive)
    {
        // Floating strip near the bottom of the window, overlapping the video.
        const float bar_w = std::min(vs.w, b.w - 48.0f);
        controls_bar_ = {b.x + (b.w - bar_w) * 0.5f,
                         b.y + b.h - kCtrlBarH - 24.0f, bar_w, kCtrlBarH};
    }
    else
    {
        const float bar_y = vy + vs.h + 8.0f;
        controls_bar_ = {vx, bar_y, vs.w, kCtrlBarH};
    }

    const bool controls_shown =
        active_ && !hide_controls_ && host_.host_chrome_shown();
    bar_.arrange(lc, controls_bar_, controls_shown);
}

bool VideoViewerPage::controls_hovered() const
{
    return bar_.hovered();
}

// ── paint ─────────────────────────────────────────────────────────────────

void VideoViewerPage::paint_content(tk::PaintCtx& ctx)
{
    if (!active_)
    {
        return;
    }

    auto& cv = ctx.canvas;

    // Video frame, thumbnail, or placeholder
    const tk::Image* frame = (video_player_ && !is_loading_)
                                 ? video_player_->current_frame()
                                 : nullptr;
    const tk::Image* thumb = (!frame && !thumb_url_.empty())
                                 ? host_.host_image(thumb_url_)
                                 : nullptr;
    const tk::Image* display = frame ? frame : thumb;

    if (display)
    {
        cv.push_clip_rounded_rect(video_rect_, 4.0f);
        cv.draw_image(*display, video_rect_);
        cv.pop_clip();
    }
    else
    {
        cv.fill_rounded_rect(video_rect_, 4.0f, ctx.theme.palette.chrome_bg);
        cv.stroke_rounded_rect(video_rect_, 4.0f, ctx.theme.palette.border,
                               1.0f);

        if (has_error_)
        {
            const tk::Color col_primary   = tk::Color::rgba(255, 255, 255, 200);
            const tk::Color col_secondary = tk::Color::rgba(255, 255, 255, 110);
            const float     mid_y = video_rect_.y + video_rect_.h * 0.5f;

            tk::TextStyle st{};
            st.role      = tk::FontRole::UiSemibold;
            st.max_width = video_rect_.w - 32.0f;
            auto lo      = ctx.factory.build_text(tk::tr("Unable to play video"), st);
            if (lo)
            {
                tk::Size sz = lo->measure();
                cv.draw_text(
                    *lo,
                    {video_rect_.x + (video_rect_.w - sz.w) * 0.5f,
                     mid_y - sz.h - 2.0f},
                    col_primary);
            }

            tk::TextStyle sub{};
            sub.role      = tk::FontRole::Timestamp;
            sub.max_width = video_rect_.w - 32.0f;
            // \xe2\xac\x87 = ⬇  (matches the save button glyph)
            auto sub_lo = ctx.factory.build_text(
                tk::tr("Use \xe2\xac\x87 to download and play in an external player"),
                sub);
            if (sub_lo)
            {
                tk::Size sz = sub_lo->measure();
                cv.draw_text(
                    *sub_lo,
                    {video_rect_.x + (video_rect_.w - sz.w) * 0.5f,
                     mid_y + 4.0f},
                    col_secondary);
            }
        }
    }

    // Spinning-dots loading indicator while bytes are in flight
    if (is_loading_)
    {
        const float cx = video_rect_.x + video_rect_.w * 0.5f;
        const float cy = video_rect_.y + video_rect_.h * 0.5f;
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loading_start_)
                .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        tk::draw_spinner_dots(cv, {cx, cy}, phase, /*radius=*/14.0f,
                              /*dot_r=*/3.0f, tk::Color{220, 220, 220, 255});
        host_.host_request_repaint();
    }
}

void VideoViewerPage::paint_controls(tk::PaintCtx& ctx)
{
    if (!active_)
    {
        return;
    }
    // ── Controls bar ── (hidden when hide_controls_ is set, and auto-hidden
    // with the chrome in full-screen) ──────────────────────────────────────
    if (!hide_controls_ && host_.host_chrome_shown())
    {
        bar_.paint(ctx, transport_state_());
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool VideoViewerPage::on_content_pointer_down(tk::Point w, tk::Point local)
{
    (void)local;
    // Only reached when the transport bar's buttons (real Button children)
    // declined the press first — see Widget::dispatch_pointer_down.
    if (!hide_controls_ && host_.host_chrome_shown())
    {
        if (bar_.on_pointer_down(w, transport_state_()))
        {
            return true;
        }
    }
    const bool bar_shown = !hide_controls_ && host_.host_chrome_shown();
    if (rect_contains(video_rect_, w) || (bar_shown && bar_.contains(w)))
    {
        // Inside the player/controls but not on a control — consume so the
        // press is not treated as an outside-tap dismiss.
        return true;
    }
    return false;
}

bool VideoViewerPage::on_content_pointer_up(tk::Point /*w*/, tk::Point /*local*/,
                                            bool /*inside_self*/)
{
    return bar_.on_pointer_up();
}

void VideoViewerPage::on_pointer_drag(tk::Point local)
{
    if (bar_.scrubbing())
    {
        // The bar's geometry is in world (root-surface) coordinates — see
        // MediaOverlayBase::handle_pointer_down_'s identical local->world
        // conversion for on_content_pointer_down_'s `w`.
        bar_.on_drag(local.x + host_.host_bounds().x, transport_state_());
    }
}

MediaTransportBar::State VideoViewerPage::transport_state_() const
{
    MediaTransportBar::State s;
    s.position_ms = video_player_ ? video_player_->position_ms() : 0u;
    s.duration_ms = (video_player_ && video_player_->duration_ms() > 0)
                        ? video_player_->duration_ms()
                        : duration_ms_;
    s.playing = video_player_ && video_player_->is_playing();
    s.rate = rate_;
    // How much of the file has downloaded, as a byte fraction (a reasonable
    // stand-in for a time fraction at roughly constant bitrate). Only
    // meaningful while streaming — buffering-mode fetches hand the player
    // nothing until the whole file has arrived.
    if (is_streaming_ && stream_total_size_ > 0)
    {
        s.buffered_frac = std::clamp(static_cast<float>(stream_bytes_fed_) /
                                         static_cast<float>(stream_total_size_),
                                     0.0f, 1.0f);
    }
    return s;
}

void VideoViewerPage::seek_to_fraction_(float frac)
{
    if (!video_player_)
    {
        return;
    }
    const std::uint64_t dur = video_player_->duration_ms() > 0
                                  ? video_player_->duration_ms()
                                  : duration_ms_;
    if (dur > 0)
    {
        video_player_->seek(static_cast<std::uint64_t>(frac * static_cast<float>(dur)));
    }
}

bool VideoViewerPage::on_key(const tk::KeyEvent& e)
{
    if (!video_player_)
        return false;
    if (matches(ShortcutId::VideoPlayPause, e))
        do_play_or_pause();
    else if (matches(ShortcutId::VideoSeekBack, e))
    {
        // While loading / failed (or duration unknown) there is nothing to
        // seek: let the overlay use the arrow for gallery navigation instead.
        if (!seekable_())
            return false;
        seek_by_ms_(-5000);
    }
    else if (matches(ShortcutId::VideoSeekForward, e))
    {
        if (!seekable_())
            return false;
        seek_by_ms_(5000);
    }
    else if (matches(ShortcutId::VideoRestart, e))
        seek_by_ms_(-static_cast<std::int64_t>(video_player_->position_ms()));
    else
        return false;
    return true;
}

bool VideoViewerPage::seekable_() const
{
    if (!video_player_ || is_loading_ || has_error_)
        return false;
    return video_player_->duration_ms() > 0 || duration_ms_ > 0;
}

void VideoViewerPage::seek_by_ms_(std::int64_t delta_ms)
{
    if (!video_player_)
        return;
    const std::uint64_t dur = video_player_->duration_ms() > 0
                                  ? video_player_->duration_ms()
                                  : duration_ms_;
    if (dur == 0)
        return;
    std::int64_t target = static_cast<std::int64_t>(video_player_->position_ms()) + delta_ms;
    std::int64_t limit = static_cast<std::int64_t>(dur);
    if (is_streaming_ && stream_total_size_ > 0)
    {
        const double buffered = std::clamp(static_cast<double>(stream_bytes_fed_) /
                                               static_cast<double>(stream_total_size_),
                                           0.0, 1.0);
        limit = static_cast<std::int64_t>(buffered * static_cast<double>(dur));
    }
    target = std::clamp<std::int64_t>(target, 0, std::max<std::int64_t>(0, limit));
    video_player_->seek(static_cast<std::uint64_t>(target));
}

// ── private helpers ───────────────────────────────────────────────────────

void VideoViewerPage::do_play_or_pause()
{
    if (!video_player_)
    {
        return;
    }
    if (video_player_->is_playing())
    {
        video_player_->pause();
    }
    else
    {
        video_player_->resume();
        host_.host_playback_started();
    }
}

void VideoViewerPage::cycle_speed()
{
    if (!video_player_)
    {
        return;
    }
    if (rate_ < 1.4f)
    {
        rate_ = 1.5f;
    }
    else if (rate_ < 1.9f)
    {
        rate_ = 2.0f;
    }
    else
    {
        rate_ = 1.0f;
    }
    video_player_->set_playback_rate(rate_);
}

} // namespace tesseract::views
