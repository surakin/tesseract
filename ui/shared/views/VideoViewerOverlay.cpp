#include "VideoViewerOverlay.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/svg.h"
#include "tk/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tesseract::views
{

namespace
{
constexpr float kVideoViewerMarginX = 64.0f;
constexpr float kVideoViewerMarginY = 72.0f;  // room for controls bar
constexpr float kCtrlBarH = 56.0f; // controls bar height
constexpr float kCtrlPadX = 10.0f;
constexpr float kPlayBtnD = 36.0f;
constexpr float kSpeedPillW = 32.0f;
constexpr float kSpeedPillH = 20.0f;
constexpr float kDurW = 48.0f; // reserved for "0:00 / 0:00"
constexpr float kScrubH = 6.0f;
constexpr float kScrubR = 3.0f;
} // namespace

// ── helpers ──────────────────────────────────────────────────────────────

static std::string fmt_mm_ss(std::uint64_t ms)
{
    if (ms == 0)
    {
        return "0:00";
    }
    const std::uint64_t s = ms / 1000;
    const std::uint64_t mm = s / 60;
    const std::uint64_t ss = s % 60;
    char buf[48];  // worst case: two 20-digit uint64 fields + ':' + NUL
    std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                  static_cast<unsigned long long>(mm),
                  static_cast<unsigned long long>(ss));
    return buf;
}

// ── public API ───────────────────────────────────────────────────────────

void VideoViewerOverlay::open(std::string source_json, std::string thumb_url,
                              std::string mime_type, std::uint64_t duration_ms,
                              int natural_w, int natural_h, bool loop,
                              bool no_audio, bool hide_controls)
{
    source_json_ = std::move(source_json);
    thumb_url_ = std::move(thumb_url);
    mime_type_ = std::move(mime_type);
    duration_ms_ = duration_ms;
    natural_w_ = natural_w;
    natural_h_ = natural_h;
    rate_ = 1.0f;
    loop_ = loop;
    no_audio_ = no_audio;
    hide_controls_ = hide_controls;
    is_loading_    = true;
    loading_start_ = std::chrono::steady_clock::now();
    is_open_       = true;
    has_error_     = false;

    if (video_player_)
    {
        video_player_->stop();
        video_player_->set_loop(loop_);
        video_player_->set_muted(no_audio_);
        video_player_->set_playback_rate(1.0f);
    }
}

void VideoViewerOverlay::close()
{
    dismiss_();
}

void VideoViewerOverlay::dismiss_()
{
    if (video_player_)
    {
        video_player_->stop();
    }
    is_loading_ = false;
    MediaOverlayBase::dismiss_();
}

void VideoViewerOverlay::load_bytes(const std::uint8_t* data, std::size_t size)
{
    is_loading_ = false;
    has_error_  = false;
    if (!data || size == 0 || !video_player_)
    {
        return;
    }
    // Re-apply in case the player was replaced between open() and load_bytes().
    video_player_->set_loop(loop_);
    video_player_->set_muted(no_audio_);
    video_player_->play(data, size, mime_type_);
}

void VideoViewerOverlay::set_video_player(
    std::unique_ptr<tk::VideoPlayer> player)
{
    video_player_ = std::move(player);
    if (!video_player_)
    {
        return;
    }

    video_player_->on_frame = [this]()
    {
        if (request_repaint_)
        {
            request_repaint_();
        }
    };
    video_player_->on_progress = [this]()
    {
        if (request_repaint_)
        {
            request_repaint_();
        }
    };
    video_player_->on_error = [this]()
    {
        has_error_ = true;
        if (request_repaint_)
        {
            request_repaint_();
        }
    };
}

void VideoViewerOverlay::set_repaint_requester(std::function<void()> fn)
{
    request_repaint_ = std::move(fn);
}

void VideoViewerOverlay::set_image_provider(
    std::function<const tk::Image*(const std::string&)> fn)
{
    image_provider_ = std::move(fn);
}

// ── layout ───────────────────────────────────────────────────────────────

tk::Size VideoViewerOverlay::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void VideoViewerOverlay::arrange(tk::LayoutCtx& lc, tk::Rect b)
{
    tk::Widget::arrange(lc, b);
    recompute_layout();
}

void VideoViewerOverlay::recompute_layout()
{
    const tk::Rect b = bounds();
    if (b.w <= 0 || b.h <= 0)
    {
        return;
    }

    const float ctrl_h = hide_controls_ ? 0.0f : kCtrlBarH;
    const float avail_h = b.h - kVideoViewerMarginY - ctrl_h - 8.0f;
    const float avail_w = b.w - kVideoViewerMarginX;

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
    const float vy = b.y + (b.h - vs.h - ctrl_h - 16.0f) * 0.5f;
    video_rect_ = {vx, vy, vs.w, vs.h};

    const float bar_y = vy + vs.h + 8.0f;
    controls_bar_ = {vx, bar_y, vs.w, kCtrlBarH};

    play_btn_ = {controls_bar_.x + kCtrlPadX,
                 controls_bar_.y + (kCtrlBarH - kPlayBtnD) * 0.5f, kPlayBtnD,
                 kPlayBtnD};

    speed_pill_ = {controls_bar_.x + controls_bar_.w - kCtrlPadX - kSpeedPillW,
                   controls_bar_.y + (kCtrlBarH - kSpeedPillH) * 0.5f,
                   kSpeedPillW, kSpeedPillH};

    const float scrub_x = play_btn_.x + kPlayBtnD + kCtrlPadX;
    const float scrub_xe = speed_pill_.x - kCtrlPadX;
    const float scrub_w = std::max(0.0f, scrub_xe - scrub_x);
    scrub_bar_ = {scrub_x, controls_bar_.y + (kCtrlBarH - kScrubH) * 0.5f,
                  scrub_w, kScrubH};

    layout_chrome_(b);
}

// ── paint ─────────────────────────────────────────────────────────────────

void VideoViewerOverlay::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
    {
        return;
    }

    recompute_layout();

    const tk::Rect b = bounds();
    auto& cv = ctx.canvas;

    // Keep icon_scale_ current before the play glyph (a cached icon) is drawn;
    // the chrome buttons later reuse the same synced scale.
    sync_icon_scale_(ctx);

    // Dark backdrop
    paint_scrim_(ctx);

    // Video frame, thumbnail, or placeholder
    const tk::Image* frame = (video_player_ && !is_loading_)
                                 ? video_player_->current_frame()
                                 : nullptr;
    const tk::Image* thumb = (!frame && image_provider_ && !thumb_url_.empty())
                                 ? image_provider_(thumb_url_)
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
            auto lo      = ctx.factory.build_text("Unable to play video", st);
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
                "Use \xe2\xac\x87 to download and play in an external player",
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
        constexpr float kRadius = 14.0f;
        constexpr float kDotR   = 3.0f;
        constexpr int   kN      = 8;
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loading_start_)
                .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        for (int i = 0; i < kN; ++i)
        {
            const float angle =
                (static_cast<float>(i) / kN + phase) * 2.0f * 3.14159265f;
            const float dx    = std::cos(angle) * kRadius;
            const float dy    = std::sin(angle) * kRadius;
            const float t     = static_cast<float>(i) / kN;
            const auto  alpha = static_cast<uint8_t>(40.0f + 215.0f * t);
            cv.fill_rounded_rect({cx + dx - kDotR, cy + dy - kDotR,
                                   kDotR * 2.0f, kDotR * 2.0f},
                                 kDotR, tk::Color{220, 220, 220, alpha});
        }
        if (request_repaint_)
            request_repaint_();
    }

    // ── Controls bar ── (hidden when hide_controls_ is set) ─────────────
    if (!hide_controls_)
    {

        // Bar background
        cv.fill_rounded_rect(controls_bar_, 8.0f,
                             tk::Color::rgba(0, 0, 0, 150));

        const bool playing = video_player_ && video_player_->is_playing();
        const tk::Color glyph_col = tk::Color::rgba(255, 255, 255, 230);

        // Play / pause button
        cv.fill_rounded_rect(play_btn_, kPlayBtnD * 0.5f,
                             tk::Color::rgba(255, 255, 255, 25));
        if (playing)
        {
            // Two pause bars
            const float bar_w = 3.0f;
            const float bar_h = kPlayBtnD * 0.45f;
            const float gap = 4.0f;
            const float cy = play_btn_.y + (kPlayBtnD - bar_h) * 0.5f;
            const float cx = play_btn_.x + kPlayBtnD * 0.5f;
            cv.fill_rect({cx - gap * 0.5f - bar_w, cy, bar_w, bar_h},
                         glyph_col);
            cv.fill_rect({cx + gap * 0.5f, cy, bar_w, bar_h}, glyph_col);
        }
        else
        {
            // Play glyph (▶): Lucide play icon, tinted to the control colour.
            draw_icon_(ctx, play_btn_, kPlayBtnD * 0.5f, play_icon_, kPlaySvg,
                       glyph_col);
        }

        // Speed pill
        cv.fill_rounded_rect(speed_pill_, kSpeedPillH * 0.5f,
                             tk::Color::rgba(255, 255, 255, 25));
        {
            char rate_buf[8];
            if (rate_ >= 1.99f)
            {
                std::snprintf(rate_buf, sizeof(rate_buf), "2\xC3\x97");
            }
            else if (rate_ >= 1.49f)
            {
                std::snprintf(rate_buf, sizeof(rate_buf), "1.5\xC3\x97");
            }
            else
            {
                std::snprintf(rate_buf, sizeof(rate_buf), "1\xC3\x97");
            }
            tk::TextStyle rs{};
            rs.role = tk::FontRole::Timestamp;
            auto rate_lo = ctx.factory.build_text(rate_buf, rs);
            if (rate_lo)
            {
                tk::Size sz = rate_lo->measure();
                cv.draw_text(*rate_lo,
                             {speed_pill_.x + (speed_pill_.w - sz.w) * 0.5f,
                              speed_pill_.y + (speed_pill_.h - sz.h) * 0.5f},
                             glyph_col);
            }
        }

        // Scrub bar — track + filled region + playhead
        if (scrub_bar_.w > 0)
        {
            const std::uint64_t pos =
                video_player_ ? video_player_->position_ms() : 0u;
            const std::uint64_t dur =
                (video_player_ && video_player_->duration_ms() > 0)
                    ? video_player_->duration_ms()
                    : duration_ms_;
            const float frac = (dur > 0)
                                   ? std::clamp(static_cast<float>(pos) /
                                                    static_cast<float>(dur),
                                                0.0f, 1.0f)
                                   : 0.0f;

            // Track background
            cv.fill_rounded_rect(scrub_bar_, kScrubR,
                                 tk::Color::rgba(255, 255, 255, 60));

            // Filled portion
            if (frac > 0.0f)
            {
                const float filled_w = scrub_bar_.w * frac;
                cv.fill_rounded_rect(
                    {scrub_bar_.x, scrub_bar_.y, filled_w, scrub_bar_.h},
                    kScrubR, tk::Color::rgba(255, 255, 255, 200));
            }

            // Playhead knob
            constexpr float kKnobD = 12.0f;
            const float kx = scrub_bar_.x + scrub_bar_.w * frac - kKnobD * 0.5f;
            const float ky = scrub_bar_.y + (scrub_bar_.h - kKnobD) * 0.5f;
            cv.fill_rounded_rect({kx, ky, kKnobD, kKnobD}, kKnobD * 0.5f,
                                 glyph_col);
        }

    } // end if (!hide_controls_)

    // × close + ⬇ save chrome buttons — always visible so the user can
    // dismiss / save (shared scaffolding).
    paint_chrome_buttons_(ctx);
}

// ── pointer events ────────────────────────────────────────────────────────

bool VideoViewerOverlay::on_pointer_down(tk::Point local)
{
    return handle_pointer_down_(local);
}

void VideoViewerOverlay::on_pointer_up(tk::Point local, bool inside_self)
{
    handle_pointer_up_(local, inside_self);
}

bool VideoViewerOverlay::on_wheel(tk::Point /*local*/, float /*dx*/,
                                  float /*dy*/, bool /*is_touchpad*/)
{
    return is_open();
}

bool VideoViewerOverlay::on_content_pointer_down_(tk::Point w, tk::Point local)
{
    (void)local;
    if (!hide_controls_)
    {
        if (rect_contains(play_btn_, w))
        {
            press_play_ = true;
            return true;
        }
        if (rect_contains(speed_pill_, w))
        {
            press_speed_ = true;
            return true;
        }
        if (rect_contains(scrub_bar_, w))
        {
            press_scrub_ = true;
            // Immediate seek on press for snappy scrub-start.
            if (video_player_ && scrub_bar_.w > 0)
            {
                const float frac =
                    std::clamp((w.x - scrub_bar_.x) / scrub_bar_.w, 0.0f, 1.0f);
                const std::uint64_t dur = video_player_->duration_ms() > 0
                                              ? video_player_->duration_ms()
                                              : duration_ms_;
                if (dur > 0)
                {
                    video_player_->seek(static_cast<std::uint64_t>(
                        frac * static_cast<float>(dur)));
                }
            }
            return true;
        }
    }
    if (rect_contains(video_rect_, w) || rect_contains(controls_bar_, w))
    {
        // Inside the player/controls but not on a control — consume so the
        // press is not treated as an outside-tap dismiss.
        return true;
    }
    return false;
}

bool VideoViewerOverlay::on_content_pointer_up_(tk::Point w, tk::Point /*local*/,
                                                bool inside_self)
{
    if (press_play_)
    {
        press_play_ = false;
        if (inside_self && rect_contains(play_btn_, w))
        {
            do_play_or_pause();
        }
        return true;
    }
    if (press_speed_)
    {
        press_speed_ = false;
        if (inside_self && rect_contains(speed_pill_, w))
        {
            cycle_speed();
        }
        return true;
    }
    if (press_scrub_)
    {
        press_scrub_ = false;
        return true;
    }
    return false;
}

void VideoViewerOverlay::fire_save_()
{
    on_save(source_json_, mime_type_);
}

void VideoViewerOverlay::on_icon_scale_changed_()
{
    play_icon_.reset();
}

// ── private helpers ───────────────────────────────────────────────────────

void VideoViewerOverlay::do_play_or_pause()
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
    }
}

void VideoViewerOverlay::cycle_speed()
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
