#include "TimelineMediaController.h"

#include "MessageListView.h" // MessageRowData (full definition)

#include "icons.h" // kPlaySvg
#include "tk/svg.h"
#include "tk/theme.h"
#include "tk/widget.h" // tk::PaintCtx

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace tesseract::views
{

namespace
{

// Card-internal layout constants. These live here (rather than in
// MessageListView.cpp) because only the card paint below consumes them; the
// row-height constants (kVoiceCardH/W, kAudioCardH/W) stay in the view, where
// measure() uses them.
constexpr float kTimelineAudioPlayBtnSize = 32.0f;
constexpr float kTimelineAudioCardPadX    = 8.0f;
constexpr float kTimelineAudioTrackH      = 4.0f; // progress track height
constexpr float kTimelineVoicePlayBtnSize = 32.0f;
constexpr float kTimelineVoiceCardPadX    = 8.0f;
constexpr float kTimelineVoiceBarW        = 3.0f;
constexpr float kTimelineVoiceBarGap      = 2.0f;
constexpr float kTimelineVoiceBarMinH     = 3.0f;  // placeholder bar height
constexpr float kTimelineVoiceDurationW   = 40.0f; // reserved for "0:00" label
constexpr float kTimelineVoiceSpeedPillW  = 30.0f; // "1x" / "1.5x" / "2x"
constexpr float kTimelineVoiceSpeedPillH  = 20.0f;

// Local copies of the small time/size formatters. The originals are
// file-local statics in MessageListView.cpp (still used there by other
// cards), so duplicating these two trivial helpers keeps both translation
// units self-contained without a shared header.
std::string tmc_format_mmss(std::uint64_t ms)
{
    if (ms == 0)
    {
        return "0:00";
    }
    std::uint64_t total_s = ms / 1000;
    std::uint64_t mm      = total_s / 60;
    std::uint64_t ss      = total_s % 60;
    char          buf[48]; // worst case: two 20-digit uint64 fields + ':' + NUL
    std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                  static_cast<unsigned long long>(mm),
                  static_cast<unsigned long long>(ss));
    return std::string(buf);
}

std::string tmc_format_size(std::uint64_t bytes)
{
    if (bytes < 1024)
    {
        return std::to_string(bytes) + " B";
    }
    if (bytes < 1024 * 1024)
    {
        return std::to_string(bytes / 1024) + " KB";
    }
    if (bytes < 1024ull * 1024 * 1024)
    {
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    }
    return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
}

} // namespace

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

void TimelineMediaController::set_player(std::unique_ptr<tk::AudioPlayer> player)
{
    audio_player_ = std::move(player);
    if (audio_player_)
    {
        audio_player_->on_progress = [this]() { on_audio_progress(); };
    }
}

void TimelineMediaController::set_bytes_provider(VoiceBytesProvider provider)
{
    voice_bytes_provider_ = std::move(provider);
}

void TimelineMediaController::set_repaint(std::function<void()> request_repaint)
{
    request_repaint_ = std::move(request_repaint);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void TimelineMediaController::handle_voice_scrub_at(const MessageRowData& row,
                                                    float                 world_x)
{
    if (!audio_player_ || !voice_bytes_provider_)
    {
        return;
    }
    auto it = voice_card_geom_.find(row.event_id);
    if (it == voice_card_geom_.end())
    {
        return;
    }
    const tk::Rect& strip = it->second.waveform_strip;
    if (strip.w <= 0.0f)
    {
        return;
    }

    float frac = (world_x - strip.x) / strip.w;
    if (frac < 0.0f)
    {
        frac = 0.0f;
    }
    if (frac > 1.0f)
    {
        frac = 1.0f;
    }

    // Resolve total duration: the sender's metadata if present, otherwise
    // whatever the backend has discovered after loading.
    std::uint64_t total = row.duration_ms > 0 ? row.duration_ms : 0;
    if (total == 0 && row.event_id == playing_event_id_)
    {
        total = audio_player_->duration_ms();
    }
    if (total == 0)
    {
        return;
    }

    const std::uint64_t target_ms =
        static_cast<std::uint64_t>(frac * static_cast<float>(total));

    if (row.event_id != playing_event_id_)
    {
        // Start playback at the clicked position. Same byte-cache path
        // as a regular play click; on cache miss the view stays idle and
        // the user can try again once the prefetch lands.
        std::vector<std::uint8_t> bytes = voice_bytes_provider_(
            row.audio_source ? row.audio_source->fetch_token() : std::string{});
        if (bytes.empty())
        {
            if (request_repaint_)
            {
                request_repaint_();
            }
            return;
        }
        if (!playing_event_id_.empty())
        {
            switching_clip_ = true;
            audio_player_->stop();
            switching_clip_ = false;
        }
        playing_event_id_    = row.event_id;
        playing_position_ms_ = target_ms;
        playing_is_active_   = true;
        playing_ever_active_ = false;
        audio_player_->set_playback_rate(playback_rate_);
        audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
        audio_player_->seek(target_ms);
        if (request_repaint_)
        {
            request_repaint_();
        }
        return;
    }

    audio_player_->seek(target_ms);
    playing_position_ms_ = target_ms;
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void TimelineMediaController::handle_voice_speed_click()
{
    if (!audio_player_)
    {
        return;
    }
    if (playback_rate_ < 1.49f)
    {
        playback_rate_ = 1.5f;
    }
    else if (playback_rate_ < 1.99f)
    {
        playback_rate_ = 2.0f;
    }
    else
    {
        playback_rate_ = 1.0f;
    }
    audio_player_->set_playback_rate(playback_rate_);
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void TimelineMediaController::on_audio_progress()
{
    if (audio_player_)
    {
        playing_position_ms_ = audio_player_->position_ms();
        playing_is_active_   = audio_player_->is_playing();
        if (playing_is_active_)
        {
            playing_ever_active_ = true;
        }
        // Only treat this as "clip ended" once the clip was confirmed to have
        // started (playing_ever_active_) — without that guard the same
        // condition fires during the async-load window (is_playing()==false
        // while the clip is still loading), clearing playing_event_id_ before
        // the first repaint so the play button never flips to active.
        //
        // "Ended" itself is position_ms()==0 OR the backend's own reached_end()
        // signal — several backends (e.g. Qt's QMediaPlayer) do not reset
        // position_ms() to 0 on natural completion, leaving it at/near the
        // clip's duration, so position==0 alone misses that case entirely.
        if (!playing_is_active_ &&
            (playing_position_ms_ == 0 || audio_player_->reached_end()) &&
            playing_ever_active_)
        {
            std::string finished_event_id = playing_event_id_;
            playing_event_id_.clear();
            playing_ever_active_ = false;
            if (!switching_clip_ && next_voice_lookup_ &&
                !finished_event_id.empty())
            {
                if (const MessageRowData* next =
                        next_voice_lookup_(finished_event_id))
                {
                    handle_voice_play_click(*next, /*is_auto_advance=*/true);
                    return;
                }
            }
        }
    }
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void TimelineMediaController::handle_voice_play_click(const MessageRowData& row,
                                                       bool is_auto_advance)
{
    if (!audio_player_ || !voice_bytes_provider_)
    {
        return;
    }

    // Clicking the currently-active row's play button toggles pause.
    if (row.event_id == playing_event_id_)
    {
        if (audio_player_->is_playing())
        {
            audio_player_->pause();
        }
        else
        {
            audio_player_->resume();
        }
        on_audio_progress();
        return;
    }

    // Switching rows — stop the current clip cleanly first.
    if (!playing_event_id_.empty())
    {
        switching_clip_ = true;
        audio_player_->stop();
        switching_clip_ = false;
    }

    std::vector<std::uint8_t> bytes = voice_bytes_provider_(
        row.audio_source ? row.audio_source->fetch_token() : std::string{});
    if (bytes.empty())
    {
        // Cache miss — the SDK kicks off a background fetch on the first
        // call. Surface state honestly: nothing is loaded, repaint so the
        // pause glyph (if any) reverts to play. Arm a pending play so the
        // relayout fired when the bytes arrive auto-starts playback — the
        // user's single click is enough.
        playing_event_id_.clear();
        playing_is_active_     = false;
        playing_ever_active_   = false;
        playing_position_ms_   = 0;
        pending_play_event_id_ = row.event_id;
        pending_play_token_ =
            row.audio_source ? row.audio_source->fetch_token() : std::string{};
        pending_play_mime_     = row.audio_mime;
        pending_play_is_voice_ = true;
        pending_play_skip_visibility_gate_ = is_auto_advance;
        if (request_repaint_)
        {
            request_repaint_();
        }
        return;
    }
    reset_pending_play();
    playing_event_id_    = row.event_id;
    playing_position_ms_ = 0;
    playing_is_active_   = true;
    playing_ever_active_ = false;
    audio_player_->set_playback_rate(playback_rate_);
    audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void TimelineMediaController::handle_audio_play_click(const MessageRowData& row)
{
    if (!audio_player_ || !voice_bytes_provider_)
    {
        return;
    }
    if (row.event_id == playing_event_id_)
    {
        if (audio_player_->is_playing())
        {
            audio_player_->pause();
        }
        else
        {
            audio_player_->resume();
        }
        on_audio_progress();
        return;
    }
    if (!playing_event_id_.empty())
    {
        switching_clip_ = true;
        audio_player_->stop();
        switching_clip_ = false;
    }
    std::vector<std::uint8_t> bytes = voice_bytes_provider_(
        row.audio_source ? row.audio_source->fetch_token() : std::string{});
    if (bytes.empty())
    {
        // Cache miss — arm a pending play so the relayout that fires when the
        // bytes arrive auto-starts playback (single-click behavior).
        playing_event_id_.clear();
        playing_is_active_     = false;
        playing_ever_active_   = false;
        playing_position_ms_   = 0;
        pending_play_event_id_ = row.event_id;
        pending_play_token_ =
            row.audio_source ? row.audio_source->fetch_token() : std::string{};
        pending_play_mime_     = row.audio_mime;
        pending_play_is_voice_ = false;
        if (request_repaint_)
        {
            request_repaint_();
        }
        return;
    }
    reset_pending_play();
    playing_event_id_    = row.event_id;
    playing_position_ms_ = 0;
    playing_is_active_   = true;
    playing_ever_active_ = false;
    audio_player_->set_playback_rate(1.0f);
    audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void TimelineMediaController::reset_pending_play()
{
    pending_play_event_id_.clear();
    pending_play_token_.clear();
    pending_play_mime_.clear();
    pending_play_is_voice_ = false;
    pending_play_skip_visibility_gate_ = false;
}

void TimelineMediaController::stop_active_playback()
{
    if (audio_player_ && !playing_event_id_.empty())
    {
        switching_clip_ = true;
        audio_player_->stop();
        switching_clip_ = false;
        playing_event_id_.clear();
        playing_is_active_   = false;
        playing_ever_active_ = false;
        playing_position_ms_ = 0;
    }
}

void TimelineMediaController::retry_pending_voice_play()
{
    if (pending_play_event_id_.empty() || !audio_player_ || !voice_bytes_provider_)
    {
        return;
    }
    // The fetch armed on the cache-miss click may have completed and warmed
    // the cache; if so the provider hands back the bytes now. If it's still
    // in flight the provider returns empty (and won't re-fetch) — leave the
    // pending state set so a later relayout retries.
    std::vector<std::uint8_t> bytes = voice_bytes_provider_(pending_play_token_);
    if (bytes.empty())
    {
        return;
    }
    playing_event_id_    = pending_play_event_id_;
    playing_position_ms_ = 0;
    playing_is_active_   = true;
    playing_ever_active_ = false;
    audio_player_->set_playback_rate(pending_play_is_voice_ ? playback_rate_ : 1.0f);
    audio_player_->play(bytes.data(), bytes.size(), pending_play_mime_);
    reset_pending_play();
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void TimelineMediaController::handle_audio_scrub_at(const MessageRowData& row,
                                                    float                 world_x)
{
    if (!audio_player_ || !voice_bytes_provider_)
    {
        return;
    }
    auto it = audio_card_geom_.find(row.event_id);
    if (it == audio_card_geom_.end())
    {
        return;
    }
    const tk::Rect& track = it->second.progress_track;
    if (track.w <= 0.0f)
    {
        return;
    }
    float frac = (world_x - track.x) / track.w;
    frac       = std::max(0.0f, std::min(1.0f, frac));

    std::uint64_t total = row.duration_ms > 0 ? row.duration_ms : 0;
    if (total == 0 && row.event_id == playing_event_id_)
    {
        total = audio_player_->duration_ms();
    }
    if (total == 0)
    {
        return;
    }
    const std::uint64_t target_ms =
        static_cast<std::uint64_t>(frac * static_cast<float>(total));

    if (row.event_id != playing_event_id_)
    {
        std::vector<std::uint8_t> bytes = voice_bytes_provider_(
            row.audio_source ? row.audio_source->fetch_token() : std::string{});
        if (bytes.empty())
        {
            if (request_repaint_)
            {
                request_repaint_();
            }
            return;
        }
        if (!playing_event_id_.empty())
        {
            switching_clip_ = true;
            audio_player_->stop();
            switching_clip_ = false;
        }
        playing_event_id_    = row.event_id;
        playing_position_ms_ = target_ms;
        playing_is_active_   = true;
        playing_ever_active_ = false;
        audio_player_->set_playback_rate(1.0f);
        audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
        audio_player_->seek(target_ms);
        if (request_repaint_)
        {
            request_repaint_();
        }
        return;
    }
    audio_player_->seek(target_ms);
    playing_position_ms_ = target_ms;
    if (request_repaint_)
    {
        request_repaint_();
    }
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void TimelineMediaController::paint_voice_card(const MessageRowData& m,
                                               tk::PaintCtx& ctx, tk::Rect dst,
                                               tk::IconCache& play_icon)
{
    // Card chrome.
    ctx.canvas.fill_rounded_rect(dst, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(dst, 10.0f, ctx.theme.palette.border, 1.0f);

    // Play / pause button (circle on the left).
    const float btn_d = kTimelineVoicePlayBtnSize;
    const float btn_x = dst.x + kTimelineVoiceCardPadX;
    const float btn_y = dst.y + (dst.h - btn_d) * 0.5f;
    tk::Rect    btn_rect{btn_x, btn_y, btn_d, btn_d};
    ctx.canvas.fill_rounded_rect(btn_rect, btn_d * 0.5f, ctx.theme.palette.accent);

    const bool is_active_row =
        m.event_id == playing_event_id_ && playing_is_active_;

    const tk::Color glyph_col = ctx.theme.palette.text_on_accent;
    if (is_active_row)
    {
        // Two pause bars centred in the button.
        const float bar_w = 4.0f;
        const float bar_h = btn_d * 0.45f;
        const float gap   = 4.0f;
        const float cy    = btn_y + (btn_d - bar_h) * 0.5f;
        const float cx    = btn_x + btn_d * 0.5f;
        ctx.canvas.fill_rect({cx - gap * 0.5f - bar_w, cy, bar_w, bar_h},
                             glyph_col);
        ctx.canvas.fill_rect({cx + gap * 0.5f, cy, bar_w, bar_h}, glyph_col);
    }
    else
    {
        // Play glyph (>): Lucide play icon, tinted to the on-accent colour.
        play_icon.draw(ctx.canvas, ctx.factory, kPlaySvg, btn_rect, btn_d * 0.55f,
                       glyph_col);
    }

    // Right-justified duration label. When this row is the active one
    // and the backend reports a non-zero position, show remaining time
    // instead of total — matches Element / FluffyChat affordances.
    std::uint64_t total =
        m.duration_ms > 0
            ? m.duration_ms
            : (m.event_id == playing_event_id_
                   ? audio_player_ ? audio_player_->duration_ms() : 0u
                   : 0u);
    std::uint64_t label_ms = total;
    if (is_active_row && total > 0 && playing_position_ms_ <= total)
    {
        label_ms = total - playing_position_ms_;
    }
    tk::TextStyle ds{};
    ds.role           = tk::FontRole::Timestamp;
    auto     dur_lo   = ctx.factory.build_text(tmc_format_mmss(label_ms), ds);
    tk::Size dur_sz   = dur_lo ? dur_lo->measure() : tk::Size{};
    float    dur_w    = dur_sz.w;
    float    dur_h    = dur_sz.h;
    if (dur_lo)
    {
        ctx.canvas.draw_text(*dur_lo,
                             {dst.x + dst.w - kTimelineVoiceCardPadX - dur_w,
                              dst.y + (dst.h - dur_h) * 0.5f},
                             ctx.theme.palette.text_secondary);
    }

    // Speed pill — sits just to the left of the duration label. Only
    // the active row's pill is interactive; the rate is global, so
    // rendering it on every row would be a lie about scope.
    tk::Rect pill_rect{};
    if (audio_player_ && is_active_row)
    {
        const float pill_x =
            dst.x + dst.w - kTimelineVoiceCardPadX - dur_w - 6.0f - kTimelineVoiceSpeedPillW;
        const float pill_y = dst.y + (dst.h - kTimelineVoiceSpeedPillH) * 0.5f;
        pill_rect          = {pill_x, pill_y, kTimelineVoiceSpeedPillW, kTimelineVoiceSpeedPillH};
        ctx.canvas.fill_rounded_rect(pill_rect, kTimelineVoiceSpeedPillH * 0.5f,
                                     ctx.theme.palette.subtle_hover);

        char        rate_buf[8];
        const float r = playback_rate_;
        if (r >= 1.99f)
        {
            std::snprintf(rate_buf, sizeof(rate_buf), "2\xc3\x97");
        }
        else if (r >= 1.49f)
        {
            std::snprintf(rate_buf, sizeof(rate_buf), "1.5\xc3\x97");
        }
        else
        {
            std::snprintf(rate_buf, sizeof(rate_buf), "1\xc3\x97");
        }
        tk::TextStyle rs{};
        rs.role       = tk::FontRole::Timestamp;
        auto rate_lo  = ctx.factory.build_text(rate_buf, rs);
        if (rate_lo)
        {
            tk::Size rsz = rate_lo->measure();
            ctx.canvas.draw_text(*rate_lo,
                                 {pill_rect.x + (pill_rect.w - rsz.w) * 0.5f,
                                  pill_rect.y + (pill_rect.h - rsz.h) * 0.5f},
                                 ctx.theme.palette.text_secondary);
        }
    }

    // Waveform strip. Sits between the play button and the duration
    // label (squeezed further when the speed pill is showing). Bars
    // to the left of the cursor render in the accent colour; bars to
    // the right stay muted. When the sender omitted the MSC1767
    // waveform, we paint a flat row of minimum-height bars so the
    // card still has visual rhythm.
    const float strip_x = btn_x + btn_d + kTimelineVoiceCardPadX;
    const float right_anchor =
        (pill_rect.w > 0.0f)
            ? pill_rect.x - 6.0f
            : dst.x + dst.w - kTimelineVoiceCardPadX - kTimelineVoiceDurationW;
    const float strip_w_avail = right_anchor - strip_x;
    if (strip_w_avail < kTimelineVoiceBarW)
    {
        return;
    }
    const float strip_y = dst.y + dst.h * 0.5f;
    const float strip_h = dst.h - 16.0f;

    const float step = kTimelineVoiceBarW + kTimelineVoiceBarGap;
    const int   bars = std::max(1, static_cast<int>(strip_w_avail / step));

    // Resample sender waveform -> `bars` buckets. When empty, the loop
    // below uses the placeholder height. Senders often record voice well
    // below the spec's 0..=1024 ceiling (a normal-volume voice peaks at
    // ~10% of full scale), so normalise by the per-message peak instead
    // of the spec ceiling — otherwise quiet recordings collapse into a
    // flat row of minimum-height bars.
    std::uint16_t wf_peak = 0;
    for (std::uint16_t v : m.waveform)
        if (v > wf_peak) wf_peak = v;
    const float wf_norm =
        wf_peak > 0 ? 1.0f / static_cast<float>(wf_peak) : 0.0f;
    auto amp_at = [&](int i) -> float
    {
        if (m.waveform.empty())
        {
            return 0.0f;
        }
        const std::size_t n   = m.waveform.size();
        const std::size_t src = std::min<std::size_t>(
            n - 1, static_cast<std::size_t>(static_cast<double>(i) / bars *
                                            static_cast<double>(n)));
        return std::min(1.0f, static_cast<float>(m.waveform[src]) * wf_norm);
    };

    float cursor_frac = 0.0f;
    if (is_active_row && total > 0)
    {
        cursor_frac =
            static_cast<float>(playing_position_ms_) / static_cast<float>(total);
        if (cursor_frac > 1.0f)
        {
            cursor_frac = 1.0f;
        }
    }
    const int cursor_bar = static_cast<int>(cursor_frac * bars);

    for (int i = 0; i < bars; ++i)
    {
        float a     = amp_at(i);
        float bar_h = m.waveform.empty()
                          ? kTimelineVoiceBarMinH
                          : std::max(kTimelineVoiceBarMinH, a * strip_h);
        float bx    = strip_x + i * step;
        float by    = strip_y - bar_h * 0.5f;
        tk::Color c = (i < cursor_bar) ? ctx.theme.palette.accent
                                       : ctx.theme.palette.text_muted;
        ctx.canvas.fill_rounded_rect({bx, by, kTimelineVoiceBarW, bar_h},
                                     kTimelineVoiceBarW * 0.5f, c);
    }

    // Record world-coord geometry so on_pointer_down can hit-test
    // the play button, waveform strip, and speed pill without
    // re-running the layout maths.
    if (!m.event_id.empty())
    {
        tk::Rect strip_rect{
            strip_x,
            strip_y - strip_h * 0.5f,
            strip_w_avail,
            strip_h,
        };
        voice_card_geom_[m.event_id] =
            VoiceCardGeom{m.event_id, btn_rect, strip_rect, pill_rect, dst};
    }
}

void TimelineMediaController::paint_audio_card(const MessageRowData& m,
                                               tk::PaintCtx& ctx, tk::Rect dst,
                                               tk::IconCache& play_icon)
{
    // Card chrome — same radius as voice card.
    ctx.canvas.fill_rounded_rect(dst, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(dst, 10.0f, ctx.theme.palette.border, 1.0f);

    // Layout:
    //   Row 1 (top 36 px): [play btn 32px] [track] [time label]
    //   Row 2 (bottom 24 px): filename · size
    const float row1_h  = 36.0f;
    const float row1_cy = dst.y + row1_h * 0.5f;

    // Play / pause button — identical to voice card.
    const float btn_d = kTimelineAudioPlayBtnSize;
    const float btn_x = dst.x + kTimelineAudioCardPadX;
    const float btn_y = row1_cy - btn_d * 0.5f;
    tk::Rect    btn_rect{btn_x, btn_y, btn_d, btn_d};
    ctx.canvas.fill_rounded_rect(btn_rect, btn_d * 0.5f, ctx.theme.palette.accent);

    const bool is_active_row =
        m.event_id == playing_event_id_ && playing_is_active_;
    const tk::Color glyph_col = ctx.theme.palette.text_on_accent;
    if (is_active_row)
    {
        const float bar_w = 4.0f;
        const float bar_h = btn_d * 0.45f;
        const float gap   = 4.0f;
        const float cy    = btn_y + (btn_d - bar_h) * 0.5f;
        const float cx    = btn_x + btn_d * 0.5f;
        ctx.canvas.fill_rect({cx - gap * 0.5f - bar_w, cy, bar_w, bar_h},
                             glyph_col);
        ctx.canvas.fill_rect({cx + gap * 0.5f, cy, bar_w, bar_h}, glyph_col);
    }
    else
    {
        // Play glyph (>): Lucide play icon, tinted to the on-accent colour.
        play_icon.draw(ctx.canvas, ctx.factory, kPlaySvg, btn_rect, btn_d * 0.55f,
                       glyph_col);
    }

    // Duration / elapsed time label — right-justified in row 1.
    std::uint64_t total =
        m.duration_ms > 0
            ? m.duration_ms
            : (m.event_id == playing_event_id_ && audio_player_
                   ? audio_player_->duration_ms()
                   : 0u);
    tk::TextStyle ds{};
    ds.role                = tk::FontRole::Timestamp;
    std::uint64_t label_ms = (is_active_row && total > 0 &&
                              playing_position_ms_ <= total)
                                 ? (total - playing_position_ms_)
                                 : total;
    auto        dur_lo     = ctx.factory.build_text(tmc_format_mmss(label_ms), ds);
    tk::Size    dur_sz     = dur_lo ? dur_lo->measure() : tk::Size{};
    const float time_x     = dst.x + dst.w - kTimelineAudioCardPadX - dur_sz.w;
    if (dur_lo)
    {
        ctx.canvas.draw_text(*dur_lo, {time_x, row1_cy - dur_sz.h * 0.5f},
                             ctx.theme.palette.text_secondary);
    }

    // Linear progress track — between play button and time label.
    const float track_x     = btn_x + btn_d + kTimelineAudioCardPadX;
    const float track_right = time_x - kTimelineAudioCardPadX;
    const float track_w     = track_right - track_x;
    const float track_y     = row1_cy - kTimelineAudioTrackH * 0.5f;
    tk::Rect    track_bg{track_x, track_y, track_w, kTimelineAudioTrackH};
    if (track_w > 0.0f)
    {
        ctx.canvas.fill_rounded_rect(track_bg, kTimelineAudioTrackH * 0.5f,
                                     ctx.theme.palette.text_muted);

        float fill_frac = 0.0f;
        if (is_active_row && total > 0)
        {
            fill_frac = static_cast<float>(playing_position_ms_) /
                        static_cast<float>(total);
            if (fill_frac > 1.0f) fill_frac = 1.0f;
        }
        if (fill_frac > 0.0f)
        {
            ctx.canvas.fill_rounded_rect(
                {track_x, track_y, track_w * fill_frac, kTimelineAudioTrackH},
                kTimelineAudioTrackH * 0.5f, ctx.theme.palette.accent);
        }
    }

    // Row 2: filename · size.
    const float       row2_y = dst.y + row1_h + 4.0f;
    const float       name_x = btn_x + btn_d + kTimelineAudioCardPadX;
    const float       name_w = dst.x + dst.w - kTimelineAudioCardPadX - name_x;
    const std::string display_name = m.file_name.empty() ? m.body : m.file_name;
    const std::string meta =
        m.file_size > 0
            ? display_name + " \xc2\xb7 " + tmc_format_size(m.file_size)
            : display_name;
    tk::TextStyle ns{};
    ns.role      = tk::FontRole::Timestamp;
    ns.trim      = tk::TextTrim::Ellipsis;
    ns.max_width = name_w;
    auto name_lo = ctx.factory.build_text(meta, ns);
    if (name_lo)
    {
        ctx.canvas.draw_text(*name_lo, {name_x, row2_y},
                             ctx.theme.palette.text_secondary);
    }

    // Record hit-test geometry.
    if (!m.event_id.empty())
    {
        tk::Rect full_track{track_x, dst.y, track_w,
                            row1_h}; // full row-1 height for easier tapping
        audio_card_geom_[m.event_id] =
            AudioCardGeom{m.event_id, btn_rect, full_track, dst};
    }
}

} // namespace tesseract::views
