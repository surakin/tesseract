#pragma once
#include "tk/animator.h"
#include "tk/widget.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tesseract::views
{

// Generic single-column popup menu. Owned by (add_child()'d onto) whatever
// widget triggers it — anywhere in the tree, no matter how narrow or deep —
// and always renders as a true top-level overlay, on top of literally
// everything: paint() only registers it as the host's active popup each
// frame it's open (Host::register_popup(), also giving it click-anywhere-
// dismiss input priority); the actual drawing happens in paint_overlay(),
// the second paint pass the host runs after the whole tree's ordinary
// paint() has finished. arrange() ignores the rect its owner passes in and
// always positions/clamps against root_bounds() (the true window), for the
// same reason — a widget's own local bounds are irrelevant to an overlay
// that can render anywhere.
//
// Anchor rect passed to open() must be in WORLD coordinates.
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
    static constexpr float kSeparatorHeight = 9.0f; // thin rule + margin
    static constexpr float kGlyphX   = 10.0f;  // icon left margin
    static constexpr float kTextX    = 34.0f;  // label left margin (with icon)
    static constexpr float kTextXNoIcon = 12.0f; // label left margin (no icon)

    struct Item
    {
        std::string glyph;        // Unicode icon character(s); empty = no icon
        // Optional SVG icon (embedded k*Svg byte span). When non-empty it is
        // rasterized + tinted and drawn instead of `glyph`. Points at static
        // data, so the span stays valid for the menu's lifetime.
        std::span<const std::uint8_t> svg_icon{};
        std::string label;
        bool destructive = false; // draws label in pal.destructive colour
        std::function<void()> on_selected;
        // A thin rule instead of a label/icon; not hoverable or clickable.
        // Ignores every other field (glyph/svg_icon/label/destructive/
        // enabled/on_selected). Placed after on_selected (rather than next to
        // destructive) so existing 5-positional-arg aggregate-init call sites
        // (glyph, svg_icon, label, destructive, on_selected) keep compiling.
        bool is_separator = false;
        // false: label/icon dimmed, no hover highlight, on_selected doesn't
        // fire, and clicking it doesn't dismiss the menu (matches native
        // disabled-menu-item behavior).
        bool enabled = true;
    };

    // Show the menu anchored to `anchor` in WORLD coordinates. The menu opens
    // below the anchor, right-aligned to its right edge; flips above if the
    // menu would clip the parent's bottom.
    void open(std::vector<Item> items, tk::Rect anchor_world);
    void close();
    bool is_open() const { return open_; }

    // Test-only: the items passed to the most recent open() call.
    const std::vector<Item>& items_for_test() const { return items_; }

    // Fires when an item is selected or the backdrop is clicked.
    std::function<void()> on_dismissed;

    // Fires when the open/closed state changes. Wire the parent's repaint /
    // relayout trigger here so the popup appears immediately.
    std::function<void()> on_layout_changed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size) override;
    // bounds param is ignored — see the class doc comment above.
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    void     paint_overlay(tk::PaintCtx&) override;
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
    // paint_overlay() adds bounds_.origin to convert back to world for drawing.
    tk::Rect menu_rect_{}; // the visible card, in LOCAL coords

    int  hovered_index_  = -1;
    int  pressed_index_  = -1;
    bool press_backdrop_ = false;

    // Per-item rasterized SVG icons (Item::svg_icon), tinted to the row colour.
    // Rebuilt when items change (open) or the canvas DPI scale changes.
    std::vector<std::unique_ptr<tk::Image>> icon_cache_;

    float icon_scale_ = -1.0f;
    // Opacity entrance reveal, restarted each time open() is called.
    tk::FloatTween reveal_{1.0f};

    // Height of row i — kSeparatorHeight for a separator item, kRowHeight
    // otherwise. Rows no longer have uniform height once a menu can contain
    // separators, so total menu height and each row's rect are both summed
    // from this rather than a flat multiply.
    float row_height(int i) const;

    // Item rect in LOCAL coords (recomputed in arrange, used by paint + row_at).
    tk::Rect item_rect(int i) const;

    int row_at(tk::Point local) const;
};

} // namespace tesseract::views
