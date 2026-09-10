#pragma once

#include <algorithm>

#include "tesseract/visual.h"

// Pure geometry helpers for the resizable / collapsible room-list sidebar.
// Kept free of any widget / canvas dependency so the drag math is unit-tested
// in isolation (see tests/cpp/test_resizable_sidebar.cpp). MainAppWidget's
// RootLayoutWidget is the only production caller.
namespace tesseract::views
{

// Largest width the sidebar may be dragged to:
//   min( max(window_w * 0.5, kSidebarWidth) , longest_row_w )
// - The window-fraction term is floored at the default sidebar width so a
//   moderately narrow window never forces the sidebar below its default.
// - longest_row_w is the width needed to show the widest room row (name /
//   last-message preview) in the currently expanded sections without
//   truncation. A value <= 0 means "no rows to measure" and disables that
//   term (the window fraction alone applies).
inline float sidebar_max_width(float window_w, float longest_row_w)
{
    const float floor_w = static_cast<float>(tesseract::visual::kSidebarWidth);
    const float window_term = std::max(window_w * 0.5f, floor_w);
    if (longest_row_w <= 0.0f)
        return window_term;
    return std::min(window_term, longest_row_w);
}

struct SidebarDragResult
{
    float width;      // expanded width to apply (meaningful only when !collapsed)
    bool  collapsed;  // true → icon-only mode
};

// Resolve a raw dragged width (drag-start width + pointer delta) to a snapped,
// clamped result. A drag that ends below the midpoint between the collapsed
// width and the minimum expanded width snaps to collapsed; otherwise the width
// is clamped to [kSidebarMinExpandedWidth, max_w].
inline SidebarDragResult resolve_sidebar_drag(float raw_w, float max_w)
{
    const float collapsed_w = static_cast<float>(tesseract::visual::kSidebarCollapsedWidth);
    const float min_expanded = static_cast<float>(tesseract::visual::kSidebarMinExpandedWidth);
    const float snap_threshold = (collapsed_w + min_expanded) * 0.5f;

    if (raw_w < snap_threshold)
        return {min_expanded, true};

    const float hi = std::max(min_expanded, max_w);
    return {std::clamp(raw_w, min_expanded, hi), false};
}

} // namespace tesseract::views
