#pragma once

// Shared sticker picker. A TabbedGridPicker whose grid paints sticker
// thumbnails (one tab per pack plus a Favorites tab), backed by a host-supplied
// image cache.
//
// Cells render bitmaps fetched async from the homeserver (matrix-sdk handles
// decryption for MSC2545 encrypted stickers). The host owns the bytes cache and
// exposes a synchronous `ImageProvider` lookup — when the picker asks for a
// sticker that hasn't been loaded yet the host kicks off a fetch on a worker
// thread and calls `invalidate_image_cache` once the result lands; the picker
// then repaints the affected cells.
//
// The tab strip, virtualised grid, search-mode switching, image-provider
// cache, and pointer/hover/press handling live in TabbedGridPicker.

#include "TabbedGridPicker.h"
#include "tk/media_kind.h"

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

class StickerPicker : public TabbedGridPicker
{
protected:
    StickerPicker();
    TK_WIDGET_FACTORY_FRIEND(StickerPicker)

public:
    // Popup card dimensions (logical px) — canonical size for hosts using
    // register_popup()/open_at(), replacing the shell-duplicated size
    // constants each native-popup wrapper used to define separately.
    static constexpr float kWidth  = 360.0f;
    static constexpr float kHeight = 420.0f;

    ~StickerPicker() override;

    /// Borrowed SDK client. Used to pull the pack list from
    /// `list_image_packs()` and the favourites from
    /// `list_favorite_stickers()`. Optional — when null the picker shows
    /// "Loading packs…".
    void set_client(tesseract::Client* c);

    /// Which room this picker is currently being shown for (empty = no
    /// room context, e.g. viewing the room list). Read by `refresh_packs()`
    /// to order that room's own pack right after the personal pack — see
    /// `order_picker_packs`. Callers should set this before calling
    /// `refresh_packs()`.
    void set_current_room_id(std::string room_id)
    {
        current_room_id_ = std::move(room_id);
    }

    /// Every Space (direct and ancestor) that the current room is in — see
    /// `ShellBase::parent_spaces_for_room_`. Read by `refresh_packs()` to
    /// surface those spaces' own packs right after the current room's pack
    /// — see `order_picker_packs`. Callers should set this before calling
    /// `refresh_packs()`.
    void set_current_room_parent_spaces(std::vector<std::string> space_ids)
    {
        current_room_parent_spaces_ = std::move(space_ids);
    }

    /// Re-pull packs + favourites from the client. The picker calls this
    /// automatically on `set_client`; the host should call it from its
    /// `IEventHandler::on_image_packs_updated` callback.
    void refresh_packs();

    /// Fires when the user picks a sticker. The host invokes
    /// `Client::send_sticker` with these fields.
    std::function<void(const tesseract::ImagePackImage&)> on_selected;

    // Test introspection.
    const std::vector<tesseract::ImagePack>& packs() const
    {
        return packs_;
    }
    const std::vector<tesseract::ImagePackImage>& current() const
    {
        return current_items_;
    }
    int active_tab() const
    {
        return active_tab_;
    }

    // For ShellBase::run_media_prefetch_'s pre-paint disk-cache warm pass —
    // see tk/media_kind.h's doc comment. Mirrors paint_cell's key
    // derivation for the active tab's current_items_ (the picker has no
    // GridView-level viewport culling to scope further — unlike
    // MessageListView/RoomListView/RoomMediaView, tk::GridView exposes no
    // visible_range() equivalent — so this covers the whole active tab;
    // run_media_prefetch_impl_'s max_items cap still bounds total work).
    // Tagged MediaKind::MediaImage: despite the "is_sticker" bool
    // ShellBase::ensure_picker_image_ takes, its actual cache routing/decode
    // size (plain url key, image_cache_, kMaxInlineImageWidth/Height) is
    // identical to MediaKind::MediaImage, not MediaKind::Sticker (that tag
    // is timeline-only, decode-clamped to kStickerSize instead). Pack-avatar
    // tab icons are not included (a smaller, separate concern — see
    // paint_tab_content's own image_provider() call).
    std::vector<tk::MediaPrefetchKey> collect_prefetchable_media_keys() const;

protected:
    // Layout config.
    float cell_size() const override
    {
        return 96.0f;
    }
    float cell_gap() const override
    {
        return 6.0f;
    }
    float grid_padding() const override
    {
        return 8.0f;
    }
    float tab_height() const override
    {
        return 40.0f; // taller than emoji tabs to host avatars
    }
    float tab_slot_min() const override
    {
        return 40.0f;
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
        Favorites,
        Pack,
        Search
    };

    void switch_to_favorites();
    void switch_to_pack(int idx);
    void switch_to_search();
    void rebuild_current_items();

    bool has_favorites_tab() const
    {
        return !favorites_.empty();
    }
    int favorites_tab_offset() const
    {
        return has_favorites_tab() ? 1 : 0;
    }

    tesseract::Client* client_ = nullptr;
    std::string current_room_id_;
    std::vector<std::string> current_room_parent_spaces_;

    std::vector<tesseract::ImagePack> packs_; // sticker-capable packs only
    std::vector<tesseract::ImagePackImage> favorites_;
    std::vector<tesseract::ImagePackImage> current_items_;

    Page page_ = Page::Favorites;
    int active_tab_ = 0; // 0 = Favorites, 1.. = pack
};

} // namespace tesseract::views
