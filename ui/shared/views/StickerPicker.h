#pragma once

// Shared sticker picker. A self-contained tk::Widget parallel to
// EmojiPicker. Renders a virtualised grid of sticker thumbnails (one tab
// per pack plus a Favorites tab), backed by a host-supplied image cache.
//
// Just like EmojiPicker:
//   - search field rect for the host to overlay a NativeTextField on top
//   - bottom (or side) tab strip for pack navigation
//   - virtualised cell grid
//
// Unlike EmojiPicker, cells render bitmaps fetched async from the homeserver
// (matrix-sdk handles decryption for MSC2545 encrypted stickers). The host
// owns the bytes cache and exposes a synchronous `ImageProvider` lookup —
// when the picker asks for a sticker that hasn't been loaded yet the host
// kicks off a fetch on a worker thread and calls `invalidate_image_cache`
// once the result lands; the picker then repaints the affected cells.

#include "tk/canvas.h"
#include "tk/list_view.h"
#include "tk/widget.h"

#include <tesseract/image_pack.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract { class Client; }

namespace tesseract::views {

class StickerPicker : public tk::Widget {
public:
    StickerPicker();
    ~StickerPicker() override;

    /// Borrowed SDK client. Used to pull the pack list from
    /// `list_image_packs()` and the favourites from
    /// `list_favorite_stickers()`. Optional — when null the picker shows
    /// "Loading packs…".
    void set_client(tesseract::Client* c);

    /// Provider for already-decoded sticker bitmaps. Receives the entry's
    /// `url` (which doubles as the cache key) and the entry's
    /// `info_json` source token (passed to `Client::fetch_source_bytes`
    /// for encrypted entries). Returns null when the bitmap isn't in the
    /// host cache yet; the host is expected to kick off a fetch in that
    /// case and call `invalidate_image_cache()` once the bytes arrive.
    using ImageProvider =
        std::function<const tk::Image*(const std::string& cache_key,
                                        const std::string& source_token)>;
    void set_image_provider(ImageProvider p);

    /// Force a grid repaint — e.g. after the host's media cache lands new
    /// bitmaps the picker just asked for. Cheap.
    void invalidate_image_cache();

    /// Re-pull packs + favourites from the client. The picker calls this
    /// automatically on `set_client`; the host should call it from its
    /// `IEventHandler::on_image_packs_updated` callback.
    void refresh_packs();

    /// Host hook for the search-row overlay.
    tk::Rect search_field_rect() const { return search_rect_; }
    void     set_search_query(std::string query);

    /// Fires when the user picks a sticker. The host invokes
    /// `Client::send_sticker` with these fields.
    std::function<void(const tesseract::ImagePackImage&)> on_selected;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;
    bool     on_pointer_down(tk::Point local)                      override;
    void     on_pointer_up  (tk::Point local, bool inside_self)    override;

    // Test introspection.
    const std::vector<tesseract::ImagePack>&        packs()     const { return packs_; }
    const std::vector<tesseract::ImagePackImage>&   current()   const { return current_items_; }
    int                                              active_tab() const { return active_tab_; }

private:
    class GridAdapter;

    enum class Page : std::uint8_t { Favorites, Pack, Search };

    void switch_to_favorites();
    void switch_to_pack(int idx);
    void switch_to_search();
    void rebuild_current_items();

    int  tab_at(tk::Point local) const;
    tk::Rect tab_strip_rect() const;
    int  tab_count() const; // = 1 (Favorites) + packs_.size()

    tesseract::Client*                          client_   = nullptr;
    ImageProvider                                provider_;

    std::vector<tesseract::ImagePack>            packs_;          // sticker-capable packs only
    std::vector<tesseract::ImagePackImage>       favorites_;
    std::vector<tesseract::ImagePackImage>       current_items_;

    Page                                          page_       = Page::Favorites;
    int                                           active_tab_ = 0;   // 0 = Favorites, 1.. = pack
    std::string                                   query_;

    tk::GridView*                                 grid_ = nullptr;   // borrowed
    std::unique_ptr<GridAdapter>                  grid_adapter_;

    tk::Rect                                      search_rect_{};
    tk::Rect                                      grid_rect_{};
    tk::Rect                                      tab_rect_{};
    int                                           pressed_tab_idx_ = -1;
    int                                           hovered_tab_idx_ = -1;
};

} // namespace tesseract::views
