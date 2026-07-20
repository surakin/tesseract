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

#include "kinetic_scroller.h"
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

    // Applies a scroll delta and reports whether scroll_y_ actually moved
    // (false means a clamp bound was already hit). The default just does
    // the plain `scroll_y_ += dy; clamp_scroll();` every non-ListView
    // subclass already wants; ListView overrides this to also clear
    // stick_to_bottom_ and fire its near-top/near-bottom/on_scroll hooks,
    // so those keep firing during a kinetic fling too, not just on a
    // manual wheel event.
    virtual bool apply_scroll_delta(float dy)
    {
        float prev = scroll_y_;
        scroll_y_ += dy;
        clamp_scroll();
        return scroll_y_ != prev;
    }

    // Feed a wheel sample that should scroll content (call from on_wheel
    // once you've decided this event scrolls — e.g. not ListPopupBase's
    // fits-in-viewport selection-cycle branch) and apply its delta via
    // apply_scroll_delta(). Also feeds tk::KineticScroller so a trackpad
    // release coasts to a stop; keeps the paint loop alive while it does.
    // Defined in the .cpp (needs the full Host definition for
    // request_repaint(), and widget.h only forward-declares Host).
    bool on_wheel_scroll(float dy, bool is_touchpad);

    // Call once per paint() to advance an in-flight fling (or the idle
    // watch that precedes one). Mirrors the self-driven idiom
    // tk::FloatTween-based transitions already use elsewhere in tk/views
    // (step, then keep requesting repaints while still active).
    void step_kinetic();

    KineticScroller kinetic_;

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
