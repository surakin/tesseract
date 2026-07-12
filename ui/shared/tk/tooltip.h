#pragma once

// A non-interactive floating label drawn above the entire widget tree.
// Owned exclusively by Host: widgets never construct or paint one directly,
// they request one via Host::show_tooltip()/hide_tooltip()/
// update_tooltip_text() (see host.h). Unlike a popup (ComboBox's dropdown,
// DatePickerView), a tooltip never receives input and needs no hit-test or
// dismiss protocol — it is pure paint.

#include "canvas.h"
#include "widget.h" // PaintCtx

#include <memory>
#include <string>

namespace tk
{

class Tooltip
{
public:
    // Sets the text to display and the world-space rect it is anchored to.
    // `anchor_world` is typically the hovered widget/region's own bounds.
    void set_content(std::string text, Rect anchor_world);

    // Draws the tooltip near its anchor, clamped to stay fully inside
    // `surface_bounds` (the whole paintable surface, not any one widget's
    // bounds — this is what lets the tooltip escape ancestor clipping).
    // No-op if no content has been set via set_content().
    void paint_overlay(PaintCtx& ctx, Rect surface_bounds);

private:
    std::string text_;
    Rect anchor_{};
    std::unique_ptr<TextLayout> layout_;
    // layout_ is rebuilt only when the text or the wrap width it was built
    // for changes (the latter tracks the host surface's width, so a window
    // resize re-wraps an already-visible tooltip).
    std::string layout_text_cache_;
    float layout_max_width_cache_ = -1.0f;
};

} // namespace tk
