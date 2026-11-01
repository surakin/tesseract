#pragma once

// RowKeyboardCursor — keyboard operation of a custom-painted widget through
// the rows it already exposes to assistive technology
// (tk::WidgetRowAccessibility). Those rows carry exactly what a keyboard
// user needs: a role, a name, a world rect and an activation that runs the
// same callback as the mouse path. Embedding one of these gives the widget
// arrow-key movement between its interactive rows, Enter/Space to activate,
// a focus ring around the current row and a tooltip naming it — without a
// second, keyboard-only description of the same controls.
//
// The owner makes itself focusable() while has_stops() (and usually
// focus_on_click() == false, so clicks keep their existing behaviour), then
// forwards on_key_down / paint_own_focus_ring / on_focus_lost here.

#include "access_tree.h"
#include "widget.h"

#include <cstddef>

namespace tk
{

class RowKeyboardCursor
{
public:
    // Whether row `i` is somewhere a keyboard user can land: an interactive
    // role, not disabled, with an on-screen rect.
    static bool row_is_stop(const WidgetRowAccessibility& rows, std::size_t i);
    // Whether any row is currently a stop (refreshes the owner's rows).
    static bool has_stops(const WidgetRowAccessibility& rows);

    // Left/Up = previous stop, Right/Down = next, Home/End = first/last,
    // Enter/Space = activate the current stop (the first press with no
    // current stop just lands on the first one). Returns true when consumed.
    // A ScrollableBase owner is scrolled to keep the current row visible.
    bool handle_key(Widget& owner, WidgetRowAccessibility& rows, const KeyEvent& e);

    // Ring around the current row, or around the owner when there is none.
    void paint_ring(PaintCtx& ctx, const Widget& owner, const WidgetRowAccessibility& rows) const;

    void reset()
    {
        index_ = -1;
    }
    int index() const
    {
        return index_;
    }

private:
    void land_(Widget& owner, const WidgetRowAccessibility& rows, int index);

    int index_ = -1;
};

} // namespace tk
