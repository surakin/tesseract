#pragma once

#include "canvas.h" // Point

namespace tk
{

// Pixel distance a pointer must travel from its down-point before a press
// is reinterpreted as a drag, shared by every widget that starts an in-app
// drag (Host::begin_drag(), drag_drop.h) from its own on_pointer_down/
// on_pointer_drag. A different gesture — resize/scroll/select-drag, e.g.
// ScrollableBase's scrollbar-thumb tracking or MessageListView's
// text-selection drag — has its own hand-rolled state and does not use this.
inline constexpr float kDragThresholdPx = 4.0f;

// Minimal click-vs-drag helper so a new begin_drag() call site doesn't have
// to hand-roll distance math.
class DragGestureTracker
{
public:
    void begin(Point down_local)
    {
        down_ = down_local;
        armed_ = true;
    }

    void cancel()
    {
        armed_ = false;
    }

    // Call from on_pointer_drag(). Returns true exactly once, the first time
    // the threshold is crossed after begin(); false before that and after
    // (call begin() again to rearm).
    bool crossed_threshold(Point local)
    {
        if (!armed_)
            return false;
        float dx = local.x - down_.x;
        float dy = local.y - down_.y;
        if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx)
            return false;
        armed_ = false;
        return true;
    }

private:
    Point down_{};
    bool armed_ = false;
};

} // namespace tk
