#pragma once

#include "MediaViewerPage.h"

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <string>

namespace tesseract::views
{

// "m:ss" for a millisecond count ("0:00" for zero).
std::string format_clock_ms(std::uint64_t ms);

// The play / scrub / speed strip shared by the video and audio viewer pages.
// A non-widget helper: it owns the play (and optional speed) tk::Button
// children and the scrub tk::Slider, all created through the page host. The
// page owns the player and feeds the bar a State snapshot each time it paints;
// the bar reports user intent back through on_toggle / on_cycle_speed / on_seek.
class MediaTransportBar
{
public:
    static constexpr float kHeight = 56.0f;

    struct State
    {
        std::uint64_t position_ms = 0;
        std::uint64_t duration_ms = 0;
        bool playing = false;
        // Fraction of the file downloaded so far (0..1), or negative when the
        // whole file is available (no downloaded band, no seek limit).
        float buffered_frac = -1.0f;
        float rate = 1.0f;
    };

    MediaTransportBar(MediaViewerPageHost& host, bool with_speed);

    std::function<void()> on_toggle;
    std::function<void()> on_cycle_speed;
    // Scrub target as a 0..1 fraction of the duration, already clamped to the
    // downloaded part of the file.
    std::function<void(float)> on_seek;

    // Place the bar (background strip `bar`) and show/hide its buttons.
    void arrange(tk::LayoutCtx& lc, tk::Rect bar, bool shown);
    // Hide the buttons and the scrub slider (page deactivated).
    void deactivate();
    void paint(tk::PaintCtx& ctx, const State& s);

    bool scrubbing() const
    {
        return scrub_ && scrub_->dragging();
    }

    bool contains(tk::Point world) const;
    bool hovered() const;

    tk::Button* play_btn() const
    {
        return play_btn_;
    }
    tk::Button* speed_btn() const
    {
        return speed_btn_;
    }
    tk::Slider* scrub_slider() const
    {
        return scrub_;
    }

private:
    tk::Button* play_btn_ = nullptr;
    tk::Button* speed_btn_ = nullptr;
    tk::Slider* scrub_ = nullptr;
    tk::Rect bar_{};
};

} // namespace tesseract::views
