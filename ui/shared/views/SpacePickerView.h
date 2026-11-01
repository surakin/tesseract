#pragma once

// Narrow vertical strip of avatar tabs to the left of the room list: a fixed
// Home tab, then one tab per space the user pinned. Hidden by MainAppWidget
// while nothing is pinned. Selecting a tab drills the sidebar into that space
// (Home returns to the plain room list); right-click offers "Unpin from
// sidebar". Unread state is the space's aggregated child counts.

#include "PopupMenu.h"
#include "tk/canvas.h"
#include "tk/drag_drop.h"
#include "tk/drag_gesture.h"
#include "tk/scrollable_base.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{
class KeyboardTarget;
}

namespace tesseract::views
{

struct SpacePickerEntry
{
    std::string space_id;
    std::string name;
    std::string avatar_url; // mxc://… (empty → initials fallback)
    std::uint64_t notification_count = 0;
    std::uint64_t highlight_count = 0;
    std::uint64_t unread_count = 0;
    bool muted = false;

    bool operator==(const SpacePickerEntry&) const = default;
};

class SpacePickerView : public tk::ScrollableBase
{
public:
    static constexpr float kWidth = 56.0f;

    SpacePickerView();
    ~SpacePickerView() override = default;

    /// Replace the pinned tabs (order = tab order). No-op when unchanged.
    void set_entries(std::vector<SpacePickerEntry> entries);
    /// The highlighted tab; empty = Home.
    void set_active_space(const std::string& space_id);

    using ImageProvider = std::function<const tk::Image*(const std::string&)>;
    void set_image_provider(ImageProvider p);

    std::function<void(const std::string& /*space_id*/)> on_select;
    std::function<void()> on_home;
    /// A space tab was dragged to a new place. `before_space_id` is the tab it
    /// now sits in front of (empty = moved to the end). The picker has already
    /// reordered its own entries; the host persists the order.
    std::function<void(const std::string& /*space_id*/, const std::string& /*before_space_id*/)>
        on_reorder;
    std::function<void(const std::string& /*space_id*/)> on_unpin;
    /// Fired while painting a tab whose avatar isn't cached yet.
    std::function<void(const std::string& /*space_id*/)> on_avatar_needed;

    const std::vector<SpacePickerEntry>& entries() const
    {
        return entries_;
    }
    const std::string& active_space() const
    {
        return active_;
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    // Draws in paint_before_children (not paint()) so the base paint still
    // runs paint_children(): the tab context menu is a child, and it only
    // registers as the host's popup (click-outside/Escape dismissal) by being
    // painted — see PopupMenu.
    void paint_before_children(tk::PaintCtx&) override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    void on_pointer_drag(tk::Point local) override;
    bool on_pointer_move(tk::Point local) override;
    // Drop target for the picker's own tabs (tk in-app drag-and-drop).
    bool on_drag_enter(tk::Point local, const tk::DragPayload& payload) override;
    void on_drag_over(tk::Point local, const tk::DragPayload& payload) override;
    void on_drag_leave_target() override;
    bool on_drop(tk::Point local, tk::DragPayload payload) override;
    void on_pointer_leave() override;
    bool on_right_click(tk::Point local) override;
    void on_popup_dismiss() override;

    // Test helpers. Tab 0 is Home; tab i+1 is entries()[i].
    int tab_count() const
    {
        return static_cast<int>(entries_.size()) + 1;
    }
    int drop_slot_for_test() const
    {
        return drop_slot_;
    }
    tk::Rect tab_rect_for_test(int tab) const
    {
        return tab_rect_(tab);
    }
    const std::vector<PopupMenu::Item>& context_menu_items_for_test() const
    {
        return menu_->items_for_test();
    }

protected:
    float content_height() const override
    {
        return kPadTop + kTabH * static_cast<float>(tab_count());
    }
    bool apply_scroll_delta(float dy) override;

public:
    void scroll_into_view(tk::Rect world_rect) override;

private:
    static constexpr float kTabH = 48.0f;
    static constexpr float kPadTop = 8.0f;
    static constexpr float kAvatar = 36.0f;

    tk::Rect tab_rect_(int tab) const;
    int tab_at_(tk::Point local) const;
    void select_tab_(int tab);
    void paint_tab_(tk::PaintCtx&, int tab, tk::Rect row);
    tk::DragVisual build_drag_visual_(const SpacePickerEntry& entry);
    // Tab slot (1..n) under a world y; Home keeps slot 0.
    int slot_at_world_y_(float world_y) const;
    int tab_of_(const std::string& space_id) const;
    void rebuild_targets_();
    // Positions the keyboard targets from tab_rect_() (no LayoutCtx needed).
    void update_targets_();
    void paint_edge_fades_(tk::PaintCtx&);
    // Auto-scroll while a reorder drag hovers near the top/bottom edge.
    void autoscroll_for_drag_(float local_y);
    std::string tab_name_(int tab) const;

    std::vector<SpacePickerEntry> entries_;
    std::string active_;
    ImageProvider image_provider_;

    int hover_ = -1;
    int pressed_ = -1;
    // Reordering uses tk's in-app drag-and-drop: a press on a space tab
    // arms the tracker, crossing the threshold starts Host::begin_drag(), and
    // this view is also the drop target. drop_slot_ is the insertion slot
    // while a "pinned_space_id" drag is over it; drag_from_ the dragged tab.
    tk::DragGestureTracker drag_tracker_;
    int drop_slot_ = -1;
    int drag_from_ = -1;
    // Cached from the latest paint(), for rendering the drag ghost.
    tk::CanvasFactory* factory_ = nullptr;
    const tk::Theme* theme_ = nullptr;
    float scale_factor_ = 1.0f;
    tk::IconCache home_icon_;
    PopupMenu* menu_ = nullptr;                // owned via children()
    std::vector<tk::KeyboardTarget*> targets_; // owned via children()
};

} // namespace tesseract::views
