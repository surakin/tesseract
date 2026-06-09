#pragma once
#include "tk/widget.h"
#include <algorithm>

namespace tesseract::views
{

// Shared scaffolding for the composer's single-column list popups
// (MentionPopup / ShortcodePopup / SlashCommandPopup). These three widgets
// differ only in their item data model and in how a single row is drawn;
// everything else — measure/arrange, hit-testing, pointer + wheel handling,
// the selected/hovered row fill, per-row separators, and the 1px border — is
// identical and lives here.
//
// Subclasses provide:
//   - row_count()            : how many rows are currently visible
//   - paint_row(...)         : draw one item's content into its row rect
//   - row_height()/width()/max_visible_rows() : tunables (default to common
//                              values; override to change geometry)
//   - on_row_activated(i)    : fire the popup's typed on_accepted callback
//
// Public selection API (selected_index/set_selected_index/visible_rows) is the
// same surface the shell keyboard handlers drive, so it stays here unchanged.
class ListPopupBase : public tk::Widget
{
public:
    // Number of rows to actually render (== rows clamped to max_visible_rows()).
    int visible_rows() const
    {
        return std::min((int)row_count(), max_visible_rows());
    }

    int selected_index() const
    {
        return selected_index_;
    }
    // Plain assignment matching Mention/Shortcode; subclasses that need
    // clamping (SlashCommand) override the non-virtual setter via hiding —
    // controllers always call through the concrete popup type.
    void set_selected_index(int index)
    {
        selected_index_ = index;
    }

    // tk::Widget overrides — shared across all three popups.
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(tk::PaintCtx& ctx) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

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
    // index, or -1 when outside the populated rows.
    int row_at(float y) const
    {
        int r = (int)(y / row_height());
        return (r >= 0 && r < visible_rows()) ? r : -1;
    }
};

} // namespace tesseract::views
