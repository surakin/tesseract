#pragma once

// Shared room list. Renders a `std::vector<tesseract::RoomInfo>` as four
// collapsible sections — Favorites / Direct Messages / Rooms / Spaces —
// each showing avatar + name + last-message preview + unread badge per row.
//
// Composition: this widget owns a child `tk::ListView` and an optional
// search header strip that the host overlays a `NativeTextField` on
// top of. The search strip is shown only when the total content height
// exceeds the viewport. Filtering by display name is case-insensitive and
// happens inside the view (section_rooms_ feeds the adapter). When a
// search query is active the collapsed state is ignored so all matches
// are visible.
//
// Avatar bytes live in a host-side cache; pass a provider lambda that
// returns the decoded tk::Image* for a given mxc_url (or nullptr for
// the initials-disc fallback).

#include "tk/canvas.h"
#include "tk/list_view.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views {

class RoomListView : public tk::Widget {
public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;

    RoomListView();
    ~RoomListView() override;     // out-of-line — Adapter is opaque here

    // Replace the room list. Re-measures, re-applies the search filter,
    // re-paints, and preserves selection by room ID across the swap.
    void set_rooms(std::vector<tesseract::RoomInfo> rooms);
    const std::vector<tesseract::RoomInfo>& rooms() const { return rooms_; }

    // Hook for retrieving decoded avatar images. The provider returns
    // null when the image isn't ready yet, in which case the row falls
    // back to an initials disc.
    void set_avatar_provider(AvatarProvider p);

    // Selection by room ID. Stays stable across filter changes — when
    // the filter excludes the selected room, the index is cleared on the
    // inner ListView but the chosen room ID is remembered; clearing the
    // filter restores the selection.
    void                set_selected_room(const std::string& room_id);
    std::string         selected_room_id() const;

    // Position among all currently visible room rows (headers excluded),
    // or -1 when nothing is selected. Preserved across search / collapse
    // changes as long as the selected room remains visible.
    int                 selected_index() const;

    // Search header: host-overlaid NativeTextField. The view exposes the
    // rect the field should occupy plus a visibility bit; layout decides
    // them in `arrange()` based on the inner list's content height.
    tk::Rect            search_field_rect()    const;
    bool                search_field_visible() const;

    // Host pipes its NativeTextField's text changes back into the view.
    void                set_search_text(std::string q);
    const std::string&  search_text() const { return search_text_; }

    // Room IDs of all room rows currently visible in the viewport.
    // Headers are excluded. Empty when no rows are visible or laid out.
    std::vector<std::string> visible_room_ids() const;

    // Fires when the user clicks a room row (selection moves to that row).
    std::function<void(const std::string& /*room_id*/)> on_room_selected;

    // Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;
    bool     on_pointer_down(tk::Point local)              override;
    void     on_pointer_up  (tk::Point local, bool inside_self) override;

private:
    class Adapter;

    // Section indices (matches array positions throughout the class).
    static constexpr int kSecFavorites = 0;
    static constexpr int kSecDMs       = 1;
    static constexpr int kSecRooms     = 2;
    static constexpr int kSecSpaces    = 3;
    static constexpr int kNumSections  = 4;

    static constexpr const char* kSectionTitles[kNumSections] = {
        "Favorites", "Direct Messages", "Rooms", "Spaces"
    };

    // Flat item list fed to the inner ListView. Each item is either a
    // collapsible section header or a pointer into section_rooms_[s].
    struct Item {
        enum class Kind : uint8_t { Header, Room } kind = Kind::Room;
        int section  = 0;  // which section (0-3)
        int room_idx = 0;  // index within section_rooms_[section] (Room only)
    };

    // Rebuild section_rooms_[4] from rooms_ + search filter, then
    // rebuild items_ from those buckets + collapsed_[] state.
    void rebuild_items();

    float search_header_h() const;

    std::vector<tesseract::RoomInfo>         rooms_;
    // Per-section filtered room pointers (into rooms_); rebuilt by
    // rebuild_items(). Invalidated whenever rooms_ is replaced.
    std::vector<const tesseract::RoomInfo*>  section_rooms_[kNumSections];
    // Flat item list the adapter iterates over.
    std::vector<Item>                        items_;
    // Collapsed state per section (false = expanded by default).
    bool                                     collapsed_[kNumSections] = {};

    std::string                              search_text_;
    AvatarProvider                           avatar_provider_;

    std::unique_ptr<Adapter>                 adapter_;
    tk::ListView*                            list_                  = nullptr;
    tk::Rect                                 search_field_rect_     {};
    bool                                     search_field_visible_  = false;

    // Last-known selected room ID, preserved across filter/collapse changes
    // when the selection is temporarily hidden.
    std::string                              selected_room_id_cache_;
};

} // namespace tesseract::views
