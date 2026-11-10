#pragma once
#include "tk/widget.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>

namespace tesseract::views
{

/// Horizontal strip of image cells shown above the composer (GIF and sticker
/// search). Owns everything that is independent of the item type: cell layout,
/// selection, horizontal scroll, pointer hover/click, wheel stepping, the card
/// chrome and the one-line status mode. Subclasses store the items and supply
/// the per-item image drawing (`paint_item`), accept action (`accept_item`) and
/// an optional footer row. Keyboard nav is driven by the owning controller.
class ImageStripPopup : public tk::Widget
{
public:
    static constexpr float kGap = 4.0f;
    static constexpr float kPad = 6.0f;
    static constexpr float kCardRadius =
        tesseract::visual::kRadiusMD; // matches the compose attachment band
    // Status-row metrics (single-line message mode).
    static constexpr float kStatusH = 22.0f;
    static constexpr float kStatusMinW = 160.0f;

    /// Show a one-line status message instead of items. Subclasses override to
    /// also drop their stored items, then call this.
    void set_status(std::string message);
    bool has_status() const
    {
        return !status_.empty() && total_ == 0;
    }
    void set_selected_index(int index);
    int selected_index() const
    {
        return selected_index_;
    }
    int visible_count() const
    {
        return std::min(static_cast<int>(total_), max_cells_);
    }

    /// Shift the selection by `delta` (clamped to [0, visible-1]). Returns true
    /// when there is a list to move within.
    bool move_selection(int delta);

    /// Logical size the popup wants given the host's available width. Returns
    /// {0,0} when there is nothing to show (no items and no status). The strip
    /// width is clamped to `max_width` (the overflow is reachable by scrolling
    /// the selection); a status row gets a compact width clamped the same way.
    tk::Size content_size(float max_width) const;

    std::function<void()> on_dismissed;

    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(tk::PaintCtx& ctx) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

protected:
    ImageStripPopup(float cell_w, float cell_h, float footer_h, int max_cells)
        : cell_w_(cell_w), cell_h_(cell_h), footer_h_(footer_h), max_cells_(max_cells)
    {
    }

    /// Called by subclasses after replacing their item list: resets selection
    /// (first item preselected), hover, press and scroll.
    void reset_items_(std::size_t total);

    /// Draw item `i`'s image inside `cell` (and note animation damage).
    virtual void paint_item(tk::PaintCtx& ctx, int i, tk::Rect cell) = 0;
    /// Pointer-activated item `i`.
    virtual void accept_item(int i) = 0;
    /// Draw the footer row (only called when footer_h > 0 and items are shown).
    virtual void paint_footer(tk::PaintCtx&)
    {
    }

    /// Fit `img` inside `cell`, centred, preserving aspect ratio.
    static void draw_image_fit(tk::PaintCtx& ctx, const tk::Image& img, tk::Rect cell);

private:
    int cell_at(tk::Point local) const;
    tk::Rect cell_rect(int i) const;
    float content_width_() const;
    void ensure_selected_visible_();

    float cell_w_;
    float cell_h_;
    float footer_h_;
    int max_cells_;
    std::size_t total_ = 0;
    std::string status_;
    int selected_index_ = -1;
    int hovered_index_ = -1;
    int pressed_index_ = -1;
    float scroll_x_ = 0.0f;    // horizontal scroll offset of the cell row
    float wheel_carry_ = 0.0f; // sub-notch accumulator for smooth-scroll devices
};

} // namespace tesseract::views
