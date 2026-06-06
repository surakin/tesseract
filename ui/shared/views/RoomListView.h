#pragma once

// Shared room list. Renders a `std::vector<tesseract::RoomInfo>` as six
// collapsible sections — Invitations / Favorites / Direct Messages / Rooms /
// Spaces / Inactive — each showing avatar + name + last-message preview +
// unread badge per row. The Invitations section is fed from a separate
// `std::vector<tesseract::InviteInfo>` pointer and is hidden when null or empty.
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

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class RoomListView : public tk::Widget
{
public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;
    using StickerProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;
    using PresenceProvider =
        std::function<tesseract::PresenceState(const std::string& user_id)>;

    RoomListView();
    ~RoomListView() override; // out-of-line — Adapter is opaque here

    // Replace the room list. Re-measures, re-applies the search filter,
    // re-paints, and preserves selection by room ID across the swap.
    void set_rooms(std::vector<tesseract::RoomInfo> rooms);
    const std::vector<tesseract::RoomInfo>& rooms() const
    {
        return rooms_;
    }

    // Hook for retrieving decoded avatar images. The provider returns
    // null when the image isn't ready yet, in which case the row falls
    // back to an initials disc.
    void set_avatar_provider(AvatarProvider p);

    void set_sticker_provider(StickerProvider p);
    void set_presence_provider(PresenceProvider p);

    // Selection by room ID. Stays stable across filter changes — when
    // the filter excludes the selected room, the index is cleared on the
    // inner ListView but the chosen room ID is remembered; clearing the
    // filter restores the selection.
    void set_selected_room(const std::string& room_id);
    std::string selected_room_id() const;

    // Position among all currently visible room rows (headers excluded),
    // or -1 when nothing is selected. Preserved across search / collapse
    // changes as long as the selected room remains visible.
    int selected_index() const;

    // Search header: host-overlaid NativeTextField. The view exposes the
    // rect the field should occupy plus a visibility bit; layout decides
    // them in `arrange()` based on the inner list's content height.
    tk::Rect search_field_rect() const;
    bool search_field_visible() const;

    // Debounce delay every shell should wait after the last keystroke
    // before pushing the typed query into set_search_text(). Kept here so
    // all four platform shells share one source of truth.
    static constexpr int kSearchDebounceMs = 250;

    // Host pipes its NativeTextField's text changes back into the view.
    void set_search_text(std::string q);
    const std::string& search_text() const
    {
        return search_text_;
    }

    // Room IDs of all room rows currently visible in the viewport.
    // Headers are excluded. Empty when no rows are visible or laid out.
    std::vector<std::string> visible_room_ids() const;

    // Fired whenever the room list is scrolled by user input.
    std::function<void()> on_scroll;

    // Fires when the user clicks a room row (selection moves to that row).
    std::function<void(const std::string& /*room_id*/)> on_room_selected;

    // Fires from paint for a room row whose avatar isn't cached yet, so the
    // host can lazily fetch it. Only visible (painted) rows trigger this, so
    // avatars in collapsed or off-screen sections are never requested.
    std::function<void(const tesseract::RoomInfo& /*room*/)> on_room_avatar_needed;

    // Fires when the user clicks the × clear button in the search header.
    // The host should clear the NativeTextField text and reset the search.
    std::function<void()> on_search_clear;

    // Fires when the user clicks the + button in the search header.
    // The host should open the JoinRoomView dialog.
    std::function<void()> on_join_room_requested;

    // Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    // The pinned sticky header overlays the top of the inner ListView child;
    // these intercept pointer events over it before the child (which is tried
    // first by the default dispatch) can claim them for a room row beneath.
    tk::Widget* dispatch_pointer_down(tk::Point world) override;
    tk::Widget* dispatch_pointer_move(tk::Point world, bool* dirty) override;
    void on_pointer_leave() override;

    // Section indices (matches array positions throughout the class).
    // kSecInvites is fed from invites_; all others are fed from rooms_.
    static constexpr int kSecInvites   = 0;
    static constexpr int kSecFavorites = 1;
    static constexpr int kSecDMs       = 2;
    static constexpr int kSecRooms     = 3;
    static constexpr int kSecSpaces    = 4;
    static constexpr int kSecInactive  = 5;
    static constexpr int kNumSections  = 6;

    // kSectionTitles[kSecInvites] is a placeholder; the actual header label is
    // "Invitations (N)" and is constructed dynamically in paint_header.
    static constexpr const char* kSectionTitles[kNumSections] = {
        "Invitations", "Favorites", "Direct Messages", "Rooms", "Spaces",
        "Inactive"};

    // Programmatically collapse or expand a section (e.g. to restore saved
    // state on launch). No-op if section is out of range or already in the
    // requested state.
    void set_section_collapsed(int section, bool collapsed);

    // Fired whenever the user toggles a section header. section is one of
    // the kSec* constants; collapsed is the new state after the toggle.
    std::function<void(int section, bool collapsed)> on_section_toggled;

    // Invitations data source. The pointer is not owned; the caller must
    // keep it alive for the lifetime of the view (or until replaced / cleared).
    // Passing nullptr hides the Invitations section entirely.
    void set_invites(const std::vector<tesseract::InviteInfo>* invites);

    // Fires when the user clicks an invite row.
    std::function<void(const std::string& /*room_id*/)> on_invite_selected;

    // Re-run section classification (e.g. after a settings change) and repaint.
    void refresh();

private:
    class Adapter;

    // Flat item list fed to the inner ListView. Each item is either a
    // collapsible section header, a pointer into section_rooms_[s], or an
    // invite row (index into *invites_).
    struct Item
    {
        enum class Kind : uint8_t
        {
            Header,
            Room,
            Invite
        } kind = Kind::Room;
        int section  = 0; // which section (0–5)
        int room_idx = 0; // index within section_rooms_[section] (Room);
                          // index within *invites_ (Invite)
    };

    // Rebuild section_rooms_[kNumSections] from rooms_ + search filter, then
    // rebuild items_ from those buckets + collapsed_[] state.
    void rebuild_items();

    // Flat items_ index of the room/space with `id`, or -1 if it isn't a
    // visible row (filtered out, or inside a collapsed section).
    int item_index_for_room_(const std::string& id) const;

    // The unread "active" room/space with the most recent activity, or null.
    // Active = notification_count > 0, not low-priority, in the Favorites/DMs/
    // Rooms/Spaces sections (excludes Inactive). Spaces carry the aggregate of
    // their children (see ShellBase::apply_space_child_counts_). Used to decide
    // which row to auto-scroll into view when new messages arrive.
    const tesseract::RoomInfo* most_recent_unread_active_() const;

    // Auto-scroll the most-recent unread row into view when newer than what we
    // last scrolled to. Gated on Settings::autoscroll_unread_rooms.
    void autoscroll_to_unread_();

    float search_header_h() const;

    // Collapse/expand a section and refresh the flat item list + selection.
    // Shared by the real header click and the sticky-header click.
    void toggle_section_collapsed_(int section);

    // Sticky (pinned) section header: which header to draw at the top of the
    // list while its section's rooms occupy the viewport top, and where.
    struct StickyHeader
    {
        bool  show        = false;
        int   header_item = -1; // index into items_
        int   section     = -1;
        float world_y     = 0.f; // world-space top of the pinned header
    };
    StickyHeader sticky_header_() const;

    // True when `world` (root-surface coords) falls within the visible band of
    // the pinned header described by `s`.
    bool sticky_band_contains_(const StickyHeader& s, tk::Point world) const;

    std::vector<tesseract::RoomInfo> rooms_;
    // Pending invitations. Not owned; null when no account is signed in or
    // when the data has not yet been wired up.
    const std::vector<tesseract::InviteInfo>* invites_ = nullptr;
    // Per-section filtered room pointers (into rooms_); rebuilt by
    // rebuild_items(). kSecInvites slot is always empty (invites are separate).
    std::vector<const tesseract::RoomInfo*> section_rooms_[kNumSections];
    // Flat item list the adapter iterates over.
    std::vector<Item> items_;
    // Collapsed state per section (false = expanded by default).
    bool collapsed_[kNumSections] = {};

    std::string search_text_;
    AvatarProvider avatar_provider_;
    StickerProvider sticker_provider_;
    PresenceProvider presence_provider_;

    std::unique_ptr<Adapter> adapter_;
    tk::ListView* list_ = nullptr;
    tk::Rect search_field_rect_{};
    tk::Rect search_clear_rect_{};
    tk::Rect join_room_rect_{};
    bool search_field_visible_ = false;
    bool press_search_clear_ = false;
    bool press_join_room_ = false;
    // Sticky-header interaction: section pressed on the pinned header (-1 =
    // none), and whether the pointer is currently over the pinned header.
    int  press_sticky_section_ = -1;
    bool sticky_hovered_ = false;

    // Last-known selected room ID, preserved across filter/collapse changes
    // when the selection is temporarily hidden.
    std::string selected_room_id_cache_;

    // Most recent activity timestamp we last auto-scrolled an unread room to.
    // Only strictly-newer activity re-triggers a scroll, so unrelated room-list
    // updates don't yank the user's scroll position. Starts at 0 so the first
    // load with existing unread rooms scrolls the most recent into view.
    std::uint64_t last_unread_scroll_ts_ = 0;
};

// Pure room→section classifier. Favorites and Spaces are never grouped; when
// `group_inactive` is true, DMs and Rooms whose `last_activity_ts` is more than
// `threshold_days` before `now_ms` go to kSecInactive. `last_activity_ts == 0`
// (timestamp not yet available from sync) is treated as active.
int classify_room_section(const tesseract::RoomInfo& r, bool group_inactive,
                          int threshold_days, std::uint64_t now_ms);

} // namespace tesseract::views
