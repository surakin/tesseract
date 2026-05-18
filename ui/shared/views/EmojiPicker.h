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
#include <tesseract/image_pack.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract
{
class Client;
}

namespace tesseract::views
{

class EmojiPicker : public tk::Widget
{
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

    /// Refresh the MSC2545 emoticon packs (extra tabs after the Unicode
    /// categories). Hosts call this from
    /// `IEventHandler::on_image_packs_updated`.
    void refresh_emoticon_packs();

    /// Host-supplied image cache (same shape as MessageListView). Used to
    /// render custom emoticon tabs.
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

    /// Called by the host's NativeTextField on every text change.
    void set_search_query(std::string query);

    /// Fires when the user picks a Unicode glyph.
    std::function<void(const std::string&)> on_selected;

    /// Fires when the user picks a custom (MSC2545) emoticon from a pack
    /// tab. For now hosts insert `:shortcode:` into the compose field;
    /// the MSC2545 Phase B "rich emoticon" sending path lives in a
    /// follow-up.
    std::function<void(const tesseract::ImagePackImage&)> on_emoticon_selected;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_wheel(tk::Point local, float dx, float dy) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    class GridAdapter;

    enum class Page : std::uint8_t
    {
        Frequents,
        Category,
        CustomPack,
        Search
    };

    void switch_to_frequents();
    void switch_to_category(tesseract::emoji::Category c);
    void switch_to_custom_pack(int idx);
    void switch_to_search();
    void rebuild_current_items();

    // Tab layout. Indexes:
    //   0          → Frequents
    //   1..N       → Unicode categories (kCategories)
    //   N+1..end   → Custom MSC2545 emoticon packs (in custom_packs_ order)
    int tab_at(tk::Point local) const;
    tk::Rect tab_strip_rect() const;
    int builtin_tab_count() const; // 1 + kCategories (constant)
    int
    total_tab_count() const; // frequents (if any) + categories + custom packs
    bool has_frequents_tab() const
    {
        return !frequents_glyphs_.empty();
    }
    int frequents_tab_offset() const
    {
        return has_frequents_tab() ? 1 : 0;
    }

    tesseract::Client* client_ = nullptr;
    ImageProvider provider_;

    Page page_ = Page::Category;
    tesseract::emoji::Category category_ =
        tesseract::emoji::Category::SmileysPeople;
    int custom_pack_idx_ = -1;
    std::string query_;

    std::vector<std::string> frequents_glyphs_;
    std::vector<std::string> current_glyphs_;        // unicode page items
    std::vector<tesseract::ImagePack> custom_packs_; // emoticon-capable
    std::vector<tesseract::ImagePackImage>
        current_emoticons_; // image-cell items
    std::vector<std::string>
        current_shortcodes_; // parallel to current_glyphs_ / current_emoticons_
    int hovered_grid_cell_ = -1;

    tk::GridView* grid_ = nullptr; // borrowed
    std::unique_ptr<GridAdapter> grid_adapter_;

    tk::Rect search_rect_{};
    tk::Rect grid_rect_{};
    tk::Rect tab_rect_{};
    int pressed_tab_idx_ = -1;
    int hovered_tab_idx_ = -1;
    float tab_scroll_offset_ = 0.0f;
};

} // namespace tesseract::views
