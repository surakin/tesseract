#include "MediaTransportBar.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/i18n.h"

#include <algorithm>
#include <cstdio>

namespace tesseract::views
{

namespace
{
constexpr float kTbPadX = 10.0f;
constexpr float kTbPlayBtnD = 36.0f;
constexpr float kTbSpeedPillW = 32.0f;
constexpr float kTbSpeedPillH = 20.0f;
constexpr float kTbScrubH = 6.0f;
constexpr float kTbScrubR = 3.0f;

// Fixed, backdrop-tuned fill states for the play/speed buttons — the
// controls bar is always near-black regardless of the app's light/dark
// theme, so these bypass tk::Button's default theme-driven fill (see
// Button::FillOverride).
constexpr tk::Color kTbFillRest = tk::Color::rgba(255, 255, 255, 25);
constexpr tk::Color kTbFillHover = tk::Color::rgba(255, 255, 255, 75);
constexpr tk::Color kTbFillPressed = tk::Color::rgba(255, 255, 255, 110);

// Tint used for the controls-bar glyphs/text (play icon, rate label, scrub
// playhead) — always this fixed near-white regardless of theme.
constexpr tk::Color kTbGlyphColor = tk::Color::rgba(255, 255, 255, 230);

// Scrub-bar "downloaded so far" band — a distinct accent so it reads at a
// glance as a different kind of progress from the playback position.
constexpr tk::Color kTbBufferedFill = tk::Color::rgba(110, 190, 255, 140);
} // namespace

std::string format_clock_ms(std::uint64_t ms)
{
    if (ms == 0)
    {
        return "0:00";
    }
    const std::uint64_t s = ms / 1000;
    const std::uint64_t mm = s / 60;
    const std::uint64_t ss = s % 60;
    char buf[48]; // worst case: two 20-digit uint64 fields + ':' + NUL
    std::snprintf(buf, sizeof(buf), "%llu:%02llu", static_cast<unsigned long long>(mm),
                  static_cast<unsigned long long>(ss));
    return buf;
}

MediaTransportBar::MediaTransportBar(MediaViewerPageHost& host, bool with_speed)
{
    play_btn_ = host.host_add_icon_button();
    play_btn_->set_on_click(
        [this]
        {
            if (on_toggle)
            {
                on_toggle();
            }
        });
    play_btn_->set_fill_override(
        tk::Button::FillOverride{kTbFillRest, kTbFillHover, kTbFillPressed});
    play_btn_->set_icon(kPlaySvg, kTbPlayBtnD * 0.5f);
    play_btn_->set_icon_color_override(kTbGlyphColor);
    play_btn_->set_visible(false);

    if (with_speed)
    {
        speed_btn_ = host.host_add_icon_button();
        speed_btn_->set_on_click(
            [this]
            {
                if (on_cycle_speed)
                {
                    on_cycle_speed();
                }
            });
        speed_btn_->set_fill_override(
            tk::Button::FillOverride{kTbFillRest, kTbFillHover, kTbFillPressed});
        speed_btn_->set_visible(false);
    }
}

void MediaTransportBar::arrange(tk::LayoutCtx& lc, tk::Rect bar, bool shown)
{
    bar_ = bar;
    const tk::Rect play_rect{bar.x + kTbPadX, bar.y + (kHeight - kTbPlayBtnD) * 0.5f,
                             kTbPlayBtnD, kTbPlayBtnD};
    play_btn_->arrange(lc, play_rect);

    float scrub_xe = bar.x + bar.w - kTbPadX;
    if (speed_btn_)
    {
        const tk::Rect speed_rect{bar.x + bar.w - kTbPadX - kTbSpeedPillW,
                                  bar.y + (kHeight - kTbSpeedPillH) * 0.5f,
                                  kTbSpeedPillW, kTbSpeedPillH};
        speed_btn_->arrange(lc, speed_rect);
        scrub_xe = speed_rect.x - kTbPadX;
    }

    const float scrub_x = play_rect.x + kTbPlayBtnD + kTbPadX;
    const float scrub_w = std::max(0.0f, scrub_xe - scrub_x);
    scrub_ = {scrub_x, bar.y + (kHeight - kTbScrubH) * 0.5f, scrub_w, kTbScrubH};

    play_btn_->set_visible(shown);
    if (speed_btn_)
    {
        speed_btn_->set_visible(shown);
    }
}

void MediaTransportBar::deactivate()
{
    scrubbing_ = false;
    play_btn_->set_visible(false);
    if (speed_btn_)
    {
        speed_btn_->set_visible(false);
    }
}

bool MediaTransportBar::hovered() const
{
    return (play_btn_ && play_btn_->hovered()) || (speed_btn_ && speed_btn_->hovered());
}

bool MediaTransportBar::contains(tk::Point world) const
{
    return rect_contains(bar_, world);
}

void MediaTransportBar::paint(tk::PaintCtx& ctx, const State& s)
{
    auto& cv = ctx.canvas;

    cv.fill_rounded_rect(bar_, 8.0f, tk::Color::rgba(0, 0, 0, 150));

    const tk::Color& glyph_col = kTbGlyphColor;

    // Refreshed every frame from the same `playing` read that picks the glyph
    // below, so the accessible name can never drift from what's shown.
    play_btn_->set_accessible_name(s.playing ? tk::tr("Pause") : tk::tr("Play"));

    // Button's own fill (via FillOverride) draws the pill; the clip only shapes
    // it into a circle. The play glyph is Button's own icon; while playing it
    // is hidden (empty span) and the pause bars are drawn here instead.
    const tk::Rect play_bounds = play_btn_->bounds();
    play_btn_->set_icon(s.playing ? std::span<const std::uint8_t>{} : kPlaySvg,
                        kTbPlayBtnD * 0.5f);
    cv.push_clip_rounded_rect(play_bounds, kTbPlayBtnD * 0.5f);
    play_btn_->paint(ctx);
    cv.pop_clip();
    if (s.playing)
    {
        const float bar_w = 3.0f;
        const float bar_h = kTbPlayBtnD * 0.45f;
        const float gap = 4.0f;
        const float cy = play_bounds.y + (kTbPlayBtnD - bar_h) * 0.5f;
        const float cx = play_bounds.x + kTbPlayBtnD * 0.5f;
        cv.fill_rect({cx - gap * 0.5f - bar_w, cy, bar_w, bar_h}, glyph_col);
        cv.fill_rect({cx + gap * 0.5f, cy, bar_w, bar_h}, glyph_col);
    }

    if (speed_btn_)
    {
        const tk::Rect speed_bounds = speed_btn_->bounds();
        cv.push_clip_rounded_rect(speed_bounds, kTbSpeedPillH * 0.5f);
        speed_btn_->paint(ctx);
        cv.pop_clip();

        char rate_buf[8];
        const char* rate_plain; // plain "x" for the accessible name, not the
                                // "×" glyph, avoiding pronunciation ambiguity.
        if (s.rate >= 1.99f)
        {
            std::snprintf(rate_buf, sizeof(rate_buf), "2\xC3\x97");
            rate_plain = "2x";
        }
        else if (s.rate >= 1.49f)
        {
            std::snprintf(rate_buf, sizeof(rate_buf), "1.5\xC3\x97");
            rate_plain = "1.5x";
        }
        else
        {
            std::snprintf(rate_buf, sizeof(rate_buf), "1\xC3\x97");
            rate_plain = "1x";
        }
        speed_btn_->set_accessible_name(tk::trf(tk::tr("Playback speed: {0}"), {rate_plain}));
        tk::TextStyle rs{};
        rs.role = tk::FontRole::Timestamp;
        auto rate_lo = ctx.factory.build_text(rate_buf, rs);
        if (rate_lo)
        {
            tk::Size sz = rate_lo->measure();
            cv.draw_text(*rate_lo,
                         {speed_bounds.x + (speed_bounds.w - sz.w) * 0.5f,
                          speed_bounds.y + (speed_bounds.h - sz.h) * 0.5f},
                         glyph_col);
        }
    }

    // Scrub bar — track + downloaded band + filled region + playhead
    if (scrub_.w > 0)
    {
        const float frac = (s.duration_ms > 0)
                               ? std::clamp(static_cast<float>(s.position_ms) /
                                                static_cast<float>(s.duration_ms),
                                            0.0f, 1.0f)
                               : 0.0f;

        cv.fill_rounded_rect(scrub_, kTbScrubR, tk::Color::rgba(255, 255, 255, 60));

        if (s.buffered_frac > 0.0f)
        {
            const float buffered_w = scrub_.w * std::min(1.0f, s.buffered_frac);
            cv.fill_rounded_rect({scrub_.x, scrub_.y, buffered_w, scrub_.h}, kTbScrubR,
                                 kTbBufferedFill);
        }

        // Playback position, drawn over the downloaded band.
        if (frac > 0.0f)
        {
            cv.fill_rounded_rect({scrub_.x, scrub_.y, scrub_.w * frac, scrub_.h},
                                 kTbScrubR, tk::Color::rgba(255, 255, 255, 200));
        }

        constexpr float kKnobD = 12.0f;
        const float kx = scrub_.x + scrub_.w * frac - kKnobD * 0.5f;
        const float ky = scrub_.y + (scrub_.h - kKnobD) * 0.5f;
        cv.fill_rounded_rect({kx, ky, kKnobD, kKnobD}, kKnobD * 0.5f, glyph_col);
    }
}

bool MediaTransportBar::on_pointer_down(tk::Point world, const State& s)
{
    if (!rect_contains(scrub_, world))
    {
        return false;
    }
    scrubbing_ = true;
    // Immediate seek on press for snappy scrub-start; on_drag continues.
    seek_from_x_(world.x, s);
    return true;
}

bool MediaTransportBar::on_pointer_up()
{
    if (scrubbing_)
    {
        scrubbing_ = false;
        return true;
    }
    return false;
}

void MediaTransportBar::on_drag(float world_x, const State& s)
{
    if (scrubbing_)
    {
        seek_from_x_(world_x, s);
    }
}

void MediaTransportBar::seek_from_x_(float world_x, const State& s)
{
    if (scrub_.w <= 0 || !on_seek)
    {
        return;
    }
    float frac = std::clamp((world_x - scrub_.x) / scrub_.w, 0.0f, 1.0f);
    // Never seek ahead of what's actually downloaded — that byte range doesn't
    // exist yet, so the player would just block waiting on data that hasn't
    // arrived. Only meaningful while streaming and not yet fully downloaded.
    if (s.buffered_frac >= 0.0f)
    {
        frac = std::min(frac, std::clamp(s.buffered_frac, 0.0f, 1.0f));
    }
    on_seek(frac);
}

} // namespace tesseract::views
