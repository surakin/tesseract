#pragma once
#include "tk/scrollable_base.h"
#include <algorithm>

namespace tesseract::views
{

// Shared scaffolding for the composer's single-column list popups
// (MentionPopup / ShortcodePopup / SlashCommandPopup). These three widgets
// differ only in their item data model and in how a single row is drawn;
// everything else — measure/arrange, hit-testing, pointer + wheel handling,
// the selected/hovered row fill, per-row separators, scrolling, and the 1px
// border — is identical and lives here.
//
// Subclasses provide:
//   - row_count()            : how many rows the model currently holds
//   - paint_row(...)         : draw one item's content into its row rect
//   - row_height()/width()/max_visible_rows() : tunables (default to common
//                              values; override to change geometry)
//   - on_row_activated(i)    : fire the popup's typed on_accepted callback
//
// The viewport is always capped to max_visible_rows() (the popup's on-screen
// height never grows), but row_count() may exceed that — extra rows are
// reached via tk::ScrollableBase's scrollbar/scroll_y_, not truncated.
//
// Public selection API (selected_index/set_selected_index/visible_rows) is the
// same surface the shell keyboard handlers drive, so it stays here unchanged.
class ListPopupBase : public tk::ScrollableBase
{
public:
    // Number of rows to actually render (== rows clamped to max_visible_rows()).
    int visible_rows() const
    {
        return std::min((int)row_count(), max_visible_rows());
    }

    // Total number of items behind the popup, before the visible-rows cap —
    // i.e. how far scrolling/keyboard nav can actually reach.
    size_t total_rows() const
    {
        return row_count();
    }

    int selected_index() const
    {
        return selected_index_;
    }
    // Plain assignment matching Mention/Shortcode; subclasses that need
    // clamping (SlashCommand) override this.
    virtual void set_selected_index(int index)
    {
        selected_index_ = index;
    }

    // tk::Widget overrides — shared across all three popups.
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(tk::PaintCtx& ctx) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    void on_pointer_drag(tk::Point local) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

protected:
    // ---- Subclass contract --------------------------------------------------

    // How many items the model currently holds (before the visible-rows cap).
    virtual size_t row_count() const = 0;

    // Draw one item's content into `row_rect`. The base has already filled the
    // row background (selected/hovered) and will draw the separator/border; the
    // subclass only paints the item itself (text / icon / avatar).
    virtual void paint_row(tk::PaintCtx& ctx, const tk::Rect& row_rect,
                           size_t index, bool selected, bool hovered) = 0;

    // Fire the typed on_accepted callback for the activated row.
    virtual void on_row_activated(size_t index) = 0;

    // ---- Tunables (default to the common values; override as needed) --------
    virtual float row_height() const = 0;
    virtual float width() const = 0;
    virtual int max_visible_rows() const = 0;

    // tk::ScrollableBase contract — total content height behind the viewport.
    float content_height() const override
    {
        return float(row_count()) * row_height();
    }

    // Scroll so that row `index` is fully within the viewport. No-op for an
    // out-of-range index. Subclasses call this from their set_selected_index
    // override so keyboard nav keeps the selection visible.
    void ensure_row_visible(int index)
    {
        if (index < 0)
        {
            return;
        }
        float top = float(index) * row_height();
        float bottom = top + row_height();
        if (top < scroll_y_)
        {
            scroll_y_ = top;
        }
        else if (bottom > scroll_y_ + bounds_.h)
        {
            scroll_y_ = bottom - bounds_.h;
        }
        clamp_scroll();
    }

    // Reset hover/press transient state (called by set_* in subclasses).
    void reset_transient_state_()
    {
        hovered_index_ = -1;
        pressed_index_ = -1;
    }

    int selected_index_ = -1;
    int hovered_index_  = -1;
    int pressed_index_  = -1;

    // Map a widget-local y (0-based; parent has translated the canvas) to a row
    // index, or -1 when outside the populated rows. Accounts for scroll_y_ so
    // hit-testing resolves correctly against scrolled content.
    int row_at(float y) const
    {
        int r = (int)((y + scroll_y_) / row_height());
        return (r >= 0 && r < (int)row_count()) ? r : -1;
    }
};

} // namespace tesseract::views
