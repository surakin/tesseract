#pragma once

// Left-side widget of the space room-management section (SpaceRootView): a
// search field + flat filtered list of the user's joined rooms that are
// candidates to add to the space (i.e. not already a child). Dragging a row
// onto SpaceChildRoomGrid, or pressing Enter/clicking a row, requests adding
// that room; a drop landing here (from the grid) requests removing the
// dropped room. This widget never mutates space membership itself — it only
// reports intent via callbacks; SpaceRootView/the shell perform the actual
// (optimistic) state change and the async API call.

#include "tk/canvas.h"
#include "tk/drag_drop.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace tesseract::views
{

class SpaceAddRoomList : public tk::Widget
{
protected:
    // host() is nullable: when null, the search field is skipped (mirrors
    // ForwardRoomPicker's identical rationale for detached tests).
    SpaceAddRoomList();
    TK_WIDGET_FACTORY_FRIEND(SpaceAddRoomList)

public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;
    using RoomsProvider = std::function<std::vector<tesseract::RoomInfo>()>;

    ~SpaceAddRoomList() override;

    // ── Data ─────────────────────────────────────────────────────────────
    void set_rooms_provider(RoomsProvider p);
    void set_avatar_provider(AvatarProvider p);
    // Room ids to exclude from the candidate list (the space's own id, plus
    // its current children, joined and unjoined).
    void set_excluded_room_ids(std::vector<std::string> ids);
    // Re-pulls from rooms_provider_, reapplies exclusions + the current
    // query, and rebuilds the list. Call after set_excluded_room_ids() or
    // whenever the underlying room set may have changed.
    void refresh();
    // Host pipes NativeTextField text changes in here.
    void set_query(const std::string& q);

    // Gates add interactions (drag-source, click/Enter). The list stays
    // visible for context even when false.
    void set_can_manage(bool v);
    bool can_manage() const { return can_manage_; }

    // ── Native-field rect delegation (mirrors ForwardRoomPicker) ───────────
    tk::Rect search_field_rect() const { return search_field_rect_; }
    tk::TextField* search_field() const { return search_field_; }

    // ── Callbacks ────────────────────────────────────────────────────────
    // Fired by Enter/click on a candidate row.
    std::function<void(std::string room_id)> on_add_requested;
    // Fired when a "space_room_id" drag payload (from SpaceChildRoomGrid)
    // is dropped here — requests removing that room from the space.
    std::function<void(std::string room_id)> on_room_dropped_for_remove;
    std::function<void(const tesseract::RoomInfo&)> on_room_avatar_needed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

    bool on_drag_enter(tk::Point local, const tk::DragPayload& payload) override;
    void on_drag_over(tk::Point local, const tk::DragPayload& payload) override;
    void on_drag_leave_target() override;
    bool on_drop(tk::Point local, tk::DragPayload payload) override;

    static constexpr float kRowH = 40.0f;

private:
    class Adapter;
    friend class Adapter;
    class InnerListView;
    friend class InnerListView;

    void refilter_();
    const tesseract::RoomInfo* room_at_(std::size_t index) const;
    tk::DragVisual build_drag_visual_(const tesseract::RoomInfo& room);
    std::string tooltip_text_(const tesseract::RoomInfo& room) const;

    RoomsProvider  rooms_provider_;
    AvatarProvider avatar_provider_;
    std::vector<tesseract::RoomInfo> all_candidates_;
    std::vector<tesseract::RoomInfo> filtered_;
    std::unordered_set<std::string> excluded_ids_;
    std::string query_;
    bool can_manage_ = true;

    std::unique_ptr<Adapter> adapter_;
    InnerListView* list_ = nullptr;
    tk::TextField* search_field_ = nullptr;
    tk::Rect search_field_rect_{};

    bool drag_hover_ = false;

    // Cached from the most recent paint() — used to rasterize a drag-ghost
    // bitmap outside of a paint pass (on_pointer_drag has no PaintCtx of its
    // own). Mirrors SpaceRootView's own factory_seen_ idiom for the same
    // reason.
    tk::CanvasFactory* factory_ = nullptr;
    const tk::Theme* theme_ = nullptr;
    float scale_factor_ = 1.0f;
};

} // namespace tesseract::views
