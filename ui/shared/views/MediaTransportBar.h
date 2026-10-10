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
// children, created through the page host, and the scrub-bar geometry, painting
// and drag maths. The page owns the player and feeds the bar a State snapshot
// each time it paints or handles a scrub; the bar reports user intent back
// through on_toggle / on_cycle_speed / on_seek.
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
    // Hide the buttons and drop any scrub drag (page deactivated).
    void deactivate();
    void paint(tk::PaintCtx& ctx, const State& s);

    // Press that no button claimed: starts a scrub when it lands on the track.
    bool on_pointer_down(tk::Point world, const State& s);
    // Ends a scrub; true when one was active.
    bool on_pointer_up();
    bool scrubbing() const
    {
        return scrubbing_;
    }
    void on_drag(float world_x, const State& s);

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

private:
    void seek_from_x_(float world_x, const State& s);

    tk::Button* play_btn_ = nullptr;
    tk::Button* speed_btn_ = nullptr;
    tk::Rect bar_{};
    tk::Rect scrub_{};
    bool scrubbing_ = false;
};

} // namespace tesseract::views
