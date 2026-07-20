#pragma once

// Shared emoji picker. A TabbedGridPicker whose grid paints Unicode glyphs
// (and custom MSC2545 emoticon images on pack tabs) and whose tab strip is:
//   - a Frequents tab (when non-empty)
//   - the 8 Unicode categories
//   - one tab per custom emoticon pack
//
// The host wires the picker into a popover window of its choice
// (QFrame popup, NSPanel, WS_POPUP HWND, GtkPopover). Selection fires
// `on_selected(glyph)`; the picker also calls Client::recent_emoji_bump
// if a Client is provided so the Frequents tab learns from use.
//
// The tab strip, virtualised grid, search-mode switching, image-provider
// cache, and pointer/hover/press handling live in TabbedGridPicker.

#include "TabbedGridPicker.h"

#include <tesseract/emoji.h>
#include <tesseract/image_pack.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tesseract
{
class Client;
}

namespace tesseract::views
{

class EmojiPicker : public TabbedGridPicker
{
protected:
    EmojiPicker();
    TK_WIDGET_FACTORY_FRIEND(EmojiPicker)

public:
    // Popup card dimensions (logical px) — canonical size for hosts using
    // register_popup()/open_at(), replacing the shell-duplicated size
    // constants each native-popup wrapper used to define separately.
    static constexpr float kWidth  = 304.0f;
    static constexpr float kHeight = 360.0f;

    ~EmojiPicker() override;

    /// Borrowed SDK client. Used to read `recent_emoji_top` for the
    /// Frequents tab and to call `recent_emoji_bump` on selection. May
    /// be null (the picker still functions; the Frequents tab is empty).
    void set_client(tesseract::Client* c);

    /// Which room this picker is currently being shown for (empty = no
    /// room context, e.g. viewing the room list). Read by
    /// `refresh_emoticon_packs()` to order that room's own pack right
    /// after the personal pack — see `order_picker_packs`. Callers should
    /// set this before calling `refresh_emoticon_packs()`.
    void set_current_room_id(std::string room_id)
    {
        current_room_id_ = std::move(room_id);
    }

    /// Every Space (direct and ancestor) that the current room is in — see
    /// `ShellBase::parent_spaces_for_room_`. Read by
    /// `refresh_emoticon_packs()` to surface those spaces' own packs right
    /// after the current room's pack — see `order_picker_packs`. Callers
    /// should set this before calling `refresh_emoticon_packs()`.
    void set_current_room_parent_spaces(std::vector<std::string> space_ids)
    {
        current_room_parent_spaces_ = std::move(space_ids);
    }

    /// Resolve a cached bitmap for a custom emoticon's mxc:// URL via the
    /// picker's own image provider, or null on a cache miss. Hosts pass
    /// this to tk::TextArea::insert_emoticon when inline-inserting a
    /// selected emoticon — some backends (Qt6's NativeTextArea in
    /// particular) render a plain ":shortcode:" fallback instead of a real
    /// inline image when given a null image, with no later async resolve,
    /// unlike Win32/macOS/GTK4's resolver-backed paths — so passing an
    /// already-cached bitmap here (the grid was just displaying this exact
    /// image, so it usually is cached) keeps that working uniformly.
    const tk::Image* resolve_image(const std::string& mxc_url) const
    {
        return image_provider() ? image_provider()(mxc_url, mxc_url) : nullptr;
    }

    /// Pull the latest Frequents from the client. Call before each
    /// presentation so re-shows reflect new picks.
    void refresh_frequents();

    /// Refresh the MSC2545 emoticon packs (extra tabs after the Unicode
    /// categories). Hosts call this from
    /// `IEventHandler::on_image_packs_updated`.
    void refresh_emoticon_packs();

    /// Fires when the user picks a Unicode glyph.
    std::function<void(const std::string&)> on_selected;

    /// Fires when the user picks a custom (MSC2545) emoticon from a pack
    /// tab. For now hosts insert `:shortcode:` into the compose field;
    /// the MSC2545 Phase B "rich emoticon" sending path lives in a
    /// follow-up.
    std::function<void(const tesseract::ImagePackImage&)> on_emoticon_selected;

protected:
    // Layout config.
    float cell_size() const override
    {
        return 32.0f;
    }
    float cell_gap() const override
    {
        return 2.0f;
    }
    float grid_padding() const override
    {
        return 4.0f; // kPadding * 0.5
    }
    float tab_height() const override
    {
        return 32.0f;
    }
    float tab_slot_min() const override
    {
        return 24.0f;
    }

    // Item model.
    std::size_t item_count() const override;
    void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool selected, bool hovered) override;
    void on_item_activated(int index) override;
    std::string cell_tooltip(int index) const override;

    // Tab model.
    int tab_count() const override;
    int active_tab_index() const override;
    void paint_tab_content(int index, tk::PaintCtx& ctx, tk::Rect tab) override;
    void on_tab_clicked(int index) override;

    // Search.
    void on_search_query_changed(const std::string& query,
                                 bool cleared) override;

private:
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

    // Tab layout. Visual indexes:
    //   0          → Frequents (only when has_frequents_tab())
    //   foff..     → Unicode categories (kCategories)
    //   then       → Custom MSC2545 emoticon packs (in custom_packs_ order)
    bool has_frequents_tab() const
    {
        return !frequents_glyphs_.empty();
    }
    int frequents_tab_offset() const
    {
        return has_frequents_tab() ? 1 : 0;
    }

    tesseract::Client* client_ = nullptr;
    std::string current_room_id_;
    std::vector<std::string> current_room_parent_spaces_;

    Page page_ = Page::Category;
    tesseract::emoji::Category category_ =
        tesseract::emoji::Category::SmileysPeople;
    int custom_pack_idx_ = -1;

    std::vector<std::string> frequents_glyphs_;
    std::vector<std::string> current_glyphs_;        // unicode page items
    std::vector<tesseract::ImagePack> custom_packs_; // emoticon-capable
    std::vector<tesseract::ImagePackImage>
        current_emoticons_; // image-cell items
    std::vector<std::string>
        current_shortcodes_; // parallel to current_glyphs_ / current_emoticons_
};

} // namespace tesseract::views
