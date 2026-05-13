#pragma once

// Shared room list. Renders a `std::vector<tesseract::RoomInfo>` as
// rows of avatar + name + last-message preview + unread badge.
//
// Composition: this widget owns a child `tk::ListView` and an optional
// search header strip that the host overlays a `NativeTextField` on
// top of. The search strip is shown only when the total content height
// exceeds the viewport — accounts with a handful of rooms don't get a
// useless input bar. Filtering by display name is case-insensitive and
// happens inside the view (filtered_rooms_ feeds the adapter).
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

    // Index into the currently visible (filtered) row set, or -1 when
    // nothing is selected. Exposed mostly for tests; production code
    // should prefer `selected_room_id()`.
    int                 selected_index() const;

    // Search header: host-overlaid NativeTextField. The view exposes the
    // rect the field should occupy plus a visibility bit; layout decides
    // them in `arrange()` based on the inner list's content height.
    tk::Rect            search_field_rect()    const;
    bool                search_field_visible() const;

    // Host pipes its NativeTextField's text changes back into the view.
    void                set_search_text(std::string q);
    const std::string&  search_text() const { return search_text_; }

    // Fires when the user clicks a row (selection moves to that row).
    std::function<void(const std::string& /*room_id*/)> on_room_selected;

    // Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;
    bool     on_pointer_down(tk::Point local)              override;
    void     on_pointer_up  (tk::Point local, bool inside_self) override;

private:
    class Adapter;

    void rebuild_filtered();
    float search_header_h() const;

    std::vector<tesseract::RoomInfo> rooms_;
    std::vector<tesseract::RoomInfo> filtered_rooms_;
    std::string                      search_text_;
    AvatarProvider                   avatar_provider_;

    std::unique_ptr<Adapter>         adapter_;
    tk::ListView*                    list_                  = nullptr; // borrowed
    tk::Rect                         search_field_rect_     {};
    bool                             search_field_visible_  = false;

    // Last-known selected room ID, preserved across filter changes when
    // the filter temporarily hides the selected row.
    std::string                      selected_room_id_cache_;
};

} // namespace tesseract::views
