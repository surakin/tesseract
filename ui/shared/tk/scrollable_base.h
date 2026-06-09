#pragma once

// ScrollableBase — shared vertical-scroll + scrollbar machinery for the tk
// list/grid family. ListView and GridView both render a virtualised column of
// content taller than their viewport and overlay a draggable scrollbar thumb on
// the right edge. The thumb geometry, hit-test, drag handling, scroll clamping,
// and thumb paint were byte-for-byte identical between the two; they live here.
//
// Subclasses provide only their content metric — `content_height()` — and keep
// their own item layout / paint / hit-test. The base owns `scroll_y_`, the
// drag state, and the scrollbar pointer handling. Subclasses that override the
// pointer handlers call the base's `scrollbar_*` helpers first so the thumb
// drag wins over any content hit underneath it (the pre-refactor order).

#include "widget.h"

namespace tk
{

class ScrollableBase : public Widget
{
public:
    // Current vertical scroll offset (0 = top).
    float scroll_y() const
    {
        return scroll_y_;
    }

protected:
    // Total height of the scrollable content. 0 when empty / not yet laid out.
    virtual float content_height() const = 0;

    // Clamp scroll_y_ into [0, max(0, content_height() - viewport)].
    void clamp_scroll();

    struct ThumbGeom
    {
        float track_top, track_h, thumb_h, thumb_top;
    };
    // Scrollbar thumb geometry in world coordinates. A zero-filled struct when
    // content fits the viewport (no thumb).
    ThumbGeom thumb_geom() const;

    // True when `local` (widget-local coords) is over the scrollbar thumb.
    // Subclasses test this first in on_pointer_down so the scrollbar wins over
    // any content hit test underneath it.
    bool thumb_hit(Point local) const;

    // Paint the scrollbar thumb overlay if content overflows the viewport.
    // Call after painting content (and popping any content clip).
    void paint_scrollbar(PaintCtx& ctx) const;

    // Scrollbar pointer handling. Subclasses route their pointer handlers
    // through these:
    //   - scrollbar_on_pointer_down: returns true (and begins a drag) when the
    //     press hit the thumb; the subclass then returns early.
    //   - scrollbar_on_pointer_drag: returns true while a thumb drag is active
    //     (and has moved scroll_y_); the subclass then returns early.
    //   - scrollbar_on_pointer_up: returns true when a thumb drag was active
    //     (and ends it); the subclass then returns early.
    bool scrollbar_on_pointer_down(Point local);
    bool scrollbar_on_pointer_drag(Point local);
    bool scrollbar_on_pointer_up();

    bool scrollbar_dragging() const
    {
        return scrollbar_drag_;
    }

    float scroll_y_ = 0;

    // Scrollbar drag state. When `scrollbar_drag_` is true the pointer-down hit
    // the thumb and `drag_anchor_y_` records the local y-offset within the thumb
    // that should stay under the cursor for the duration of the drag.
    bool scrollbar_drag_ = false;
    float drag_anchor_y_ = 0;
};

} // namespace tk
