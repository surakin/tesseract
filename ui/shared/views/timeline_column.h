#pragma once

// The content column the room timeline lays its rows out in: a percentage of
// the timeline's width (never narrower than kTimelineColumnMinW, never wider
// than the timeline), flush left or centred. Pure geometry so the paint,
// measure and hit-test paths all share one answer.

#include "tesseract/settings.h"

#include <algorithm>

namespace tesseract::views
{

inline constexpr float kTimelineColumnMinW = 468.0f;

struct TimelineColumn
{
    float x = 0.0f; // offset from the timeline's left edge
    float w = 0.0f;
};

inline TimelineColumn timeline_column(float timeline_w, int width_pct,
                                      Settings::TimelineColumnAnchor anchor)
{
    if (!(timeline_w > 0.0f))
    {
        return {0.0f, 0.0f};
    }
    const int pct =
        std::clamp(width_pct, Settings::kTimelineColumnMinPct, Settings::kTimelineColumnMaxPct);
    const float floor_w = std::min(kTimelineColumnMinW, timeline_w);
    const float w = std::clamp(timeline_w * static_cast<float>(pct) / 100.0f, floor_w, timeline_w);
    const float x =
        anchor == Settings::TimelineColumnAnchor::Center ? (timeline_w - w) * 0.5f : 0.0f;
    return {x, w};
}

} // namespace tesseract::views
