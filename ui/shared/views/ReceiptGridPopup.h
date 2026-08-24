#pragma once

// ReceiptGridPopup — small popup listing read receipts hidden behind a
// message row's "+N" overflow pill as a scrollable grid of avatar icons.
//
// Usage (mirrors DatePickerView/TabbedGridPicker's register_popup recipe):
//   1. set_image_provider() once, and set_entries() before each open.
//   2. open_at(world_rect) to position the popup and reset transient state.
//   3. During the owner's paint(), call
//      ctx.host->register_popup(popup.get()) to register it as the active
//      popup for this frame.
//   4. The host then calls paint_overlay() after the tree paint and routes
//      pointer/wheel events through the Widget::dispatch_pointer_*/wheel
//      methods (the grid child handles its own scrolling/hover/click).
//   5. Wire on_dismiss to react to an outside click or Escape.
//
// Cells are avatar icons only — no name text — matching the read-receipt
// disc cluster's own visual language. Hovering a cell shows a tooltip with
// that user's display name + "hh:mm" (format_hhmm), the same content the
// inline disc tooltip already shows.

#include "MessageListView.h"
#include "tk/list_view.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <vector>

namespace tesseract::views
{

class ReceiptGridPopup : public tk::Widget
{
protected:
    ReceiptGridPopup();
    TK_WIDGET_FACTORY_FRIEND(ReceiptGridPopup)

public:
    using ImageProvider = MessageListView::ImageProvider;

    void set_image_provider(ImageProvider p);
    void invalidate_image_cache();

    // Replace the entry list and recompute natural_size(). Call before
    // each open_at().
    void set_entries(std::vector<tesseract::ReadReceipt> entries);

    // The size this popup would like at its current entry count, capped at
    // kMaxVisibleRows tall (extra rows scroll). Feed into the caller's own
    // anchor-clamping helper (e.g. RoomView::clamp_picker_rect_).
    tk::Size natural_size() const
    {
        return natural_size_;
    }

    // Position the popup at the given world rect and reset transient
    // interaction state. Call once before making the popup visible each
    // time it opens — mirrors DatePickerView::open_at()/
    // TabbedGridPicker::open_at().
    void open_at(tk::Rect world_rect);

    std::function<void()> on_dismiss;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    // All visible drawing lives in paint() already; this just makes sure it
    // still runs (and that a deferred arrange() pass happens first) when
    // this widget is driven as a registered popup rather than a tree child
    // — see open_at()'s doc comment and Host::register_popup().
    void paint_overlay(tk::PaintCtx&) override;
    // Reached via Host's popup-first-refusal key dispatch while this popup
    // is the registered popup. Only Escape is handled here.
    bool on_key_down(const tk::KeyEvent&) override;
    // Host's outside-click dismiss path calls this directly on the
    // registered popup — mirrors DatePickerView::on_popup_dismiss.
    void on_popup_dismiss() override;

private:
    class GridAdapter;

    tk::GridView* grid_ = nullptr; // borrowed, add_child'd
    std::unique_ptr<GridAdapter> grid_adapter_;
    ImageProvider provider_;
    std::vector<tesseract::ReadReceipt> entries_;
    tk::Size natural_size_{};
    // Set by open_at(); consumed (and cleared) by the next paint_overlay(),
    // which is the first point a live CanvasFactory is available to arrange
    // with — same idiom as TabbedGridPicker::needs_arrange_.
    bool needs_arrange_ = true;

    // Matches MessageListView::kReceiptAvatarSize exactly — receipts should
    // look the same size here as they do in the timeline's disc cluster.
    static constexpr float kCellSize       = MessageListView::kReceiptAvatarSize;
    static constexpr float kCellGap        = 6.0f;
    static constexpr float kPadding        = 8.0f;
    static constexpr int   kCols           = 6;
    static constexpr int   kMaxVisibleRows = 4; // caps popup height; extra rows scroll
};

} // namespace tesseract::views
