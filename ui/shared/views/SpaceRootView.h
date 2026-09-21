#pragma once

// SpaceRootView — full-panel widget shown in the chat area for a Matrix space.
// Mirrors RoomPreviewView's centred summary card, but describes the joined
// space and its child counts rather than offering a join action.

#include "RoomSettingsView.h"
#include "SpaceAddRoomList.h"
#include "SpaceChildRoomGrid.h"

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/scroll_view.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <optional>
#include <string>

namespace tesseract::views
{

class SpaceRootView : public tk::Widget
{
public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc)>;

protected:
    // host() is nullable: when null, settings_view_'s name field is simply
    // not constructed — lets tests that don't care about the native field
    // default-construct without a Host.
    SpaceRootView();
    TK_WIDGET_FACTORY_FRIEND(SpaceRootView)

public:
    ~SpaceRootView() override = default;

    void set_space(const tesseract::RoomInfo& space,
                   std::size_t joined_children,
                   std::size_t unjoined_children);
    void clear();
    void set_avatar_provider(AvatarProvider p);

    // Shell should kick an avatar fetch on miss and repaint.
    std::function<void(const std::string& mxc)> on_avatar_needed;
    // Fired when the user clicks an autolinked URL/matrix.to link inside the
    // topic text (see TopicLinkLabel). The shell decides whether it's a
    // Matrix link (open_matrix_link) or an ordinary URL (open in browser) —
    // mirrors ShellBase::setup_link_clicked_'s identical RoomView wiring.
    std::function<void(std::string url)> on_link_clicked;
    // Fired whenever the hovered link inside the topic changes (including
    // to/from empty) so the shell can swap the native cursor — mirrors
    // RoomView/RoomInfoPanel's identical on_link_hovered mechanism
    // (cursor-setting is platform-specific, wired per shell).
    std::function<void(std::string url)> on_link_hovered;

    // ── Room management section (add_list_ / child_grid_) ─────────────────
    // SpaceRootView does no data fetching of its own for this section — it
    // only owns the two widgets and forwards data/callbacks, exactly like
    // it already does for its own avatar (on_avatar_needed above).
    using RoomsProvider = SpaceAddRoomList::RoomsProvider;

    void set_candidate_rooms_provider(RoomsProvider p);
    void set_children(std::vector<SpaceChildRoomGrid::ChildRoomEntry> children);
    // Gates both the add and remove directions — see ShellBase's
    // can_edit_space_children, the single Matrix permission both share.
    void set_can_manage_children(bool v);

    // Forwarded from add_list_/child_grid_, carrying the current space's id
    // so the shell's mutation call sites don't need to track it separately.
    std::function<void(std::string space_id, std::string room_id)> on_add_room_to_space;
    std::function<void(std::string space_id, std::string room_id)> on_remove_room_from_space;
    std::function<void(std::string room_id)> on_child_summary_needed;
    std::function<void(const tesseract::RoomInfo&)> on_room_avatar_needed;

    // Owned settings overlay opened via the wrench icon (top-left, mirrors
    // RoomInfoPanel's). Replaces this view's own summary content entirely
    // while open — see arrange()/paint().
    RoomSettingsView* settings_view() const { return settings_view_; }

    // Fired when settings_view_'s own layout-affecting state changes (open/
    // close, tab switches) so the shell can relayout native overlays.
    std::function<void()> on_layout_changed;
    // Fired once the wrench opens settings_view_, and once more from its
    // avatar-upload request — both carry the space's room id so the shell's
    // existing per-room-id settings plumbing (permission gating,
    // ShellBase::apply_room_settings_, avatar upload staging) can be reused
    // unchanged for spaces.
    std::function<void(std::string space_id)> on_settings_opened;
    std::function<void(std::string space_id)> on_settings_avatar_upload_requested;
    // Fired when the user clicks the Room ID row in settings_view_; the
    // shell performs the actual clipboard write (mirrors RoomSettingsView's
    // own on_copy_to_clipboard, forwarded through unchanged).
    std::function<void(std::string)> on_copy_to_clipboard;

    // Fired when the user clicks the leave button (top-right, mirrors the
    // settings wrench's top-left placement); carries the space's room id so
    // the shell's existing generic confirm_leave_room_ chain can be reused
    // unchanged for spaces.
    std::function<void(std::string space_id)> on_leave_space;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;

    // Claims any press landing on this view's own empty background (the
    // gaps around/between the avatar/topic block and the room-management
    // section) rather than leaving it unclaimed. Without this, an unclaimed
    // press bubbles all the way up to MainAppWidget's sidebar-resize
    // wrapper, whose own on_pointer_down fallback claims *any* unclaimed
    // press unconditionally (it assumes every view fills its own space with
    // real interactive widgets) — starting a spurious sidebar-resize drag
    // from a click anywhere on this view's background. A no-op consume, not
    // an interactive claim.
    bool on_pointer_down(tk::Point local) override;

private:
    std::optional<tesseract::RoomInfo> space_;
    std::size_t joined_children_ = 0;
    std::size_t unjoined_children_ = 0;
    AvatarProvider avatar_provider_;

    tk::Button*       settings_btn_  = nullptr;
    tk::IconCache     settings_icon_;
    RoomSettingsView* settings_view_ = nullptr;

    tk::Button*       leave_btn_  = nullptr;

    SpaceAddRoomList*   add_list_   = nullptr;
    SpaceChildRoomGrid* child_grid_ = nullptr;
    // Topic/description: a real scrollable child (tk::ScrollView wrapping a
    // TopicLinkLabel), not a manually clipped/height-capped TextLayout — an
    // arbitrarily long topic scrolls instead of overflowing into (or
    // getting truncated against) the room-management section below/beside
    // it. topic_label_'s text is set once in set_space(), not every paint().
    class TopicLinkLabel;
    tk::ScrollView* topic_scroll_ = nullptr;
    TopicLinkLabel* topic_label_  = nullptr;
    // Whether the room-management section (add_list_/child_grid_ + the
    // explanatory label) is shown at all — set via set_can_manage_children.
    // The whole section is hidden, not just individually disabled, when the
    // user lacks permission to send m.space.child in this space; the
    // widgets' own can_manage_ gating stays as defense-in-depth underneath.
    bool can_manage_children_ = false;
    // Change-detected so add_list_/child_grid_ are only re-arranged when
    // their section actually moves/resizes, not on every paint() call (this
    // view recomputes its own text layout positions every frame, but a real
    // child widget's arrange() is not free the way redrawing cached text
    // is).
    tk::Rect last_left_rect_{-1, -1, -1, -1};
    tk::Rect last_right_rect_{-1, -1, -1, -1};

    mutable std::unique_ptr<tk::TextLayout> name_layout_;
    mutable std::unique_ptr<tk::TextLayout> alias_layout_;
    mutable std::unique_ptr<tk::TextLayout> meta_layout_;
    mutable std::unique_ptr<tk::TextLayout> hint_layout_;
    mutable tk::CanvasFactory* factory_seen_ = nullptr;

    static constexpr float kAvatarD = 72.0f;
    static constexpr float kContentW = 340.0f;
    static constexpr float kPadY = 32.0f;
    static constexpr float kGap = 12.0f;

    // Top row (avatar/name/alias column + topic column): fixed-height
    // estimates, not measured from actual text, so the row's total height
    // never depends on font metrics being available yet.
    static constexpr float kNameH = 28.0f;
    static constexpr float kAliasH = 18.0f;
    static constexpr float kTopRowMinH = 150.0f;

    // Room-management section (add_list_ / child_grid_), carved out of the
    // vertical space between the top row and the room-count row.
    static constexpr float kTopicMaxH = 80.0f;       // fixed cap, stacked-narrow layout only
    static constexpr float kSectionMinH = 180.0f;     // never shrinks below this
    static constexpr float kSectionPadX = 24.0f;      // inset from bounds_ edges
    static constexpr float kSectionGap = 16.0f;       // between list and grid
    static constexpr float kSectionLabelGap = 8.0f;   // between label and lists
    static constexpr float kStackBreakpoint = 560.0f; // side-by-side vs. stacked
    static constexpr float kLeftColumnW = 280.0f;      // candidate list width

    void reset_layouts_();
    std::string child_count_label_() const;
};

} // namespace tesseract::views
