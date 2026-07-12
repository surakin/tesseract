#pragma once

// TabbedGridPicker — shared scaffolding for EmojiPicker and StickerPicker.
//
// Both pickers are a self-contained tk::Widget that paints:
//   - a search row (the host overlays a NativeTextField on
//     search_field_rect() so IME / selection stay native)
//   - a virtualised grid of cells (a borrowed tk::GridView child)
//   - a tab strip along the bottom edge
//
// This base owns everything that was byte-for-byte (or trivially) duplicated
// between the two pickers: the layout (search / grid / tab geometry), the
// GridView child + a forwarding GridAdapter, the host-supplied image cache
// (`provider_`), the tab strip (geometry, horizontal scroll, hit-test, chrome
// paint, hover/press state), the grid hover highlight, the shortcode tooltip,
// and every pointer/wheel override.
//
// Subclasses keep only their content model:
//   - item model:  item_count(), paint_cell(), on_item_activated()
//   - tab  model:  tab_count(), paint_tab_content(), on_tab_clicked(),
//                  active_tab_index()
//   - tooltip text: cell_tooltip()
//   - search:       on_search_query_changed()
//   - cell/tab pixel metrics via the layout-config virtuals.

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/list_view.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class TabbedGridPicker : public tk::Widget
{
public:
    TabbedGridPicker();
    ~TabbedGridPicker() override;

    /// Host-supplied image cache. Receives a cache key and a source token
    /// (used by encrypted MSC2545 entries) and returns the decoded bitmap or
    /// null when it isn't loaded yet. Shared by both pickers — used for custom
    /// emoticon/sticker cells and pack-avatar tabs.
    using ImageProvider = std::function<const tk::Image*(
        const std::string& cache_key, const std::string& source_token)>;
    void set_image_provider(ImageProvider p);

    /// Force a grid repaint after the host's media cache lands new bitmaps.
    void invalidate_image_cache();

    /// Host hook for the search-row overlay. Bounds in widget-local
    /// coordinates; valid after the first arrange() pass.
    tk::Rect search_field_rect() const
    {
        return search_rect_;
    }

    /// Called by the host's NativeTextField on every text change. Stores the
    /// query and forwards to on_search_query_changed().
    void set_search_query(std::string query);

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

protected:
    // ── Layout config (override to change pixel metrics) ─────────────────
    virtual float cell_size() const = 0;
    virtual float cell_gap() const = 0;
    virtual float grid_padding() const = 0; // GridView inner padding (Edges::all)
    virtual float tab_height() const = 0;
    virtual float tab_slot_min() const = 0; // floor on per-tab width

    // ── Item (grid) model ────────────────────────────────────────────────
    virtual std::size_t item_count() const = 0;
    virtual void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                            bool selected, bool hovered) = 0;
    virtual void on_item_activated(int index) = 0;

    // ── Tab model ─────────────────────────────────────────────────────────
    virtual int tab_count() const = 0;
    // Index of the active tab, or -1 (e.g. while in search mode). Drives the
    // active-tab highlight + underline.
    virtual int active_tab_index() const = 0;
    // Paint the tab's content (glyph / avatar / initial) inside `tab`. The base
    // has already drawn the active/hover background + underline.
    virtual void paint_tab_content(int index, tk::PaintCtx& ctx,
                                   tk::Rect tab) = 0;
    // User clicked tab `index` (already validated against tab_count()).
    virtual void on_tab_clicked(int index) = 0;

    // ── Tooltip ──────────────────────────────────────────────────────────
    // Shortcode shown when grid cell `index` is hovered, e.g. ":thumbs_up:".
    // Empty disables the tooltip for that cell.
    virtual std::string cell_tooltip(int index) const = 0;

    // ── Search ────────────────────────────────────────────────────────────
    // Called from set_search_query after `query_` is updated. `cleared` is
    // true when the query just became empty.
    virtual void on_search_query_changed(const std::string& query,
                                         bool cleared) = 0;

    // ── Helpers for subclasses ───────────────────────────────────────────
    const ImageProvider& image_provider() const
    {
        return provider_;
    }
    // Reset the grid (clear selection + re-measure). Subclasses call this after
    // mutating their item model.
    void refresh_grid();
    // Reset the horizontal tab scroll (e.g. on a full pack reload).
    void reset_tab_scroll()
    {
        tab_scroll_offset_ = 0.0f;
    }
    // The current search query (last value passed to set_search_query).
    const std::string& search_query() const
    {
        return query_;
    }

    tk::GridView* grid_ = nullptr; // borrowed

private:
    class GridAdapter;

    // Index of the tab at `local` (widget-local), or -1.
    int tab_at(tk::Point local) const;

    ImageProvider provider_;
    std::unique_ptr<GridAdapter> grid_adapter_;

    std::string query_;

    tk::Rect search_rect_{};
    tk::Rect grid_rect_{};
    tk::Rect tab_rect_{};

    int pressed_tab_idx_ = -1;
    int hovered_tab_idx_ = -1;
    float tab_scroll_offset_ = 0.0f;

    // Cached from paint() so the per-frame tooltip refresh can reach
    // Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;
};

} // namespace tesseract::views
