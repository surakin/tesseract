#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "app/Launch.h"
#include <tesseract/crash_handler.h>
#include <tesseract/version.h>
#include "app/MediaPlaybackHub.h"
#include "app/RoomPane.h"
#include "app/RoomWindowBase.h"
#include "app/SearchBackend.h"
#include "app/SlashCommands.h"
#include "app/shell_helpers.h"
#include "app/UnreadPrefetch.h"
#include "app/media_preview_policy.h"
#include "tk/blurhash.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/text_util.h"
#include "tk/theme.h"
#include "views/AddRoomView.h"
#include "views/CreateRoomView.h"
#include "views/EncryptionSetupOverlay.h"
#include "views/JoinRoomView.h"
#include "views/ConfirmDialog.h"
#include "views/MainAppWidget.h"
#include "views/RoomListView.h"
#include "views/InviteDialog.h"
#include "views/text_util.h"
#include "views/RoomSearchBar.h"
#include "views/SettingsView.h"
#include "views/RoomView.h"
#include "views/UserInfo.h"
#include "tk/image_sniff.h"
#include "views/html_spans.h"
#include "views/image_pack_order.h"
#include "views/map_tiles.h"
#include "views/pronoun_utils.h"
#include "views/thread_unread.h"
#include <tesseract/paths.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/secret_store.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace tesseract
{

// ── Tab management ─────────────────────────────────────────────────────────

namespace
{

// Works with any vector whose element has a .room_id field.
template <typename Tab>
size_t find_tab_(const std::vector<Tab>& tabs, const std::string& room_id)
{
    for (size_t i = 0; i < tabs.size(); ++i)
    {
        if (tabs[i].room_id == room_id)
        {
            return i;
        }
    }
    return SIZE_MAX;
}

} // namespace

void ShellBase::tab_open_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    // Already open in a pop-out window: raise it instead of re-opening here.
    if (focus_secondary_window_(room_id))
    {
        return;
    }
    if (main_room_pane_)
        main_room_pane_->save_compose_draft_(current_room_id_);
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX)
    {
        if (active_tab_idx_ < tabs_.size())
        {
            tabs_[active_tab_idx_].scroll_offset =
                get_message_scroll_fraction_();
        }
        active_tab_idx_ = existing;
        {
            if (main_room_pane_)
            {
                auto _tt = compute_thread_transition_(
                    main_room_pane_->thread_panel(),
                    main_room_pane_->thread_panel_prev(),
                    main_room_pane_->thread_root(),
                    ThreadTrigger::RoomSwitch, {});
                main_room_pane_->apply_thread_transition_(_tt);
            }
        }
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        if (main_room_pane_)
            main_room_pane_->retarget(current_room_id_);
        after_active_room_changed_();
        on_tab_state_changed_ui_();
        return;
    }
    if (!tabs_.empty())
    {
        tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
    }
    // Bootstrap: wrap current_room_id_ as first tab if tabs_ is empty.
    if (tabs_.empty() && !current_room_id_.empty())
    {
        tabs_.push_back({current_room_id_, 0.f});
    }
    tabs_.push_back({room_id, 0.f});
    active_tab_idx_ = tabs_.size() - 1;
    {
        if (main_room_pane_)
        {
            auto _tt = compute_thread_transition_(
                main_room_pane_->thread_panel(),
                main_room_pane_->thread_panel_prev(),
                main_room_pane_->thread_root(),
                ThreadTrigger::RoomSwitch, {});
            main_room_pane_->apply_thread_transition_(_tt);
        }
    }
    current_room_id_ = room_id;
    if (main_room_pane_)
        main_room_pane_->retarget(current_room_id_);
    after_active_room_changed_();
    on_tab_state_changed_ui_();
}

void ShellBase::tab_select_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    // Already open in a pop-out window: raise it and leave the main app as-is.
    if (focus_secondary_window_(room_id))
    {
        return;
    }
    // Dismiss any visible InviteCard so the chat panel shows the room view.
    if (main_app_)
        main_app_->show_room();
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX)
    {
        if (existing == active_tab_idx_)
            return;
        if (main_room_pane_)
            main_room_pane_->save_compose_draft_(current_room_id_);
        if (active_tab_idx_ < tabs_.size())
        {
            tabs_[active_tab_idx_].scroll_offset =
                get_message_scroll_fraction_();
        }
        active_tab_idx_ = existing;
        {
            if (main_room_pane_)
            {
                auto _tt = compute_thread_transition_(
                    main_room_pane_->thread_panel(),
                    main_room_pane_->thread_panel_prev(),
                    main_room_pane_->thread_root(),
                    ThreadTrigger::RoomSwitch, {});
                main_room_pane_->apply_thread_transition_(_tt);
            }
        }
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        if (main_room_pane_)
            main_room_pane_->retarget(current_room_id_);
        after_active_room_changed_();
        on_tab_state_changed_ui_();
        return;
    }
    if (main_room_pane_)
        main_room_pane_->save_compose_draft_(current_room_id_);
    if (tabs_.empty())
    {
        tabs_.push_back({room_id, 0.f});
    }
    else
    {
        tabs_[active_tab_idx_] = {room_id, 0.f};
    }
    {
        if (main_room_pane_)
        {
            auto _tt = compute_thread_transition_(
                main_room_pane_->thread_panel(),
                main_room_pane_->thread_panel_prev(),
                main_room_pane_->thread_root(),
                ThreadTrigger::RoomSwitch, {});
            main_room_pane_->apply_thread_transition_(_tt);
        }
    }
    current_room_id_ = room_id;
    if (main_room_pane_)
        main_room_pane_->retarget(current_room_id_);
    after_active_room_changed_();
    on_tab_state_changed_ui_();
}

void ShellBase::tab_navigate_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    // Already open in a pop-out window: raise it instead of navigating here.
    if (focus_secondary_window_(room_id))
    {
        return;
    }
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX)
    {
        if (main_room_pane_)
            main_room_pane_->save_compose_draft_(current_room_id_);
        if (active_tab_idx_ < tabs_.size())
        {
            tabs_[active_tab_idx_].scroll_offset =
                get_message_scroll_fraction_();
        }
        active_tab_idx_ = existing;
        {
            if (main_room_pane_)
            {
                auto _tt = compute_thread_transition_(
                    main_room_pane_->thread_panel(),
                    main_room_pane_->thread_panel_prev(),
                    main_room_pane_->thread_root(),
                    ThreadTrigger::RoomSwitch, {});
                main_room_pane_->apply_thread_transition_(_tt);
            }
        }
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        if (main_room_pane_)
            main_room_pane_->retarget(current_room_id_);
        after_active_room_changed_();
        on_tab_state_changed_ui_();
        return;
    }
    if (tabs_.size() <= 1)
    {
        tab_select_room(room_id);
    }
    else
    {
        tab_open_room(room_id);
    }
}

void ShellBase::tab_close(const std::string& room_id)
{
    size_t idx = find_tab_(tabs_, room_id);
    if (idx == SIZE_MAX)
    {
        return;
    }

    if (tabs_.size() <= 1)
    {
        // Closing the only open tab: deselect to the "no active room" empty
        // state (RoomView falls back to showing BrandView) rather than
        // silently no-op'ing and leaving the room shown as if it were still
        // open — the same empty state already used on a fresh login, when
        // leaving the last-remaining tab's room, and during account switch
        // / SDK restart.
        if (main_room_pane_)
        {
            auto _tt = compute_thread_transition_(
                main_room_pane_->thread_panel(),
                main_room_pane_->thread_panel_prev(),
                main_room_pane_->thread_root(),
                ThreadTrigger::RoomSwitch, {});
            main_room_pane_->apply_thread_transition_(_tt);
        }
        if (main_room_pane_)
            main_room_pane_->save_compose_draft_(current_room_id_);
        current_room_id_.clear();
        tabs_.clear();
        active_tab_idx_ = 0;
        after_active_room_changed_();
        if (room_view_)
            room_view_->clear_room();
        request_relayout_();
        on_tab_state_changed_ui_();
        return;
    }

    const bool closing_active = (idx == active_tab_idx_);
    if (!closing_active)
    {
        tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
    }
    else
    {
        if (main_room_pane_)
        main_room_pane_->save_compose_draft_(current_room_id_);
    }
    size_t new_active = active_tab_idx_;
    if (closing_active)
    {
        new_active = (idx > 0) ? idx - 1 : 0;
    }
    else if (idx < active_tab_idx_)
    {
        --new_active;
    }

    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(idx));
    active_tab_idx_ = new_active;
    {
        if (main_room_pane_)
        {
            auto _tt = compute_thread_transition_(
                main_room_pane_->thread_panel(),
                main_room_pane_->thread_panel_prev(),
                main_room_pane_->thread_root(),
                ThreadTrigger::RoomSwitch, {});
            main_room_pane_->apply_thread_transition_(_tt);
        }
    }
    current_room_id_ = tabs_[active_tab_idx_].room_id;
    if (main_room_pane_)
        main_room_pane_->retarget(current_room_id_);
    after_active_room_changed_();
    on_tab_state_changed_ui_();
}

void ShellBase::tab_popout_room(const std::string& room_id)
{
    // Copy first: the callers pass a reference to a TabBar TabItem's room_id
    // string, and tab_close() below rebuilds the tab bar via
    // on_tab_state_changed_ui_(), freeing that string. Using the reference
    // afterwards would be a use-after-free (it manifested as the active room
    // popping out instead of the clicked one, and closing the active tab doing
    // nothing because the dangling read came back empty).
    const std::string id = room_id;
    if (id.empty())
    {
        return;
    }
    tab_close(id);                // deselects to empty state if this is the last tab
    open_room_in_new_window(id);  // raises an existing pop-out if present
}

bool ShellBase::room_open_in_tab(const std::string& room_id) const
{
    return find_tab_(tabs_, room_id) != SIZE_MAX;
}

bool ShellBase::room_open_in_window(const std::string& room_id) const
{
    return find_secondary_(room_id, active_account_ ? active_account_->user_id
                                                    : std::string{}) != nullptr;
}

bool ShellBase::try_restore_tab_session_(
    const std::vector<std::string>& room_ids,
    const std::string&              active_room_id)
{
    std::vector<TabState> new_tabs;
    for (const auto& id : room_ids)
    {
        for (const auto& r : rooms_)
        {
            if (r.id == id && !r.is_space)
            {
                new_tabs.push_back({id, 0.f});
                break;
            }
        }
    }
    if (new_tabs.empty())
        return false;

    tabs_          = std::move(new_tabs);
    active_tab_idx_ = 0;
    if (!active_room_id.empty())
    {
        for (size_t i = 0; i < tabs_.size(); ++i)
        {
            if (tabs_[i].room_id == active_room_id)
            {
                active_tab_idx_ = i;
                break;
            }
        }
    }
    {
        if (main_room_pane_)
        {
            auto _tt = compute_thread_transition_(
                main_room_pane_->thread_panel(),
                main_room_pane_->thread_panel_prev(),
                main_room_pane_->thread_root(),
                ThreadTrigger::RoomSwitch, {});
            main_room_pane_->apply_thread_transition_(_tt);
        }
    }
    current_room_id_ = tabs_[active_tab_idx_].room_id;
    if (main_room_pane_)
        main_room_pane_->retarget(current_room_id_);
    after_active_room_changed_();
    on_tab_state_changed_ui_();

    // Restore pop-out windows saved from the previous session. Only rooms
    // that are now in the room list are opened; rooms not yet known are kept
    // in pending_restore_popouts_ and retried on the next rooms update.
    if (!pending_restore_popouts_.empty())
    {
        std::vector<std::string> still_pending;
        for (const auto& id : pending_restore_popouts_)
        {
            bool known = std::any_of(rooms_.begin(), rooms_.end(),
                                     [&id](const RoomInfo& r)
                                     { return r.id == id; });
            if (known)
                open_room_in_new_window(id);
            else
                still_pending.push_back(id);
        }
        pending_restore_popouts_ = std::move(still_pending);
    }

    return true;
}

// compute_thread_transition_ is now a thin inline forwarder to
// ThreadPanelController::compute_transition (see ShellBase.h); the pure switch
// lives in ThreadPanelController.cpp.

// ── Thread panel public entry points ────────────────────────────────────
// The state machine itself (apply_thread_transition_, on_threads_button_
// clicked, on_thread_open/close_requested, on_thread_send(_reply)_requested)
// lives on RoomPane now — RoomView's thread-panel callbacks are wired
// directly to main_room_pane_ via RoomPane::wire_room_view_(). See
// RoomPane.cpp for the shared implementation (used by pop-out windows too).

void ShellBase::on_pin_requested(const std::string& event_id)
{
    if (!client_ || current_room_id_.empty() || event_id.empty())
        return;
    auto sess = active_account_;
    auto rid = current_room_id_;
    auto eid = event_id;
    run_async_mut_([sess, rid, eid]() mutable {
        if (!sess || !sess->client) return;
        auto r = sess->client->pin_event(rid, eid);
        if (!r.ok)
        {
            // TODO: surface this via a transient status mechanism once one exists
            std::fprintf(stderr, "[pin] pin failed for %s in %s: %s\n",
                         eid.c_str(), rid.c_str(), r.message.c_str());
        }
    });
}

void ShellBase::on_unpin_requested(const std::string& event_id)
{
    if (!client_ || current_room_id_.empty() || event_id.empty())
        return;
    auto r = client_->unpin_event(current_room_id_, event_id);
    if (!r.ok)
    {
        // TODO: surface this via a transient status mechanism once one exists
        std::fprintf(stderr, "[pin] unpin failed for %s in %s: %s\n",
                     event_id.c_str(), current_room_id_.c_str(),
                     r.message.c_str());
    }
}

// Compute and apply calls-button visibility for one room header:
// requires server support, a non-bridged room, and either the current
// user's PL permitting org.matrix.msc3401.call.member, or the user
// already being in a call for that room (so they can still hang up if
// their permission was revoked mid-call). Called for room_view_ and
// every secondary window's header whenever room state, server info, or
// the active room changes.
void ShellBase::update_call_btn_visibility_(views::RoomHeader* header,
                                             const std::string& room_id)
{
    if (!header)
        return;
    const auto* r = room_by_id_(room_id);
    const bool room_is_bridged = r && r->is_bridged && !r->bridge_overridden;
    const bool in_call_room =
        call_session_ != nullptr && call_session_->room_id() == room_id;
    const bool can_call = client_ && client_->can_start_call_in_room(room_id);
    header->set_show_call_btn(server_info_.supports_calls && !room_is_bridged &&
                               (can_call || in_call_room));
}

void ShellBase::refresh_pinned_for_current_room_()
{
    // The actual lookup/refresh logic lives on RoomPane now (shared with
    // every pop-out's own on_room_info_updated path) — this stays a thin
    // wrapper since callers (push_rooms_, after_active_room_changed_,
    // handle_timeline_reset_ui_) all operate in main-window/
    // current_room_id_ context, and main_room_pane_ is always retargeted to
    // stay in sync with current_room_id_.
    if (main_room_pane_)
        main_room_pane_->refresh_pinned_for_current_room_();
}

void ShellBase::navigate_history_back()
{
    if (room_nav_history_.empty() || room_nav_history_cursor_ == 0)
        return;
    --room_nav_history_cursor_;
    room_nav_in_progress_ = true;
    navigate_to_room_(room_nav_history_[room_nav_history_cursor_]);
    room_nav_in_progress_ = false;
}

void ShellBase::navigate_history_forward()
{
    if (room_nav_history_.empty() || room_nav_history_cursor_ + 1 >= room_nav_history_.size())
        return;
    ++room_nav_history_cursor_;
    room_nav_in_progress_ = true;
    navigate_to_room_(room_nav_history_[room_nav_history_cursor_]);
    room_nav_in_progress_ = false;
}

void ShellBase::after_active_room_changed_()
{
    // Cancel any in-room search before changing rooms so stale results can't
    // bleed into the new room. The RoomView UI side (bar close + highlights)
    // is handled by RoomView::set_room() when room_changed is true.
    in_room_search_clear_();

    // Keep the current room's own MSC2545 image pack fetched (see
    // Client::set_active_room's doc comment) and re-order the emoji/sticker
    // pickers' tabs (personal / current room / parent spaces / subscribed
    // rooms) for the new room right away, using whatever's already cached —
    // a second refresh follows once the pack rebuild this may trigger
    // resolves, via the existing on_image_packs_updated path. Also keep
    // every ancestor space's own pack fetched, so the pickers can surface
    // packs from any Space the new room is (nested-)in.
    if (client_)
    {
        client_->set_active_room(current_room_id_);
        for (const auto& space_id : parent_spaces_for_room_(current_room_id_))
            client_->set_active_room(space_id);
    }
    refresh_pickers_packs_();

    // The room media gallery is scoped to a single room — current_room_id_
    // is already the room we're switching TO at this point, so leaving it
    // open here would show (and keep paginating/fetching for) the room the
    // user just left. close() fires on_close, which runs
    // RoomPane::close_room_media_view_()'s cleanup (cancel its media group,
    // clear its own media_view_room_id_, lift the main timeline's relayout
    // suppression).
    if (main_app_ && main_app_->room_media_view() &&
        main_app_->room_media_view()->is_open() && main_room_pane_ &&
        main_room_pane_->media_view_room_id() != current_room_id_)
    {
        main_app_->room_media_view()->close();
    }

    // Navigation history for Alt+Left / Alt+Right. Must be first so it
    // executes in tests (no client_) and before any early return.
    if (!current_room_id_.empty() && !room_nav_in_progress_)
    {
        // Truncate any forward entries the user had navigated back into.
        if (!room_nav_history_.empty())
        {
            room_nav_history_.erase(
                room_nav_history_.begin() +
                    static_cast<std::ptrdiff_t>(room_nav_history_cursor_) + 1,
                room_nav_history_.end());
        }
        room_nav_history_.push_back(current_room_id_);
        room_nav_history_cursor_ = room_nav_history_.size() - 1;
        // Evict oldest entry when cap is reached.
        if (room_nav_history_.size() > kNavHistoryMax)
        {
            room_nav_history_.erase(room_nav_history_.begin());
            if (room_nav_history_cursor_ > 0)
                --room_nav_history_cursor_;
        }
    }

    // Keep the room-list highlight in sync for every navigation path, not
    // just row clicks (which the list already self-highlights immediately) —
    // covers invite-accept, permalink/matrix-link navigation, and the Add
    // Room dialog's post-join/post-create navigation, none of which touched
    // this otherwise.
    if (main_app_ && main_app_->room_list_view())
        main_app_->room_list_view()->set_selected_room(current_room_id_);

    if (const auto* cur_room = room_by_id_(current_room_id_);
        cur_room && cur_room->is_space)
    {
        show_space_root_(current_room_id_);
        return;
    }

    // Clear the room we just left from the timeline immediately and show a clean
    // loading view (a centered spinner only if the new room's snapshot takes
    // longer than the delay). The populated reset → set_messages cancels it. This
    // replaces the old "flash to empty then repopulate": the previous room's rows
    // never linger under the new room's header.
    if (room_view_)
        if (auto* ml = room_view_->message_list())
            ml->begin_switch_loading();

    // Warm-subscription LRU: mark this room most-recently-active and unsubscribe
    // any room that has aged out of the warm window (kept rooms = active + open
    // tabs + pop-out-pinned + the newest kWarmRoomsMax others). This bounds the
    // otherwise-unbounded growth of live timelines / sliding-sync subscriptions
    // for a long browsing session; rooms still warm are reused on return without
    // a rebuild (SDK subscribe_room reuse).
    touch_visited_room_(current_room_id_);
    prune_warm_subscriptions_();

    // Per-room lazy-media tracking: reset so the new room's rows are all
    // eligible for on-demand fetch via on_visible_rows_changed_.
    media_prepped_event_ids_.clear();

    // Drop the room we just left: cancel its still-pending timeline media
    // downloads (full-size images, thumbnails) so the room we're switching to
    // gets the semaphore slots instead of queueing behind the old room's flood.
    // Done before the early-return so closing the last tab also cancels.
    const std::uint64_t new_group = media_group_for_room_(current_room_id_);
    if (active_media_group_ != 0 && active_media_group_ != new_group)
        cancel_media_group_(active_media_group_);
    active_media_group_ = new_group;

    // Schedule a debounced save of the new layout (active room + open tabs).
    // Placed before the early-return below so closing the last tab — which
    // clears current_room_id_ and tabs_ before calling this function — still
    // persists that "nothing open" state, not just genuine room switches.
    schedule_account_data_save_();

    if (!client_ || current_room_id_.empty())
        return;

    // Record the visit for the quick switcher's "Recent" strip — move this
    // room to the front of the MRU list (visit order), de-duplicating and
    // capping the length.
    {
        auto& v = recent_room_ids_;
        v.erase(std::remove(v.begin(), v.end(), current_room_id_), v.end());
        v.insert(v.begin(), current_room_id_);
        if (v.size() > kRecentRoomsMax)
            v.resize(kRecentRoomsMax);
    }
    if (const auto* room = room_by_id_(current_room_id_))
        on_recent_room_visited_(*room);

    if (room_view_ && room_view_->header())
    {
        const bool in_call_room = call_session_ != nullptr &&
                                  call_session_->room_id() == current_room_id_;
        update_call_btn_visibility_(room_view_->header(), current_room_id_);
        room_view_->header()->set_call_active(in_call_room);
        refresh_call_banners_();
        // Hide the docked panel when viewing a different room; show it again
        // when the user returns. Floating/Popout: call_panel() is nullptr,
        // so this block is a no-op for those modes.
        if (auto* panel = room_view_->call_panel())
        {
            const bool was_visible = panel->visible();
            panel->set_visible(in_call_room);
            if (was_visible != in_call_room)
                schedule_relayout_();
        }
    }

    // Auto-join/leave/float/restore the call tied to call-room navigation.
    handle_call_room_navigation_();

    // Each new room starts with an unknown thread history — allow pagination.
    if (main_room_pane_)
        main_room_pane_->reset_thread_backfill();
    // Keep an always-on background subscription on the active room so the
    // threads button reflects whether the room contains threads — long before
    // the user opens the panel. subscribe_room_threads is idempotent (aborts
    // any prior subscription for this room) and the RoomSwitch transition
    // already released the outgoing room's handle.
    client_->subscribe_room_threads(current_room_id_);
    // Seed the button from the local snapshot. On a first visit the service
    // hasn't paginated yet so this is empty, which correctly hides the
    // button; on_threads_updated will reveal it once roots are discovered.
    apply_threads_list_(client_->list_room_threads(current_room_id_));
    // Refresh the pinned-events banner immediately on room switch so the new
    // room's pin state appears without waiting for the next sync tick.
    refresh_pinned_for_current_room_();
    // MSC4278: prefetch the room's media-preview override + join rule so the
    // timeline gating (and Private-mode evaluation) is ready before the
    // timeline reset builds rows.
    ensure_room_preview_override_(current_room_id_);
}

// Drive the SDK subscription for a room switch. subscribe_room runs on the
// single-thread mut pool (fast for a warm room — the SDK reuses the live
// timeline; either way it emits the reset that repopulates the just-cleared
// view and cancels the loading state). The initial back-pagination then runs
// on the SHARED pool so its blocking network round-trip never holds the one
// mut thread — otherwise the next switch's subscribe/reset would queue behind
// it and the loading spinner would flash on rapid A<->B switching. subscribe
// is dispatched on every switch (not gated by in_flight) so the reset always
// arrives; only the network paginate is deduplicated per room. Shared by all
// four shells. `visible_ids` seeds the background unread prefetch.
void ShellBase::start_room_subscription_(const std::string&       room_id,
                                         std::vector<std::string> visible_ids)
{
    if (room_id.empty() || !active_account_ || !active_account_->client)
        return;
    // Capture the OWNING session (not the raw client_): the workers run on
    // mut_pool_ / pool_ where a concurrent account switch/logout can free the
    // raw client_ pointer. Holding `sess` keeps the Client alive for the call —
    // same pattern as the unread-prefetch worker above.
    auto sess = active_account_;

    // Subscribe on the single-thread mut pool. Dispatched on EVERY switch
    // (intentionally not gated by in_flight): subscribe_room emits the
    // timeline reset that repopulates the view and cancels the loading
    // spinner, and for a warm room the SDK reuses the live timeline so this
    // is cheap.
    run_async_mut_(
        [this, sess, room_id, visible_ids = std::move(visible_ids)]() mutable
        {
            if (!sess->client)
                return;
            auto        res = sess->client->subscribe_room(room_id);
            const bool  ok  = res.ok;
            std::string msg = res.message;
            post_to_ui_(
                [this, sess, room_id, ok, msg = std::move(msg),
                 visible_ids = std::move(visible_ids)]() mutable
                {
                    if (!ok)
                    {
                        show_status_message_(tk::trf(tk::tr("Subscribe failed: {0}"), {msg}));
                        // No timeline reset will arrive, so leave the loading
                        // state — otherwise begin_switch_loading's cleared view
                        // hangs on the spinner forever.
                        if (current_room_id_ == room_id && room_view_)
                            if (auto* ml = room_view_->message_list())
                                ml->end_switch_loading();
                        return;
                    }
                    // One-time initial-history fill: the very first successful
                    // subscribe of a room this process lifetime may only have
                    // whatever the initial /sync (or a not-yet-backfilled
                    // store) provided — sometimes a single event. Gated on
                    // initial_fill_done (not reached_start, which for a busy
                    // room stays false indefinitely): every later switch back
                    // into an already-subscribed room reuses the same live SDK
                    // timeline (subscribe_room's reuse path) so nothing more is
                    // needed here — re-running this on every switch would keep
                    // fetching progressively older history, since matrix-sdk-
                    // ui's paginate_backwards(count) means "count MORE than
                    // currently shown", not "ensure count total". Cleared
                    // alongside the rest of pagination_[room_id] when the room
                    // ages out of the warm-LRU (prune_warm_subscriptions_), so
                    // a later resubscribe gets its own fresh fill. Scroll-
                    // driven pagination beyond this (RoomPane::
                    // request_pagination_back_) and the genuinely-empty-list
                    // case (MessageListView's autofill_only_when_empty_) are
                    // unaffected by this gate.
                    auto& state = pagination_[room_id];
                    if (!state.in_flight && !state.initial_fill_done)
                    {
                        state.in_flight = true;
                        run_async_(
                            [this, sess, room_id]() mutable
                            {
                                if (!sess->client)
                                {
                                    post_to_ui_([this, room_id]()
                                                { pagination_[room_id].in_flight =
                                                      false; });
                                    return;
                                }
                                auto pr = sess->client->paginate_back_with_status(
                                    room_id, kInitialFillBatch);
                                const bool reached = pr.ok && pr.reached_start;
                                post_to_ui_(
                                    [this, room_id, reached]()
                                    {
                                        auto& st = pagination_[room_id];
                                        st.in_flight = false;
                                        st.initial_fill_done = true;
                                        if (current_room_id_ == room_id)
                                            st.reached_start = reached;
                                    });
                            });
                    }
                    // Warm other visible rooms in the background on the
                    // SHARED pool so this never holds the mut thread. Has its
                    // own internal dedup (sdk/src/client/backfill.rs), so
                    // calling it on every switch is safe.
                    run_async_(
                        [sess, visible_ids = std::move(visible_ids)]() mutable
                        {
                            if (!sess->client)
                                return;
                            sess->client->start_background_backfill(visible_ids);
                        });
                });
        });
}

// Persist the current room-layout prefs (active room + open tabs) for the
// logged-in account. Builds the layout fresh from current_room_id_ + tabs_
// (PrefsData carries only these). Two calling contexts:
//  - via the DebounceSlot::AccountDataSave timer armed by
//    schedule_account_data_save_() — fire-and-forget (Client::
//    save_prefs_json), so routine mid-session saves never block the UI
//    thread.
//  - from on_window_closing_(), which passes `blocking=true` only when
//    this is the last open window (about to end the process — see its
//    doc comment for why). blocking=true cancels any still-pending
//    debounce and, if the layout was dirty, calls Client::
//    save_prefs_json_blocking() so the write is confirmed sent (or
//    definitively times out) before shutdown proceeds — closing the gap
//    where save_prefs's untracked spawned task could lose a race against
//    process exit and silently drop the last-open-room save.
void ShellBase::persist_room_layout_pref_(bool blocking)
{
    if (blocking)
    {
        // Drop any still-pending debounced fire — the save this function does
        // now (or skips, if nothing changed since the last one) supersedes it.
        cancel_debounce_(DebounceSlot::AccountDataSave);
        if (!account_data_dirty_ || !client_)
            return;
    }
    else if (!client_)
    {
        return;
    }

    std::vector<std::string> open;
    std::string last_room = current_room_id_;
    if (!pending_restore_rooms_.empty())
    {
        // The saved layout hasn't been reopened yet (startup): keep it rather
        // than writing the still-empty tab strip over it — a non-layout save
        // (e.g. a skin-tone change) can land here this early.
        open = pending_restore_rooms_;
        last_room = pending_restore_rooms_.front();
    }
    else
    {
        open.reserve(tabs_.size());
        for (const auto& t : tabs_)
            open.push_back(t.room_id);
    }
    auto prefs_data = tesseract::Prefs::room_layout(last_room, open);
    // room_layout() only fills in the tab-layout fields; carry the bridge
    // overrides through unchanged so a tab switch doesn't wipe them.
    settle_emoji_skin_tone_();
    if (active_account_)
    {
        prefs_data.bridge_not_bridged_overrides = active_account_->bridge_not_bridged_overrides;
        prefs_data.emoji_skin_tone =
            tesseract::emoji::skin_tone_key(active_account_->emoji_skin_tone);
        active_account_->recent_rooms = recent_room_ids_;
    }
    prefs_data.recent_rooms = recent_room_ids_;
    // Overlay onto the last known event so keys this build doesn't write
    // survive.
    const std::string json = tesseract::Prefs::serialize(
        prefs_data, active_account_ ? active_account_->prefs_json : std::string{});
    if (active_account_)
        active_account_->prefs_json = json;

    if (blocking)
    {
        // Deliberately synchronous: on_window_closing_() wants shutdown itself
        // to wait, so the save can't lose its race against process exit the
        // way the fire-and-forget path (below) can — save_prefs_blocking()
        // internally bounds the wait so a dead network can't hang the app.
        client_->save_prefs_json_blocking(json);
        account_data_dirty_ = false;
        return;
    }

    account_data_dirty_ = false;
    client_->save_prefs_json(json);
}

void ShellBase::rebuild_room_index_() const
{
    room_index_by_id_.clear();
    room_index_by_id_.reserve(rooms_.size());
    for (std::size_t i = 0; i < rooms_.size(); ++i)
        room_index_by_id_[rooms_[i].id] = i;
    room_index_dirty_ = false;
}

const RoomInfo* ShellBase::room_by_id_(const std::string& room_id) const
{
    if (room_index_dirty_)
        rebuild_room_index_();
    auto it = room_index_by_id_.find(room_id);
    if (it == room_index_by_id_.end() || it->second >= rooms_.size())
    {
        auto hidden = hidden_rooms_.find(room_id);
        return hidden == hidden_rooms_.end() ? nullptr : &hidden->second;
    }
    return &rooms_[it->second];
}

void ShellBase::open_room_version_(const std::string& room_id,
                                   const std::vector<std::string>& via,
                                   const std::string& highlight_event,
                                   bool join_if_missing)
{
    if (room_id.empty())
        return;
    auto reveal = [this, room_id, highlight_event]
    {
        tab_navigate_room(room_id);
        if (!highlight_event.empty())
        {
            if (room_view_ && room_view_->message_list())
                room_view_->message_list()->set_highlighted_event(highlight_event);
            try_scroll_to_room_event_(highlight_event);
        }
    };
    if (room_by_id_(room_id) && !hidden_rooms_.count(room_id))
    {
        reveal();
        return;
    }
    const auto sess = acting_session_(nullptr);
    if (!sess || !sess->client)
        return;
    // Refreshed every time: the cached copy of a hidden room goes stale (its
    // successor may have been joined or left since).
    run_async_(
        "room-version",
        [this, sess, room_id, via, highlight_event, join_if_missing, reveal]() mutable
        {
            std::optional<RoomInfo> info = sess->client->room_info(room_id);
            post_to_ui_alive_(
                [this, sess, room_id, via, highlight_event, join_if_missing, reveal,
                 info = std::move(info)]() mutable
                {
                    if (info)
                    {
                        // Still listed (e.g. its successor was left meanwhile):
                        // the live entry wins.
                        if (!room_by_id_(room_id) || hidden_rooms_.count(room_id))
                            hidden_rooms_[room_id] = std::move(*info);
                        reveal();
                        return;
                    }
                    // Not a room we're in: join it (or offer to), and let the
                    // post-join hook navigate and jump to the event.
                    if (!highlight_event.empty())
                        pending_event_scroll_after_join_[room_id] = highlight_event;
                    if (join_if_missing)
                    {
                        join_room_command_(room_id, via, sess);
                    }
                    else if (main_app_)
                    {
                        main_app_->add_room_view()->open_join_with_prefill(room_id);
                        request_relayout_();
                    }
                });
        });
}

void ShellBase::on_room_selected_(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }

    // Drill into a space if the clicked row is one.
    if (const auto* r = room_by_id_(room_id); r && r->is_space)
    {
        views::RoomListView* rlv =
            main_app_ ? main_app_->room_list_view() : nullptr;
        space_nav_frames_.push_back(SpaceNavFrame::capture(rlv));
        space_stack_.push_back(room_id);
        refresh_room_list_();
        SpaceNavFrame::enter(rlv);
        return;
    }

    hide_compose_popups_();
    handle_compose_room_leaving_(current_room_id_);
    // (No unsubscribe-on-leave here: the warm-subscription LRU in
    // prune_warm_subscriptions_ owns timeline lifecycle, keeping recently-left
    // rooms warm for instant reuse and evicting the rest.)
    current_room_id_ = room_id;
    // Member prefetch (for mention pills/clicks) lives in RoomView::set_room(),
    // so no shell has to wire it here.
    clear_focused_state_(room_id);
    restart_mark_read_timer_(Settings::instance().mark_as_read_delay_ms);
    update_typing_bar_({}, false);
    if (room_view_)
    {
        room_view_->compose_bar()->clear_reply();
        room_view_->compose_bar()->clear_editing();
        if (auto* ta = room_view_->compose_bar()->text_area())
        {
            ta->set_text("");
        }
        room_view_->clear_compose_text();
    }
    // Focus is handled by RoomView::set_room()'s own default-focus policy.
    before_room_set_();

    if (room_view_)
    {
        if (const auto* r = room_by_id_(current_room_id_))
        {
            room_view_->set_room(*r);
            after_room_set_();
        }
    }
    refresh_window_title_();
    if (main_room_pane_)
        main_room_pane_->apply_compose_draft_(current_room_id_);

    // Subscribe (mut pool) + initial history (shared pool). The split keeps the
    // network paginate off the single mut thread so the next switch's reset is
    // never blocked. See start_room_subscription_.
    auto visible_ids = main_app_ ? main_app_->room_list_view()->visible_room_ids()
                                 : std::vector<std::string>{};
    start_room_subscription_(current_room_id_, std::move(visible_ids));
}

// Called after tabs_ and current_room_id_ have been updated. The shell must:
//   1. Sync the TabBar widget (add/remove/set_active).
//   2. Show/hide TabBar; set RoomHeader condensed mode.
//   3. Restore compose_draft for the newly active tab.
// main_room_pane_->retarget(current_room_id_) (called at each tab_* site
// that updates current_room_id_, before this hook runs) plus the next
// handle_timeline_reset_ui_ call handle the room-switch display gate —
// no action needed here.
// Default: rebuild the TabBar in tabs_ order (names + avatars), mark the
// active tab, navigate to its room via on_room_selected_(), then relayout.
// A shell with extra platform work overrides this and calls the base first.
void ShellBase::on_tab_state_changed_ui_()
{
    if (!main_app_)
    {
        return;
    }

    auto* tb = main_app_->tab_bar();
    const bool show_bar = tabs_.size() > 1;
    main_app_->set_tab_bar_visible(show_bar);

    if (tb)
    {
        // Rebuild in tabs_ order so visual order is always stable.
        tb->clear();
        for (const auto& t : tabs_)
        {
            const tk::Image* avatar = nullptr;
            std::string name;
            if (const auto* r = room_by_id_(t.room_id))
            {
                name = r->name;
                const std::string& av_mxc = r->effective_avatar_url();
                if (!av_mxc.empty())
                {
                    avatar = avatar_image_(av_mxc);
                }
            }
            tb->add_tab(t.room_id, name, avatar);
        }

        if (active_tab_idx_ < tabs_.size())
        {
            tb->set_active(tabs_[active_tab_idx_].room_id);
        }
    }

    // Navigate to the active tab's room. Copy the id: on_room_selected_ can
    // re-enter tab state and invalidate a reference into tabs_.
    if (active_tab_idx_ < tabs_.size())
    {
        const std::string active_room = tabs_[active_tab_idx_].room_id;
        on_room_selected_(active_room);
    }

    schedule_relayout_();
}

std::string ShellBase::compose_window_title_() const
{
    if (!app_settings_open_)
        if (const RoomInfo* r = room_by_id_(current_room_id_);
            r && !r->name.empty())
            return "Tesseract - " + r->name;
    return "Tesseract";
}

void ShellBase::refresh_window_title_()
{
    apply_window_title_ui_(compose_window_title_());
}

void ShellBase::set_app_settings_open_(bool open)
{
    if (app_settings_open_ == open)
        return;
    app_settings_open_ = open;
    refresh_window_title_();
}

void ShellBase::touch_visited_room_(const std::string& room_id)
{
    if (room_id.empty())
        return;
    auto& v = visited_lru_;
    v.erase(std::remove(v.begin(), v.end(), room_id), v.end());
    v.insert(v.begin(), room_id); // most-recently-active at the front
}

std::vector<std::string> ShellBase::select_warm_evictions_(
    const std::unordered_set<std::string>& keep, std::size_t warm_cap)
{
    std::vector<std::string> evicted;
    std::vector<std::string> retained;
    retained.reserve(visited_lru_.size());
    std::size_t warm_kept = 0;
    for (const auto& r : visited_lru_) // front = most recent
    {
        if (keep.count(r) != 0)
        {
            retained.push_back(r); // active / open tab / pinned: always kept
        }
        else if (warm_kept < warm_cap)
        {
            retained.push_back(r); // newest warm rooms, up to the cap
            ++warm_kept;
        }
        else
        {
            evicted.push_back(r); // older than the warm window → drop
        }
    }
    visited_lru_ = std::move(retained);
    return evicted;
}

void ShellBase::prune_warm_subscriptions_()
{
    std::unordered_set<std::string> keep;
    if (!current_room_id_.empty())
        keep.insert(current_room_id_);
    for (const auto& t : tabs_)
        keep.insert(t.room_id);
    // Pinned by one of this (the active) account's pop-outs. visited_lru_ is
    // the active account's, so other accounts' pins don't apply here.
    for (const auto& r : visited_lru_)
        if (room_pinned_by_popout_(r))
            keep.insert(r);
    // Favorite rooms, once opened, stay warm for the rest of the session so
    // switching back is always instant (no timeline rebuild / gap-resolving
    // /messages). They bypass the kWarmRoomsMax cap; un-favouriting drops a room
    // back to normal warm-LRU eviction on the next prune.
    for (const auto& r : rooms_)
        if (r.is_favorite)
            keep.insert(r.id);
    for (const auto& room : select_warm_evictions_(keep, kWarmRoomsMax))
    {
        // Drop the room's per-room pagination state: unsubscribe_room tears the
        // SDK timeline down, so a rebuilt timeline on return must start fresh —
        // a stale reached_start would otherwise suppress back-pagination and
        // leave the room showing truncated history.
        pagination_.erase(room);
        // Same reasoning as pagination_ above: without this, last_sent_receipt_
        // would grow one entry per distinct room ever visited this session,
        // never freed for as long as the account stays logged in. Losing the
        // dedup guard on eviction just means one possibly-redundant read
        // receipt gets resent if/when the room goes warm again.
        last_sent_receipt_.erase(room);
        forget_thread_receipts_(room);
        if (client_)
            client_->unsubscribe_room(room);
    }
}

std::vector<std::string> ShellBase::select_idle_room_evictions_(
    const std::unordered_map<std::string, std::chrono::steady_clock::time_point>&
        last_active,
    const std::unordered_set<std::string>& currently_visible,
    std::chrono::steady_clock::time_point now,
    std::chrono::minutes ttl)
{
    std::vector<std::string> evicted;
    for (const auto& [room_id, last] : last_active)
    {
        if (currently_visible.count(room_id) != 0)
            continue; // genuinely on-screen right now: never idle-evicted
        if (now - last >= ttl)
            evicted.push_back(room_id);
    }
    return evicted;
}

std::vector<std::pair<std::string, std::string>>
ShellBase::select_idle_thread_evictions_(
    const std::map<std::pair<std::string, std::string>,
                   std::chrono::steady_clock::time_point>& last_active,
    const std::set<std::pair<std::string, std::string>>& currently_visible,
    std::chrono::steady_clock::time_point now,
    std::chrono::minutes ttl)
{
    std::vector<std::pair<std::string, std::string>> evicted;
    for (const auto& [key, last] : last_active)
    {
        if (currently_visible.count(key) != 0)
            continue;
        if (now - last >= ttl)
            evicted.push_back(key);
    }
    return evicted;
}

// Builds the currently-visible room/thread sets, calls the two selection
// functions above, unsubscribes every evicted room/thread, and erases
// their bookkeeping state (pagination_, last_sent_receipt_,
// room_last_active_ / thread_last_active_). Called from the existing
// presence tick — see notify_presence_tick_ — so it needs no timer of
// its own.
void ShellBase::sweep_idle_timelines_()
{
    auto activity_scope = activity_.begin("idle-timeline-eviction", "UI housekeeping", "periodic");
    const auto now = std::chrono::steady_clock::now();

    std::unordered_set<std::string> visible_rooms;
    if (!current_room_id_.empty())
        visible_rooms.insert(current_room_id_);
    for (const auto& w : owned_secondary_windows_)
        visible_rooms.insert(w->room_id());

    std::set<std::pair<std::string, std::string>> visible_threads;
    if (main_room_pane_ && !current_room_id_.empty() &&
        !main_room_pane_->thread_root().empty())
        visible_threads.insert({current_room_id_, main_room_pane_->thread_root()});
    for (const auto& w : owned_secondary_windows_)
        if (!w->popout_thread_root().empty())
            visible_threads.insert({w->room_id(), w->popout_thread_root()});

    // Self-refresh: anything on-screen right now is never idle regardless of
    // when (or whether) it last went through touch_visited_room_ /
    // apply_thread_transition_ — this is what keeps a pop-out-only room (one
    // never shown in the main window) from acquiring no entry at all and thus
    // being permanently exempt from eviction even after its window closes.
    for (const auto& room : visible_rooms)
        room_last_active_[room] = now;
    for (const auto& key : visible_threads)
        thread_last_active_[key] = now;

    for (const auto& room :
         select_idle_room_evictions_(room_last_active_, visible_rooms, now,
                                     kIdleTimelineTtl))
    {
        // Same bookkeeping as prune_warm_subscriptions_: unsubscribe_room tears
        // the SDK timeline down, so stale pagination/receipt state must go too,
        // or the rebuilt timeline on return would show truncated history / skip
        // a receipt resend.
        pagination_.erase(room);
        last_sent_receipt_.erase(room);
        forget_thread_receipts_(room);
        room_last_active_.erase(room);
        if (client_)
            client_->unsubscribe_room(room);
    }

    for (const auto& [room_id, thread_root] :
         select_idle_thread_evictions_(thread_last_active_, visible_threads, now,
                                       kIdleTimelineTtl))
    {
        thread_last_active_.erase({room_id, thread_root});
        if (client_)
            client_->unsubscribe_thread(room_id, thread_root);
    }
}
} // namespace tesseract
