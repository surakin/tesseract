#pragma once

// Shared "row of room chips" painter: an optional caption followed by a
// horizontal run of avatar+name chips, each with its hit rect recorded for
// click handling. Extracted so QuickSwitcher's Ctrl+K "Recent" strip and
// MruSwitcher's Ctrl+Tab cycle card render identical chips from one
// implementation instead of two copies of the same loop.

#include "tk/canvas.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tesseract::views
{

using ChipAvatarProvider =
    std::function<const tk::Image*(const std::string& mxc_url)>;
using ChipAvatarNeeded = std::function<void(const tesseract::RoomInfo&)>;

struct RoomChipStripStyle
{
    float chip_w = 64.0f;
    float chip_gap = 8.0f;
    float avatar_size = 40.0f;
    float pad_x = 12.0f;
    float top_pad = 8.0f;

    // Drawn above the chip row when non-empty (QuickSwitcher's "Recent"
    // caption). Left empty for a caption-less strip (MruSwitcher's cycle
    // card).
    std::string caption;

    // Index into `rooms` to draw highlighted, or -1 for none.
    int highlight_index = -1;
    tk::Color highlight_fill{};

    // Also stroke the highlighted chip's border — used for a persistent
    // keyboard selection (MruSwitcher) as opposed to a transient
    // press-highlight fill alone (QuickSwitcher).
    bool highlight_border = false;
    tk::Color highlight_border_color{};
};

// Paints the chip row into `strip` (left-aligned, style.pad_x inset from
// strip.x), clipping to however many chips fit strip.w. Appends each drawn
// chip's world-space hit rect + room id to `out_chips` (not cleared first —
// callers clear once per paint() before calling, matching the existing
// recent_chips_ rebuild convention).
void paint_room_chips(tk::PaintCtx& ctx, tk::Rect strip,
                      const std::vector<tesseract::RoomInfo>& rooms,
                      const ChipAvatarProvider& avatar_provider,
                      const ChipAvatarNeeded& on_avatar_needed,
                      const RoomChipStripStyle& style,
                      std::vector<std::pair<tk::Rect, std::string>>& out_chips);

} // namespace tesseract::views
