#pragma once

// Right-side widget of the space room-management section (SpaceRootView): a
// grid of the space's current child rooms (joined + unjoined), each cell
// painted like a simplified RoomListView row (avatar + name). Dragging a
// cell onto SpaceAddRoomList, or selecting a cell and pressing Delete,
// requests removing that room; a drop landing here (from the list) requests
// adding the dropped room. This widget never mutates space membership
// itself — see SpaceAddRoomList's identical note.

#include "tk/canvas.h"
#include "tk/drag_drop.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class SpaceChildRoomGrid : public tk::Widget
{
protected:
    SpaceChildRoomGrid();
    TK_WIDGET_FACTORY_FRIEND(SpaceChildRoomGrid)

public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;

    struct ChildRoomEntry
    {
        tesseract::RoomInfo info;
        bool joined = true;
    };

    ~SpaceChildRoomGrid() override;

    void set_children(std::vector<ChildRoomEntry> children);
    void set_avatar_provider(AvatarProvider p);
    void set_can_manage(bool v);
    bool can_manage() const { return can_manage_; }

    // ── Callbacks ────────────────────────────────────────────────────────
    // Fired (at most once per room_id per set_children() call) for a child
    // whose entry has no name yet — an unjoined child awaiting its MSC3266
    // preview. This widget never fetches it itself.
    std::function<void(std::string room_id)> on_unjoined_summary_needed;
    // Fired by Delete on the selected cell.
    std::function<void(std::string room_id)> on_remove_requested;
    // Fired when a "space_room_id" drag payload (from SpaceAddRoomList) is
    // dropped here — requests adding that room to the space.
    std::function<void(std::string room_id)> on_room_dropped_for_add;
    std::function<void(const tesseract::RoomInfo&)> on_room_avatar_needed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

    bool on_drag_enter(tk::Point local, const tk::DragPayload& payload) override;
    void on_drag_over(tk::Point local, const tk::DragPayload& payload) override;
    void on_drag_leave_target() override;
    bool on_drop(tk::Point local, tk::DragPayload payload) override;

    // Cell shape matches a RoomListView row (avatar + single-line name,
    // horizontal), just laid out in a wrapping grid instead of a vertical
    // list — same visual language as SpaceAddRoomList's rows and
    // RoomListView's own rows, not a vertical avatar-on-top card.
    static constexpr float kCellW = 220.0f;
    static constexpr float kCellH = 48.0f;

private:
    class Adapter;
    friend class Adapter;
    class InnerGridView;
    friend class InnerGridView;

    const ChildRoomEntry* entry_at_(std::size_t index) const;
    tk::DragVisual build_drag_visual_(const ChildRoomEntry& entry);
    std::string tooltip_text_(const ChildRoomEntry& entry) const;

    std::vector<ChildRoomEntry> children_;
    AvatarProvider avatar_provider_;
    bool can_manage_ = true;

    std::unique_ptr<Adapter> adapter_;
    InnerGridView* grid_ = nullptr;

    bool drag_hover_ = false;

    // Cached from the most recent paint() — see SpaceAddRoomList's identical
    // members for the rationale (drag-ghost rendering outside a paint pass).
    tk::CanvasFactory* factory_ = nullptr;
    const tk::Theme* theme_ = nullptr;
    float scale_factor_ = 1.0f;
};

} // namespace tesseract::views
