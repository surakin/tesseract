#pragma once
#include "tk/host.h"
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
// widget triggers it. Renders inside a real native popup surface
// (tk::PopupSurfaceHandle — the same primitive tk::ComboBox's dropdown uses),
// not canvas-drawn content, so it reliably paints above native controls
// (NativeTextField/NativeTextArea) the way canvas-only overlays never can —
// a real OS popup window z-orders like any other window. PopupMenu itself
// stays mounted in its owner's tree only to hold state and register itself
// as the host's active popup (Host::register_popup(), giving it
// click-anywhere-dismiss input priority via the owner's on_popup_dismiss());
// it is never itself painted or hit-tested — all drawing/row hit-testing
// happens in the popup surface's own separate widget tree.
//
// Anchor rect passed to open() must be in WORLD coordinates (this widget's
// own local coordinate space, since anchors are always computed by the
// owner from its own bounds_ + a click/button rect).
//
// Usage:
//   popup->open(items, anchor_world_rect);
//   // → on_layout_changed fires; parent re-arranges / repaints
//   // User clicks an item → item.on_selected(); on_dismissed();
//   // User clicks elsewhere in the app → owner's on_popup_dismiss() fires;
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
    // menu would clip the screen's bottom.
    void open(std::vector<Item> items, tk::Rect anchor_world);
    void close();
    bool is_open() const { return open_; }

    // Test-only: the items passed to the most recent open() call.
    const std::vector<Item>& items_for_test() const { return items_; }

    // Fires when an item is selected or the menu is otherwise dismissed via
    // its own row interaction. Owner is expected to call close() here.
    std::function<void()> on_dismissed;

    // Fires when the open/closed state changes. Wire the parent's repaint /
    // relayout trigger here so the popup appears immediately.
    std::function<void()> on_layout_changed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size) override;
    // Repositions the popup surface (if open) each layout pass. The bounds
    // param is unused — a popup surface positions itself against the anchor
    // + the whole screen, not this widget's own local bounds.
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    // Only registers this widget as the host's active popup while open, for
    // click-anywhere-dismiss — never draws (drawing happens inside the
    // native popup surface's own separate paint pass).
    void     paint(tk::PaintCtx&) override;
    void     on_theme_changed(const tk::Theme&) override;
    // Fired by Host::dispatch_pointer_down when a click lands outside this
    // registered popup (see register_popup()'s doc comment) — the
    // click-anywhere-dismiss path now that there's no backdrop of our own to
    // detect it locally. Fires on_dismissed(), matching the existing
    // contract owners already wire to call close().
    void     on_popup_dismiss() override;

private:
    class MenuList; // popup surface's root widget — defined in the .cpp

    void reposition_(); // computes size + positions popup_surface_

    bool              open_   = false;
    std::vector<Item> items_;
    tk::Rect          anchor_world_{}; // anchor in local/world coords, set by open()

    std::unique_ptr<tk::PopupSurfaceHandle> popup_surface_;
    MenuList* list_ = nullptr; // owned by popup_surface_ via set_root()

    // Height of row i — kSeparatorHeight for a separator item, kRowHeight
    // otherwise. Rows no longer have uniform height once a menu can contain
    // separators, so total menu height is summed from this rather than a
    // flat multiply.
    float row_height(int i) const;
};

} // namespace tesseract::views
