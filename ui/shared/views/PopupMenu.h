#pragma once
#include "tk/widget.h"
#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

// Generic single-column popup menu. Intended to be owned by a parent widget
// (e.g. RoomView) and arranged at the parent's full bounds so the backdrop
// can intercept clicks anywhere outside the menu card and dismiss it.
//
// Anchor rect passed to open() must be in WORLD coordinates (the same space
// that arrange() receives its bounds in). PopupMenu converts internally.
//
// Usage:
//   popup->open(items, anchor_world_rect);
//   // → on_layout_changed fires; parent re-arranges / repaints
//   // User clicks an item → item.on_selected(); on_dismissed();
//   // User clicks backdrop → on_dismissed();
//   popup->on_dismissed = [this]{ popup->close(); };
class PopupMenu : public tk::Widget
{
public:
    static constexpr float kWidth     = 180.0f;
    static constexpr float kRowHeight = 34.0f;
    static constexpr float kGlyphX   = 10.0f;  // icon left margin
    static constexpr float kTextX    = 34.0f;  // label left margin (with icon)
    static constexpr float kTextXNoIcon = 12.0f; // label left margin (no icon)

    struct Item
    {
        std::string glyph;        // Unicode icon character(s); empty = no icon
        std::string label;
        bool destructive = false; // draws label in pal.destructive colour
        std::function<void()> on_selected;
    };

    // Show the menu anchored to `anchor` in WORLD coordinates. The menu opens
    // below the anchor, right-aligned to its right edge; flips above if the
    // menu would clip the parent's bottom.
    void open(std::vector<Item> items, tk::Rect anchor_world);
    void close();
    bool is_open() const { return open_; }

    // Fires when an item is selected or the backdrop is clicked.
    std::function<void()> on_dismissed;

    // Fires when the open/closed state changes. Wire the parent's repaint /
    // relayout trigger here so the popup appears immediately.
    std::function<void()> on_layout_changed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;

private:
    bool              open_   = false;
    std::vector<Item> items_;
    tk::Rect          anchor_world_{}; // anchor in world coords, set by open()

    // Computed by arrange(); stored in LOCAL coords (relative to bounds_.origin)
    // so pointer handlers (which receive local coords) can compare directly.
    // paint() adds bounds_.origin to convert back to world for drawing.
    tk::Rect menu_rect_{}; // the visible card, in LOCAL coords

    int  hovered_index_  = -1;
    int  pressed_index_  = -1;
    bool press_backdrop_ = false;

    // Item rect in LOCAL coords (recomputed in arrange, used by paint + row_at).
    tk::Rect item_rect(int i) const
    {
        return {menu_rect_.x, menu_rect_.y + float(i) * kRowHeight,
                menu_rect_.w, kRowHeight};
    }

    int row_at(tk::Point local) const;
};

} // namespace tesseract::views
