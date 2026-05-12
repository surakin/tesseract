#pragma once

// Shared room list. Renders a `std::vector<tesseract::RoomInfo>` as
// rows of avatar + name + last-message preview + unread badge. Selection
// + click are handled by the underlying tk::ListView; the host wires
// pointer events through the standard dispatch chain.
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

class RoomListView : public tk::ListView {
public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;

    RoomListView();
    ~RoomListView() override;     // out-of-line — Adapter is opaque here

    // Replace the room list. Re-measures and re-paints.
    void set_rooms(std::vector<tesseract::RoomInfo> rooms);
    const std::vector<tesseract::RoomInfo>& rooms() const { return rooms_; }

    // Hook for retrieving decoded avatar images. The provider returns
    // null when the image isn't ready yet, in which case the row falls
    // back to an initials disc.
    void set_avatar_provider(AvatarProvider p);

    // Selection by room ID, kept in sync with the underlying integer
    // selected_index().
    void set_selected_room(const std::string& room_id);
    std::string selected_room_id() const;

    // Fires when the user clicks a row (selection moves to that row).
    std::function<void(const std::string& /*room_id*/)> on_room_selected;

private:
    class Adapter;

    std::vector<tesseract::RoomInfo> rooms_;
    AvatarProvider                   avatar_provider_;
    std::unique_ptr<Adapter>         adapter_;
};

} // namespace tesseract::views
