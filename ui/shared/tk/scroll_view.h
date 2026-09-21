#pragma once

// ScrollView — generic vertical-scroll wrapper for a single arbitrary child
// widget whose natural height may exceed its viewport. For content that
// doesn't need its own bespoke ScrollableBase subclass the way ListView/
// GridView's virtualized item models do (e.g. a Label, a small hand-built
// panel) — just wrap it in a ScrollView instead of hand-rolling scroll_y_/
// wheel/scrollbar logic again. Reuses ScrollableBase's thumb/wheel/kinetic-
// scroll machinery; unlike GridView/ListView there is exactly one "item" —
// the child itself, measured at its natural (unconstrained) height and
// repositioned whenever scroll_y_ changes.

#include "scrollable_base.h"

#include <functional>
#include <memory>

namespace tk
{

class ScrollView : public ScrollableBase
{
protected:
    ScrollView() = default;
    TK_WIDGET_FACTORY_FRIEND(ScrollView)

public:
    // Takes ownership of `child` (construct it via create_widget<T>(this,
    // ...) first, exactly like Widget::add_child() elsewhere), replacing
    // any previous one. Returns the raw pointer for convenience.
    Widget* set_child(std::unique_ptr<Widget> child);
    Widget* child() const
    {
        return child_;
    }

    // Resets scroll position to the top — call when the child's content is
    // swapped out for something unrelated (mirrors ListView/GridView's own
    // scroll_to_top(), minimal version: no smooth-scroll, just a jump).
    void scroll_to_top()
    {
        scroll_y_ = 0.0f;
    }

    // Fired whenever a wheel or scrollbar drag changes scroll_y_, so the
    // owner can trigger a relayout — re-arranging the child at its new
    // scrolled position happens in arrange(), not paint(), since it's a
    // real child widget with its own bounds_ (mirrors RoomInfoPanelBody's
    // identical need/pattern for its own scrolled child widgets).
    std::function<void()> on_layout_changed;

    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void paint(PaintCtx&) override;
    bool on_wheel(Point local, float dx, float dy, bool is_touchpad = false) override;
    bool on_pointer_down(Point local) override;
    void on_pointer_drag(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;

protected:
    float content_height() const override
    {
        return content_h_;
    }

private:
    Widget* child_ = nullptr; // owned via Widget::children_ (add_child)
    float content_h_ = 0.0f;
};

} // namespace tk
