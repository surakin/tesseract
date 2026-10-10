#include "AudioViewerPage.h"
#include "icons.h"
#include "media_utils.h"
#include "shortcut_registry.h"

#include "tk/i18n.h"
#include "tk/loading_spinner.h"
#include "tk/theme.h"

#include <algorithm>
#include <cmath>

namespace tesseract::views
{

namespace
{
constexpr float kAudioCardMaxW = 520.0f;
constexpr float kAudioCardMinH = 300.0f;
constexpr float kAudioCardPad = 20.0f;
constexpr float kAudioGlyphPx = 48.0f;
constexpr float kAudioWaveH = 56.0f;
constexpr float kAudioWaveBarW = 3.0f;
constexpr float kAudioWaveBarGap = 2.0f;
constexpr float kAudioWaveBarMinH = 3.0f;
constexpr tk::Color kAudioCardFill = tk::Color::rgba(28, 28, 32, 235);
constexpr tk::Color kAudioCardBorder = tk::Color::rgba(255, 255, 255, 36);
constexpr tk::Color kAudioTextPrimary = tk::Color::rgba(255, 255, 255, 235);
constexpr tk::Color kAudioTextSecondary = tk::Color::rgba(255, 255, 255, 140);
constexpr tk::Color kAudioBarPlayed = tk::Color::rgba(255, 255, 255, 220);
constexpr tk::Color kAudioBarRest = tk::Color::rgba(255, 255, 255, 80);
} // namespace

AudioViewerPage::AudioViewerPage(MediaViewerPageHost& host)
    : host_(host), bar_(host, /*with_speed=*/true)
{
    bar_.on_toggle = [this] { toggle_(); };
    bar_.on_cycle_speed = [this] { cycle_speed_(); };
    bar_.on_seek = [this](float frac) { seek_to_fraction_(frac); };
}

void AudioViewerPage::set_audio_player(std::unique_ptr<tk::AudioPlayer> player)
{
    player_ = std::move(player);
    if (player_)
    {
        player_->on_progress = [this] { on_player_progress_(); };
        player_->on_error = [this] { on_player_error_(); };
        player_->set_playback_rate(rate_);
    }
    if (active_)
    {
        // A player that arrives after activation means playback is possible
        // after all: wait for the bytes.
        is_loading_ = player_ != nullptr && loaded_bytes_ == 0 && !has_error_;
        loading_start_ = std::chrono::steady_clock::now();
        host_.host_request_repaint();
    }
}

void AudioViewerPage::activate(const MediaViewerItem& item)
{
    filename_ = item.filename;
    mime_type_ = item.mime_type;
    caption_ = item.caption;
    duration_ms_ = item.duration_ms;
    waveform_ = item.waveform;
    rate_ = 1.0f;
    active_ = true;
    has_error_ = false;
    loaded_bytes_ = 0;
    is_loading_ = player_ != nullptr; // no player: straight to the message
    loading_start_ = std::chrono::steady_clock::now();
    if (player_)
    {
        player_->stop();
        player_->set_playback_rate(1.0f);
    }
}

void AudioViewerPage::deactivate()
{
    if (player_)
    {
        player_->stop();
    }
    active_ = false;
    is_loading_ = false;
    has_error_ = false;
    loaded_bytes_ = 0;
    waveform_.clear();
    waveform_.shrink_to_fit();
    bar_.deactivate();
}

void AudioViewerPage::load_bytes(const std::uint8_t* data, std::size_t size)
{
    if (!active_)
    {
        return;
    }
    is_loading_ = false;
    if (!data || size == 0)
    {
        has_error_ = true;
        host_.host_request_repaint();
        return;
    }
    if (!player_)
    {
        host_.host_request_repaint();
        return;
    }
    has_error_ = false;
    loaded_bytes_ = size;
    player_->set_playback_rate(rate_);
    player_->play(data, size, mime_type_);
    host_.host_playback_started();
    host_.host_request_repaint();
}

void AudioViewerPage::on_player_progress_()
{
    // Natural completion: the backend reports not-playing, so the transport bar
    // already shows the play icon; nothing to stop. Just repaint.
    host_.host_request_repaint();
}

void AudioViewerPage::on_player_error_()
{
    // The backend could not decode / play the clip: show the error message
    // in place of the waveform and drop the transport bar (controls_available_
    // is false while has_error_ is set).
    if (!active_)
    {
        return;
    }
    is_loading_ = false;
    has_error_ = true;
    host_.host_request_repaint();
}

std::uint64_t AudioViewerPage::duration_() const
{
    return (player_ && player_->duration_ms() > 0) ? player_->duration_ms() : duration_ms_;
}

MediaTransportBar::State AudioViewerPage::transport_state_() const
{
    MediaTransportBar::State s;
    s.position_ms = player_ ? player_->position_ms() : 0u;
    s.duration_ms = duration_();
    s.playing = player_ && player_->is_playing();
    s.rate = rate_;
    return s;
}

bool AudioViewerPage::controls_available_() const
{
    return active_ && player_ && !is_loading_ && !has_error_;
}

std::string AudioViewerPage::title_() const
{
    if (!filename_.empty())
    {
        return filename_;
    }
    if (!caption_.empty())
    {
        return caption_;
    }
    return tk::tr("Audio");
}

void AudioViewerPage::toggle_()
{
    if (!player_ || is_loading_ || has_error_)
    {
        return;
    }
    if (player_->is_playing())
    {
        player_->pause();
        return;
    }
    if (player_->reached_end())
    {
        player_->seek(0);
    }
    player_->resume();
    host_.host_playback_started();
}

void AudioViewerPage::cycle_speed_()
{
    if (!player_)
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
    player_->set_playback_rate(rate_);
}

void AudioViewerPage::seek_to_fraction_(float frac)
{
    const std::uint64_t dur = duration_();
    if (player_ && dur > 0)
    {
        player_->seek(static_cast<std::uint64_t>(frac * static_cast<float>(dur)));
    }
}

// ── layout ───────────────────────────────────────────────────────────────

void AudioViewerPage::arrange(tk::LayoutCtx& lc, tk::Rect /*b*/)
{
    const tk::Rect b = host_.host_bounds();
    if (b.w <= 0 || b.h <= 0)
    {
        return;
    }
    host_.host_refresh_chrome_autohide();

    const float card_w = std::min(kAudioCardMaxW, std::max(1.0f, b.w - 48.0f));
    const float card_h = std::min(kAudioCardMinH, std::max(1.0f, b.h - 48.0f));
    card_ = {b.x + (b.w - card_w) * 0.5f, b.y + (b.h - card_h) * 0.5f, card_w, card_h};

    const float inner_x = card_.x + kAudioCardPad;
    const float inner_w = card_.w - 2.0f * kAudioCardPad;
    bar_rect_ = {inner_x, card_.y + card_.h - kAudioCardPad - MediaTransportBar::kHeight,
                 inner_w, MediaTransportBar::kHeight};
    wave_ = {inner_x, bar_rect_.y - 12.0f - kAudioWaveH, inner_w, kAudioWaveH};

    bar_.arrange(lc, bar_rect_, controls_available_() && host_.host_chrome_shown());
}

bool AudioViewerPage::controls_hovered() const
{
    return bar_.hovered();
}

// ── paint ────────────────────────────────────────────────────────────────

void AudioViewerPage::paint_content(tk::PaintCtx& ctx)
{
    if (!active_)
    {
        return;
    }
    auto& cv = ctx.canvas;

    cv.fill_rounded_rect(card_, 12.0f, kAudioCardFill);
    cv.stroke_rounded_rect(card_, 12.0f, kAudioCardBorder, 1.0f);

    // Glyph
    const float glyph_y = card_.y + kAudioCardPad;
    glyph_.draw(cv, ctx.factory, kMusicSvg,
                {card_.x + (card_.w - kAudioGlyphPx) * 0.5f, glyph_y, kAudioGlyphPx,
                 kAudioGlyphPx},
                kAudioGlyphPx, kAudioTextSecondary);

    // Title
    float y = glyph_y + kAudioGlyphPx + 10.0f;
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::UiSemibold;
        st.max_width = card_.w - 2.0f * kAudioCardPad;
        auto lo = ctx.factory.build_text(title_(), st);
        if (lo)
        {
            const tk::Size sz = lo->measure();
            cv.draw_text(*lo, {card_.x + (card_.w - sz.w) * 0.5f, y}, kAudioTextPrimary);
            y += sz.h + 4.0f;
        }
    }

    // Clock: "pos / dur" once loaded, plain duration before.
    {
        const std::uint64_t dur = duration_();
        std::string clock;
        if (controls_available_())
        {
            clock = format_clock_ms(player_->position_ms()) + " / " + format_clock_ms(dur);
        }
        else if (dur > 0)
        {
            clock = format_clock_ms(dur);
        }
        if (!clock.empty())
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Timestamp;
            auto lo = ctx.factory.build_text(clock, st);
            if (lo)
            {
                const tk::Size sz = lo->measure();
                cv.draw_text(*lo, {card_.x + (card_.w - sz.w) * 0.5f, y},
                             kAudioTextSecondary);
            }
        }
    }

    // Waveform strip / loading / error
    if (is_loading_)
    {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - loading_start_)
                                    .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        tk::draw_spinner_dots(cv, {wave_.x + wave_.w * 0.5f, wave_.y + wave_.h * 0.5f},
                              phase, /*radius=*/14.0f, /*dot_r=*/3.0f,
                              tk::Color{220, 220, 220, 255});
        host_.host_request_repaint();
        return;
    }
    std::string msg_text;
    if (!player_)
    {
        msg_text = tk::tr("Audio playback is not available");
    }
    else if (has_error_)
    {
        msg_text = tk::tr("Unable to play audio");
    }
    if (!msg_text.empty())
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        st.max_width = wave_.w;
        auto lo = ctx.factory.build_text(msg_text, st);
        if (lo)
        {
            const tk::Size sz = lo->measure();
            cv.draw_text(*lo,
                         {wave_.x + (wave_.w - sz.w) * 0.5f,
                          wave_.y + (wave_.h - sz.h) * 0.5f},
                         kAudioTextSecondary);
        }
        return;
    }

    // Waveform bars (flat row of minimum-height bars when the sender gave none).
    const float step = kAudioWaveBarW + kAudioWaveBarGap;
    const int bars = std::max(1, static_cast<int>(wave_.w / step));
    std::uint16_t peak = 0;
    for (std::uint16_t v : waveform_)
    {
        peak = std::max(peak, v);
    }
    const float norm = peak > 0 ? 1.0f / static_cast<float>(peak) : 0.0f;
    const std::uint64_t dur = duration_();
    const float frac =
        (player_ && dur > 0)
            ? std::clamp(static_cast<float>(player_->position_ms()) / static_cast<float>(dur),
                         0.0f, 1.0f)
            : 0.0f;
    const int cursor_bar = static_cast<int>(frac * static_cast<float>(bars));
    const float mid_y = wave_.y + wave_.h * 0.5f;
    for (int i = 0; i < bars; ++i)
    {
        float h = kAudioWaveBarMinH;
        if (!waveform_.empty())
        {
            const std::size_t n = waveform_.size();
            const std::size_t src = std::min<std::size_t>(
                n - 1, static_cast<std::size_t>(static_cast<double>(i) / bars *
                                                static_cast<double>(n)));
            const float a = std::min(1.0f, static_cast<float>(waveform_[src]) * norm);
            h = std::max(kAudioWaveBarMinH, a * wave_.h);
        }
        cv.fill_rounded_rect({wave_.x + static_cast<float>(i) * step, mid_y - h * 0.5f,
                              kAudioWaveBarW, h},
                             kAudioWaveBarW * 0.5f,
                             i < cursor_bar ? kAudioBarPlayed : kAudioBarRest);
    }
}

void AudioViewerPage::paint_controls(tk::PaintCtx& ctx)
{
    if (controls_available_() && host_.host_chrome_shown())
    {
        bar_.paint(ctx, transport_state_());
    }
}

// ── input ────────────────────────────────────────────────────────────────

bool AudioViewerPage::on_content_pointer_down(tk::Point w, tk::Point /*local*/)
{
    if (controls_available_() && host_.host_chrome_shown() &&
        bar_.on_pointer_down(w, transport_state_()))
    {
        return true;
    }
    // Anywhere on the card is "inside the player": not an outside-tap dismiss.
    return rect_contains(card_, w);
}

bool AudioViewerPage::on_content_pointer_up(tk::Point, tk::Point, bool)
{
    return bar_.on_pointer_up();
}

void AudioViewerPage::on_pointer_drag(tk::Point local)
{
    if (bar_.scrubbing())
    {
        bar_.on_drag(local.x + host_.host_bounds().x, transport_state_());
    }
}

bool AudioViewerPage::on_key(const tk::KeyEvent& e)
{
    if (!controls_available_())
    {
        return false;
    }
    if (matches(ShortcutId::VideoPlayPause, e))
    {
        toggle_();
        return true;
    }
    if (matches(ShortcutId::VideoRestart, e))
    {
        player_->seek(0);
        return true;
    }
    return false;
}

} // namespace tesseract::views
