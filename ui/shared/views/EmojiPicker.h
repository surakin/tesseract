#pragma once

// Shared emoji picker. A self-contained tk::Widget that paints:
//   - a search row (the host overlays a NativeTextField on
//     search_field_rect() so IME / selection stay native)
//   - a virtualised grid of emoji glyphs
//   - a bottom tab strip: Frequents + the 8 Unicode categories
//
// The host wires the picker into a popover window of its choice
// (QFrame popup, NSPanel, WS_POPUP HWND, GtkPopover). Selection fires
// `on_selected(glyph)`; the picker also calls Client::recent_emoji_bump
// if a Client is provided so the Frequents tab learns from use.

#include "tk/canvas.h"
#include "tk/list_view.h"
#include "tk/widget.h"

#include <tesseract/emoji.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract { class Client; }

namespace tesseract::views {

class EmojiPicker : public tk::Widget {
public:
    EmojiPicker();
    ~EmojiPicker() override;

    /// Borrowed SDK client. Used to read `recent_emoji_top` for the
    /// Frequents tab and to call `recent_emoji_bump` on selection. May
    /// be null (the picker still functions; the Frequents tab is empty).
    void set_client(tesseract::Client* c);

    /// Pull the latest Frequents from the client. Call before each
    /// presentation so re-shows reflect new picks.
    void refresh_frequents();

    /// Host hook for the search-row overlay. Bounds in widget-local
    /// coordinates; valid after the first arrange() pass.
    tk::Rect search_field_rect() const { return search_rect_; }

    /// Called by the host's NativeTextField on every text change.
    void set_search_query(std::string query);

    /// Fires when the user picks an emoji.
    std::function<void(const std::string&)> on_selected;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;
    bool     on_pointer_down(tk::Point local)                      override;
    void     on_pointer_up  (tk::Point local, bool inside_self)    override;

private:
    class GridAdapter;

    enum class Page : std::uint8_t { Frequents, Category, Search };

    void switch_to_frequents();
    void switch_to_category(tesseract::emoji::Category c);
    void switch_to_search();
    void rebuild_current_items();

    int tab_at(tk::Point local) const;   // 0 = Frequents, 1..8 = categories
    tk::Rect tab_strip_rect() const;

    tesseract::Client*                   client_   = nullptr;

    Page                                 page_     = Page::Category;
    tesseract::emoji::Category           category_ = tesseract::emoji::Category::SmileysPeople;
    std::string                          query_;

    std::vector<std::string>             frequents_glyphs_;
    std::vector<std::string>             current_glyphs_;     // backs the grid

    tk::GridView*                        grid_         = nullptr;   // borrowed
    std::unique_ptr<GridAdapter>         grid_adapter_;

    tk::Rect                             search_rect_{};
    tk::Rect                             grid_rect_{};
    tk::Rect                             tab_rect_{};
    int                                  pressed_tab_idx_ = -1;
    int                                  hovered_tab_idx_ = -1;
};

} // namespace tesseract::views
