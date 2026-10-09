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
#include "views/CreatePollDialog.h"
#include "views/MainAppWidget.h"
#include "views/VideoViewerOverlay.h"
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

void ShellBase::set_room_notification_mode_(const std::string& room_id,
                                             const std::string& mode,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (!acting_client) return;
    auto sess = acting;
    run_async_mut_([sess, room_id, mode]() {
        if (!sess || !sess->client) return;
        sess->client->set_room_notification_mode(room_id, mode);
    });
}

void ShellBase::set_room_favourite_(const std::string& room_id, bool value,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (!acting_client) return;
    auto sess = acting;
    run_async_mut_([sess, room_id, value]() {
        if (!sess || !sess->client) return;
        sess->client->set_room_favourite(room_id, value);
    });
}

void ShellBase::set_room_low_priority_(const std::string& room_id, bool value,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (!acting_client) return;
    auto sess = acting;
    run_async_mut_([sess, room_id, value]() {
        if (!sess || !sess->client) return;
        sess->client->set_room_low_priority(room_id, value);
    });
}

void ShellBase::push_rooms_(std::string user_id, std::vector<RoomInfo> rooms)
{
    if (auto sess = account_manager_.find(user_id))
    {
        apply_bridge_overrides_(rooms, sess->bridge_not_bridged_overrides);
    }
    per_account_rooms_[user_id] = rooms;
    // The tray aggregate covers every signed-in account, not just the active
    // one — recompute on every account's update, even if it isn't the one
    // whose rooms_ cache is currently live.
    notify_tray_unread_();
    if (user_id != my_user_id_)
    {
        // A pending --open-room target may live on this (inactive) account.
        retry_pending_launch_room_();
        // Its pop-outs still show live room info (name, topic, avatar, ...).
        for (const auto& [rid, w] : secondary_windows_)
        {
            if (!w || w->owner_user_id() != user_id)
                continue;
            for (const auto& r : per_account_rooms_[user_id])
            {
                if (r.id == rid)
                {
                    w->on_room_info_updated(r);
                    break;
                }
            }
        }
        return;
    }
    rooms_ = std::move(rooms);
    mark_room_index_dirty_();
    // A hidden upgraded room that is listed again (its successor was left) is
    // now served by rooms_ itself.
    for (const auto& r : rooms_)
        hidden_rooms_.erase(r.id);
    // A change in the active account's room set may add/remove people; drop the
    // cached roster so the next user-mode query rebuilds it. Member-only changes
    // within existing rooms aren't tracked here — live-resolve covers anyone the
    // roster misses. Lazy rebuild means this is just a flag flip while idle.
    // Keyed on the room-id *set*, not the count: a sync that joins one room and
    // leaves another (count unchanged) still invalidates.
    std::size_t room_set_hash = 0;
    for (const auto& r : rooms_)
    {
        room_set_hash ^= std::hash<std::string>{}(r.id);
    }
    if (room_set_hash != known_users_room_set_hash_)
    {
        known_users_room_set_hash_ = room_set_hash;
        invalidate_known_users_();
    }
    update_space_children_cache_();
    // Immediate (not waiting on update_space_children_cache_'s async
    // fetch, which no-ops when the space-children set itself didn't
    // change): the room-management section's candidate list depends on
    // rooms_ directly, so a newly joined/left room needs to be reflected
    // here even when no space's children changed at all.
    //
    // Re-runs the full show_space_root_ (not just refresh_space_root_children_)
    // so the space's own RoomInfo — in particular its name — gets re-pushed
    // too: a just-created space can still be showing sync's placeholder
    // "Empty room" name (matrix-sdk's display_name() fallback, computed
    // before the m.room.name state event this same create_room call set had
    // actually arrived) at the moment it was first shown, and nothing else
    // ever revisits that once shown. show_space_root_ no-ops harmlessly
    // when nothing is currently shown (room_by_id_("") is null).
    if (!space_root_shown_id_.empty())
        show_space_root_(space_root_shown_id_);
    else
        refresh_space_root_children_(space_root_shown_id_);
    on_rooms_updated_();
    // Re-evaluate call-button and threads-button visibility for the current
    // room: bridge status (is_bridged) can change via on_rooms_updated without
    // a room switch, so after_active_room_changed_ and handle_server_info_async
    // won't re-run. Read from rooms_ which on_rooms_updated_() just refreshed.
    if (!current_room_id_.empty())
    {
        if (room_view_ && room_view_->header())
            update_call_btn_visibility_(room_view_->header(), current_room_id_);
        for (auto& [rid, w] : active_account_popouts_())
        {
            if (auto* rv = w->room_view())
                if (auto* h = rv->header())
                    update_call_btn_visibility_(h, rid);
        }
        if (client_ && room_view_)
            apply_threads_list_(client_->list_room_threads(current_room_id_));
        // A just-created room can turn out to be a space only once sync
        // actually delivers its creation_content — e.g. CreateRoomView's
        // "Create Space" navigates here before this room exists in rooms_
        // at all, so after_active_room_changed_()'s is_space check saw
        // nothing and fell through to the normal RoomView. Retry that
        // check now that rooms_ actually knows about the room, so the view
        // doesn't stay stuck showing it as a plain chat room.
        if (const auto* cur = room_by_id_(current_room_id_);
            cur && cur->is_space && space_root_shown_id_ != current_room_id_)
        {
            show_space_root_(current_room_id_);
        }
        // Same for a just-created call room: after_active_room_changed_()'s
        // handle_call_room_navigation_() couldn't see is_call_room yet, so
        // the lobby never opened. Retry once now that the room is known.
        if (pending_call_room_nav_id_ == current_room_id_ && room_by_id_(current_room_id_))
            handle_call_room_navigation_();
    }
    // Call members / bridge status may have changed with this update.
    refresh_call_banners_();
    // Refresh the pinned-events banner from the now-updated cache. Picks up
    // both pin/unpin state-event changes and PL changes that flip can_pin
    // or the redact-others (delete-others'-messages) permission.
    refresh_pinned_for_current_room_();
    retry_pending_launch_room_();

    // Every background-warming dispatch below shares the single-thread
    // mut_pool_ with subscribe_room() (queued moments ago inside
    // on_rooms_updated_(), if this tick just restored the last-active room —
    // see try_restore_tab_session_ / start_room_subscription_). Skip them all
    // while a restore is still pending: push_rooms_ can fire several times
    // before the previously-active room actually shows up in the room list
    // (sliding sync delivers it incrementally), and each earlier tick would
    // otherwise queue backfill/bridge-check/prefetch work for every other
    // visible room ahead of the eventual subscribe_room() call on that same
    // FIFO thread — the restored room would sit behind a growing backlog of
    // warming work for rooms the user isn't even looking at yet. Once
    // pending_restore_rooms_ is empty (restored, or there was nothing to
    // restore), these resume normally on every subsequent tick.
    //
    // Bounded by restore_gate_ticks_: the previously-active room can
    // legitimately never reappear (left/kicked from another device since
    // last session), which would otherwise gate this warming work for the
    // rest of the session. Give up after kRestoreGateMaxTicks ticks and let
    // it resume regardless.
    bool restore_pending = !pending_restore_rooms_.empty();
    if (restore_pending)
    {
        if (++restore_gate_ticks_ >= kRestoreGateMaxTicks)
            restore_pending = false;
    }

    // When inactive grouping is enabled, ensure every room (not just the
    // visible slice) has its last_activity_ts populated so the inactive
    // section can classify all rooms correctly. Idempotent: Rust skips rooms
    // already in backfill_ts and returns immediately if a task is running.
    // Dispatched off the UI thread regardless: start_background_backfill_all_
    // uncached is SH_FFI (see client.cpp), so it never blocks the UI thread,
    // but the call itself still does real work worth keeping off it.
    if (client_ && !restore_pending && !low_power_active() &&
        tesseract::Settings::instance().group_inactive_rooms)
    {
        auto sess = active_account_;
        run_async_mut_("backfill-uncached-rooms", [sess]() {
            if (sess && sess->client)
                sess->client->start_background_backfill_all_uncached();
        });
    }

    // Check bridge status (MSC2346) for visible rooms. Fires only when the
    // room-id set changes (fingerprint guard). The Rust side skips rooms
    // already cached in SQLite and is idempotent while a check is in flight.
    if (client_ && !restore_pending && !low_power_active() && !rooms_.empty())
    {
        std::size_t fp = 0;
        std::vector<std::string> ids;
        ids.reserve(rooms_.size());
        for (const auto& r : rooms_)
        {
            fp ^= std::hash<std::string>{}(r.id);
            ids.push_back(r.id);
        }
        if (fp != bridge_check_fingerprint_)
        {
            bridge_check_fingerprint_ = fp;
            auto sess = active_account_;
            run_async_mut_("bridge-status-check", [sess, ids = std::move(ids)]() mutable {
                if (sess && sess->client)
                    sess->client->start_bridge_status_check(ids);
            });
        }
    }

    // Proactively warm the event cache for favorite and (if enabled)
    // quiet-unread rooms so opening them is instant. Favorites are always
    // included — they're an explicit user action, not a heuristic like
    // "unread" — so this still runs with the setting off; only the unread
    // half of the selection is gated by it. push_rooms_ fires on every sync
    // tick, so we gate the FFI call on a fingerprint of the selected set — it
    // only fires when that set changes (new favorite/unread room, or new
    // messages in an already-prefetched one). The Rust side skips live
    // timelines and is idempotent while a prefetch is in flight.
    if (client_ && !restore_pending && !low_power_active())
    {
        auto sel = compute_unread_prefetch_set(
            rooms_, current_room_id_, kUnreadPrefetchCap,
            tesseract::Settings::instance().prefetch_unread_rooms);
        if (sel.fingerprint != unread_prefetch_fingerprint_)
        {
            unread_prefetch_fingerprint_ = sel.fingerprint;
            if (!sel.ids.empty())
            {
                // Capture the owning session (not `this`/`client_`): the worker
                // runs on mut_pool_ where a concurrent account switch/logout can
                // reassign or free the raw client_ pointer. Holding `sess` keeps
                // the Client alive for the call — same pattern as subscribe_room.
                auto sess = active_account_;
                run_async_mut_(
                    "unread-prefetch",
                    [sess, ids = std::move(sel.ids)]() mutable
                    {
                        if (sess && sess->client)
                            sess->client->start_unread_prefetch(ids);
                    });
            }
        }
    }

    // One-time encryption setup check — raises the overlay on the first
    // eligible sync tick after login (Disabled → Fresh, Incomplete → Recover).
    check_encryption_setup_();

    // Re-evaluate the "verify this device" banner now that the roster (and any
    // foreign cross-signing identity) has synced. The initial verification_state
    // snapshot can fire before a second device's identity is known, which would
    // otherwise leave the prompt suppressed; the shells' handler is idempotent.
    handle_verification_state_ui_(read_device_verified_());

    // Replay a matrix link that arrived before we were logged in.
    if (!pending_matrix_link_.empty())
    {
        auto uri = std::move(pending_matrix_link_);
        pending_matrix_link_.clear();
        open_matrix_link(uri);
    }
}

void ShellBase::push_invites_(std::string user_id, std::vector<InviteInfo> invites)
{
    per_account_invites_[user_id] = invites;
    if (user_id != my_user_id_)
    {
        return;
    }
    invites_ = std::move(invites);
    ensure_invite_avatars_();
    on_invites_updated_();
}

void ShellBase::ensure_invite_avatars_()
{
    // MSC4278: don't fetch invite avatars the UI won't show.
    if (!tesseract::Settings::instance().invite_avatars)
    {
        return;
    }
    for (const auto& inv : invites_)
    {
        const std::string& mxc =
            inv.is_direct ? inv.inviter_avatar_url : inv.room_avatar_url;
        ensure_user_avatar_(mxc);
    }
}

const InviteInfo* ShellBase::find_invite_(const std::string& room_id) const
{
    for (const auto& inv : invites_)
    {
        if (inv.room_id == room_id)
        {
            return &inv;
        }
    }
    return nullptr;
}

void ShellBase::accept_invite_async_(const std::string& room_id)
{
    if (room_id.empty() || !client_)
        return;
    auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id, RoomActionKind::Accept};
    client_->accept_invite_async(req_id, room_id);
}

void ShellBase::decline_invite_async_(const std::string& room_id)
{
    if (room_id.empty() || !client_)
        return;
    // Optimistically remove from the local list for immediate UX; the next
    // on_invites_updated callback from sync will confirm or restore it.
    invites_.erase(
        std::remove_if(invites_.begin(), invites_.end(),
                       [&room_id](const InviteInfo& inv)
                       { return inv.room_id == room_id; }),
        invites_.end());
    on_invites_updated_();
    client_->decline_invite_async(room_id);
}

void ShellBase::block_invite_async_(const std::string& room_id,
                                    const std::string& inviter_id)
{
    if (room_id.empty() || !client_)
        return;
    // Optimistically remove from the local list for immediate UX; the next
    // on_invites_updated callback from sync will confirm or restore it.
    invites_.erase(
        std::remove_if(invites_.begin(), invites_.end(),
                       [&room_id](const InviteInfo& inv)
                       { return inv.room_id == room_id; }),
        invites_.end());
    on_invites_updated_();
    client_->block_invite_async(room_id, inviter_id);
}

void ShellBase::push_my_knocks_(std::string user_id, std::vector<KnockedRoomInfo> knocks)
{
    per_account_my_knocks_[user_id] = knocks;
    if (user_id != my_user_id_)
    {
        return;
    }
    my_knocks_ = std::move(knocks);
    on_my_knocks_updated_();
}

const KnockedRoomInfo* ShellBase::find_my_knock_(const std::string& room_id) const
{
    for (const auto& k : my_knocks_)
    {
        if (k.room_id == room_id)
        {
            return &k;
        }
    }
    return nullptr;
}

void ShellBase::knock_room_command_(const std::string& room_id_or_alias,
                                    const std::string& reason,
                                    std::vector<std::string> via,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id_or_alias.empty() || !acting_client)
        return;
    if (via.empty())
    {
        if (auto it = pending_join_via_.find(room_id_or_alias);
            it != pending_join_via_.end())
            via = it->second;
    }
    auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id_or_alias, RoomActionKind::Knock};
    acting_client->knock_room_async(req_id, room_id_or_alias, reason, via);
}

void ShellBase::retract_knock_command_(const std::string& room_id)
{
    // Room::leave() already handles the Knocked membership state, so
    // retracting a knock is exactly the same call as leaving a room.
    leave_room_command_(room_id);
}

void ShellBase::subscribe_knock_requests_panel_(
    const std::string& room_id, const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    if (room_id.empty() || !acting || !acting->client)
        return;
    if (knock_requests_panel_room_id_ == room_id &&
        knock_requests_panel_account_.lock() == acting)
        return; // already subscribed
    unsubscribe_knock_requests_panel_();
    knock_requests_panel_room_id_ = room_id;
    knock_requests_panel_account_ = acting;
    current_room_knock_requests_.clear();
    acting->client->subscribe_room_knock_requests(room_id);
}

void ShellBase::unsubscribe_knock_requests_panel_()
{
    if (knock_requests_panel_room_id_.empty())
        return;
    // On the client that subscribed, which may not be the active one.
    if (auto sess = knock_requests_panel_account_.lock(); sess && sess->client)
        sess->client->unsubscribe_room_knock_requests(knock_requests_panel_room_id_);
    knock_requests_panel_room_id_.clear();
    knock_requests_panel_account_.reset();
    current_room_knock_requests_.clear();
}

void ShellBase::accept_knock_request_async_(const std::string& room_id,
                                            const std::string& user_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || user_id.empty() || !acting_client)
        return;
    auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id, RoomActionKind::AcceptKnock};
    acting_client->accept_knock_request_async(req_id, room_id, user_id);
}

void ShellBase::decline_knock_request_async_(const std::string& room_id,
                                             const std::string& user_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || user_id.empty() || !acting_client)
        return;
    // Optimistically remove from the local list for immediate UX; the next
    // on_knock_requests_updated poke from the SDK will confirm or restore it.
    current_room_knock_requests_.erase(
        std::remove_if(current_room_knock_requests_.begin(),
                       current_room_knock_requests_.end(),
                       [&user_id](const KnockRequestInfo& r)
                       { return r.user_id == user_id; }),
        current_room_knock_requests_.end());
    on_knock_requests_panel_updated_();
    acting_client->decline_knock_request_async(room_id, user_id, "");
}

void ShellBase::decline_and_ban_knock_request_async_(const std::string& room_id,
                                                     const std::string& user_id,
                                                     const std::string& reason,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || user_id.empty() || !acting_client)
        return;
    // Optimistically remove from the local list for immediate UX; the next
    // on_knock_requests_updated poke from the SDK will confirm or restore it.
    current_room_knock_requests_.erase(
        std::remove_if(current_room_knock_requests_.begin(),
                       current_room_knock_requests_.end(),
                       [&user_id](const KnockRequestInfo& r)
                       { return r.user_id == user_id; }),
        current_room_knock_requests_.end());
    on_knock_requests_panel_updated_();
    acting_client->decline_and_ban_knock_request_async(room_id, user_id, reason);
}

void ShellBase::leave_room_command_(const std::string& room_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || !acting_client)
        return;
    auto req_id = next_room_action_id_++;
    const RoomInfo* ri = room_by_id_(room_id);
    const auto kind = (ri && ri->is_space) ? RoomActionKind::LeaveSpace
                                           : RoomActionKind::Leave;
    pending_room_actions_[req_id] = {room_id, kind};
    acting_client->leave_room_async(req_id, room_id);
}

// Shared "step back out" navigation once a Leave completes for a room
// that was a space: pops space_stack_/space_nav_frames_ and hides the
// space-root/room-preview panels exactly like the room list's own back
// button (see each shell's on_space_back), so leaving doesn't strand the
// UI on the now-gone space's summary. Safe to call even if space_id
// wasn't actually the current stack top / active room.
void ShellBase::leave_space_navigate_back_(const std::string& space_id)
{
    if (!space_stack_.empty() && space_stack_.back() == space_id)
        space_stack_.pop_back();
    if (main_app_)
    {
        main_app_->hide_room_preview();
        main_app_->hide_space_root();
    }
    space_root_shown_id_.clear();
    refresh_room_list_();
    if (!space_nav_frames_.empty())
    {
        if (main_app_ && main_app_->room_list_view())
            space_nav_frames_.back().restore(main_app_->room_list_view());
        space_nav_frames_.pop_back();
    }
    if (current_room_id_ == space_id)
    {
        current_room_id_.clear();
        after_active_room_changed_();
    }
}

// The room list's own "back" button: exits one level of the sidebar's
// "drilled into a space" browsing (space_stack_/space_nav_frames_).
// Purely a sidebar action — current_room_id_ (the main pane's active
// room) is untouched, so if it's still a space (e.g. that's what's
// actually open in the main pane), the space-root view is re-asserted
// rather than being blindly hidden, which would otherwise reveal
// RoomView underneath showing that space's own (effectively empty)
// room instead. Every shell's on_space_back delegates here.
void ShellBase::space_back_command_()
{
    if (!space_stack_.empty())
        space_stack_.pop_back();
    if (main_app_)
        main_app_->hide_room_preview();

    if (const auto* cur = room_by_id_(current_room_id_); cur && cur->is_space)
    {
        // current_room_id_ is itself still a space — e.g. it's what's
        // actually open in the main pane — so re-assert the space root
        // instead of hiding it: hide_space_root() unconditionally reveals
        // RoomView underneath, which would show that space's own
        // (effectively empty) room instead.
        show_space_root_(current_room_id_);
    }
    else
    {
        if (main_app_)
            main_app_->hide_space_root();
        space_root_shown_id_.clear();
    }

    refresh_room_list_();
    if (!space_nav_frames_.empty())
    {
        if (main_app_ && main_app_->room_list_view())
            space_nav_frames_.back().restore(main_app_->room_list_view());
        space_nav_frames_.pop_back();
    }
}

void ShellBase::confirm_leave_room_(const std::string& room_id)
{
    if (room_id.empty() || !main_app_ || !main_app_->confirm_dialog())
        return;
    const RoomInfo* ri = room_by_id_(room_id);
    const std::string display =
        (ri && !ri->name.empty()) ? ri->name : tk::tr("this room");

    views::ConfirmDialog::Options opts;
    opts.title          = tk::trf(tk::tr("Leave {0}?"), {display});
    opts.body           = tk::tr("You will stop receiving messages and need "
                                 "to be re-invited to rejoin.");
    opts.confirm_label  = tk::tr("Leave");
    opts.cancel_label   = tk::tr("Cancel");
    opts.destructive    = true;

    main_app_->confirm_dialog()->open(
        std::move(opts),
        [this, room_id] { leave_room_command_(room_id); });
}

void ShellBase::join_room_command_(const std::string& room_id_or_alias,
                                   std::vector<std::string> via,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id_or_alias.empty() || !acting_client)
        return;
    // No caller-supplied `via`? Fall back to any `?via=` hints open_matrix_link()
    // stashed for this id/alias (a matrix.to / matrix: permalink the user
    // followed). The SDK also derives hints from joined spaces + the domain, so
    // an empty list here is still fine for the common cases.
    if (via.empty())
    {
        if (auto it = pending_join_via_.find(room_id_or_alias);
            it != pending_join_via_.end())
            via = it->second;
    }
    // Callers that only know how to "join" a room (RoomPreviewView's
    // space-browsing "Available to Join" panel, /join, unjoined space-child
    // rows) all funnel through here — redirecting knock-required rooms to
    // knock_room_command_ in this one place means none of them need their
    // own knock-awareness. JoinRoomView (the "Join a Room" dialog) already
    // knows up front via its richer preview_ and calls knock_room_command_
    // directly with an optional reason, bypassing this redirect.
    if (auto cached = acting_client->get_cached_room_summary(room_id_or_alias))
    {
        const bool wants_knock =
            (cached->join_rule == "knock" || cached->join_rule == "knock_restricted") &&
            cached->membership != "join" && cached->membership != "knock";
        if (wants_knock)
        {
            knock_room_command_(room_id_or_alias, "", std::move(via), acting);
            return;
        }
    }
    auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id_or_alias, RoomActionKind::Join};
    acting_client->join_room_async(req_id, room_id_or_alias, via);
}

void ShellBase::lookup_room_command_(const std::string& room_id_or_alias)
{
    auto* ar = main_app_ ? main_app_->add_room_view() : nullptr;
    auto* jr = ar ? ar->join_view() : nullptr;
    if (!jr)
        return;
    jr->set_state(views::JoinRoomView::State::Loading);
    request_relayout_();

    auto sess = active_account_;
    const std::uint64_t gen = ++join_room_lookup_gen_;
    // Route the preview through any `?via=` hints a followed permalink left for
    // this id/alias, so a federated room can be previewed before it's joined.
    std::vector<std::string> via;
    if (auto it = pending_join_via_.find(room_id_or_alias);
        it != pending_join_via_.end())
        via = it->second;
    run_async_(
        [this, sess, room_id_or_alias, via, gen]()
        {
            tesseract::RoomSummary summary;
            if (sess && sess->client)
                summary = sess->client->get_room_summary(room_id_or_alias, via);
            post_to_ui_alive_(
                [this, gen, room_id_or_alias, via, summary = std::move(summary)]()
                {
                    if (gen != join_room_lookup_gen_)
                        return;
                    // The Join button joins by the resolved room id, which
                    // differs from an alias permalink's target — carry the
                    // `?via=` hints over so the join still routes correctly.
                    if (summary.ok() && !via.empty() &&
                        !summary.room_id.empty() &&
                        summary.room_id != room_id_or_alias)
                        pending_join_via_[summary.room_id] = via;
                    auto* ar2 = main_app_ ? main_app_->add_room_view() : nullptr;
                    auto* jr2 = ar2 ? ar2->join_view() : nullptr;
                    if (!jr2 || !ar2->is_open() ||
                        ar2->active_tab() != views::AddRoomView::Tab::Join)
                        return;
                    if (summary.ok())
                    {
                        jr2->set_preview(summary);
                        // Unfamiliar/unjoined rooms are never in the
                        // thumbnail cache yet — explicitly fetch the avatar
                        // (mirrors fetch_single_room_summary_'s identical
                        // call for space-child preview rows) so the preview
                        // card's avatar_provider_ callback has something to
                        // return once it lands.
                        if (!summary.avatar_url.empty())
                            ensure_media_thumbnail_(summary.avatar_url, 64, 64, false);
                    }
                    else
                        jr2->set_error({});
                    request_relayout_();
                });
        });
}

void ShellBase::create_room_command_(const RoomCreateOptions& options)
{
    if (!client_)
        return;
    auto* ar = main_app_ ? main_app_->add_room_view() : nullptr;
    if (auto* cr = ar ? ar->create_view() : nullptr)
        cr->set_state(views::CreateRoomView::State::Creating);
    auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {"", RoomActionKind::Create};
    client_->create_room_async(req_id, options);
}

void ShellBase::invite_user_command_(const std::string& room_id,
                                     const std::string& user_id,
                                     const std::string& reason,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || user_id.empty() || !acting_client)
        return;
    auto req_id = next_room_action_id_++;
    pending_invites_[req_id] = {room_id, user_id, nullptr};
    acting_client->invite_user_async(req_id, room_id, user_id, reason);
}

void ShellBase::moderate_member_(ModerationAction action, const std::string& room_id,
                                 const std::string& user_id,
                                 const std::string& display_name,
                                 const std::string& reason,
                                 std::function<void(bool ok)> done,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || user_id.empty() || !acting_client)
        return;
    auto req_id = next_room_action_id_++;
    pending_moderations_[req_id] = {user_id, display_name, action, std::move(done)};
    switch (action)
    {
    case ModerationAction::Kick:
        acting_client->kick_user_async(req_id, room_id, user_id, reason);
        break;
    case ModerationAction::Ban:
        acting_client->ban_user_async(req_id, room_id, user_id, reason);
        break;
    case ModerationAction::Unban:
        acting_client->unban_user_async(req_id, room_id, user_id, reason);
        break;
    }
}

void ShellBase::invite_users_(
    const std::string& room_id, const std::vector<std::string>& user_ids,
    std::function<void(const std::string& user_id, bool ok,
                       const std::string& message)> per_user,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || !acting_client)
        return;
    // One shared copy of the per-user callback across every request.
    auto cb = std::make_shared<decltype(per_user)>(std::move(per_user));
    for (const auto& user_id : user_ids)
    {
        auto req_id = next_room_action_id_++;
        pending_invites_[req_id] = {
            room_id, user_id,
            [cb, user_id](bool ok, const std::string& message)
            {
                if (*cb)
                    (*cb)(user_id, ok, message);
            }};
        acting_client->invite_user_async(req_id, room_id, user_id);
    }
}

void ShellBase::open_create_poll_dialog_(const std::string& room_id,
                                         const std::shared_ptr<AccountSession>& on_behalf_of)
{
    auto* dlg = main_app_ ? main_app_->create_poll_dialog() : nullptr;
    const auto acting = acting_session_(on_behalf_of);
    if (!dlg || !acting || !acting->client)
        return;
    // Bound here at open time, not at shell construction, so every shell gets
    // the wiring from this one call site.
    // The poll may target a room other than the one the main window shows
    // (pop-out / thread composer), so name it in the dialog title.
    std::string room_name = room_id;
    if (const auto* r = room_by_id_(room_id); r && !r->name.empty())
        room_name = r->name;
    dlg->open([this, room_id, acting](views::PollDraft d) {
        run_async_mut_([this, acting, room_id, d = std::move(d)]() {
            if (!acting->client)
                return;
            const auto r = acting->client->send_poll(
                room_id, d.question, d.options, views::poll_draft_max_selections(d),
                !d.hide_results);
            if (!r.ok && r.message != "cancelled")
                post_to_ui_alive_([this]() {
                    show_status_message_(tk::tr("Couldn't send the poll"), 6000);
                });
        });
    }, std::move(room_name));
}

ShellBase::RoomSendOutcome ShellBase::dispatch_room_send_(
    const std::string& room_id, const std::string& body,
    const std::string& formatted_body,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    RoomSendOutcome out;
    if (room_id.empty() || !acting_client)
    {
        // No active room/client: treat as consumed so callers no-op without
        // clearing on a failed send result.
        out.handled_as_command = true;
        return out;
    }
    // Commands that open a native dialog or enqueue async room actions are
    // intercepted here; everything else falls through to dispatch_compose_send.
    if (tesseract::is_slash_command_no_arg(body, "myroomavatar"))
    {
        pick_and_set_room_avatar_(room_id, acting);
        out.handled_as_command = true;
        return out;
    }
    if (tesseract::is_slash_command_no_arg(body, "poll"))
    {
        open_create_poll_dialog_(room_id, acting);
        out.handled_as_command = true;
        return out;
    }
    if (tesseract::is_slash_command_no_arg(body, "leave"))
    {
        leave_room_command_(room_id, acting);
        out.handled_as_command = true;
        return out;
    }
    if (auto target = tesseract::parse_slash_arg(body, "join"))
    {
        join_room_command_(*target, {}, acting);
        out.handled_as_command = true;
        return out;
    }
    if (auto args = tesseract::parse_slash_args(body, "invite"))
    {
        const std::string user_id = args->empty() ? std::string() : args->front();
        std::string reason;
        for (std::size_t i = 1; i < args->size(); ++i)
        {
            if (i > 1)
                reason += ' ';
            reason += (*args)[i];
        }
        invite_user_command_(room_id, user_id, reason, acting);
        out.handled_as_command = true;
        return out;
    }
    out.handled_as_command = false;

    // Maps-link-to-location: only when the ENTIRE trimmed body is a single
    // recognized Google Maps / OpenStreetMap URL, and the user has opted
    // in. classify_maps_link is cheap/synchronous (no I/O), so it's checked
    // inline on the UI thread like the slash-command checks above. Direct
    // matches and shortlink resolution both run inside a single
    // run_async_mut_ task — see the note on WorkerPool destruction order in
    // ShellBase.h (pool_ before mut_pool_) for why the resolve+send sequence
    // must NOT hop across pools.
    if (tesseract::Settings::instance().send_maps_urls_as_location)
    {
        std::string trimmed = tesseract::text::trim(body);
        auto cls = tesseract::classify_maps_link(trimmed);
        if (cls.matched)
        {
            auto sess = acting;
            auto rid = room_id;
            auto body_copy = body;
            auto fmt_copy = formatted_body;
            auto trimmed_copy = trimmed;
            auto shortlink = cls.shortlink_url;
            bool needs_resolve = cls.needs_resolve;
            double lat = cls.lat, lon = cls.lon;
            // Through the pipeline (no prepare step) so it keeps its place
            // among this room's sends and a slow shortlink resolve shows the
            // send-button spinner.
            submit_room_send_(sess, rid, std::string{},
                              [this, sess, rid, needs_resolve, lat, lon, shortlink,
                               body_copy, fmt_copy, trimmed_copy](const std::string&) mutable {
                if (!sess || !sess->client) return;
                tesseract::Result r;
                if (needs_resolve)
                {
                    auto resolved = sess->client->resolve_maps_shortlink(shortlink);
                    if (resolved.matched)
                        r = sess->client->send_location(rid, resolved.lat,
                                                         resolved.lon, trimmed_copy);
                    else
                        r = tesseract::dispatch_compose_send(*sess->client, rid,
                                                             body_copy, fmt_copy);
                }
                else
                {
                    r = sess->client->send_location(rid, lat, lon, trimmed_copy);
                }
                report_unsent_message_(sess->user_id, rid, body_copy, r);
            });
            out.send_result = tesseract::Result{true, ""};
            return out;
        }
    }

    // Normal send: enqueue on the mutation pool so the UI thread never blocks
    // in markdown_to_html / the SH_FFI lock / block_on(timeline.send). This is
    // the one composer mutation that used to run inline (reply/edit/redact/
    // reaction already hop to mut_pool_), so under SH_FFI contention with a
    // heavy exclusive op (start_sync/clear_caches/logout) it could freeze the
    // composer; off-thread it cannot. timeline.send() only enqueues, so once
    // it succeeds any later failure surfaces via the per-message ◷→⚠/retry
    // indicator. Report success so the caller clears the composer immediately
    // (matching the optimistic clear the popout RoomWindow already does); a
    // failure before the message reached the send queue (no local echo, so no
    // retry row — e.g. the unsubscribed-room fallback, or a slash command's
    // server call) is reported by report_unsent_message_, which puts the text
    // back.
    //
    // Routed through send_pipeline_: bundled URL previews (when enabled) are
    // generated on the read pool first, so they never hold up mut_pool_.
    auto sess = acting;
    auto rid = room_id;
    auto body_copy = body;
    auto fmt_copy = formatted_body;
    submit_room_send_(sess, rid, body,
                      [this, sess, rid, body_copy, fmt_copy](const std::string& previews) mutable {
        if (!sess || !sess->client) return;
        const auto result = tesseract::dispatch_compose_send(
            *sess->client, rid, body_copy, fmt_copy, previews);
        report_unsent_message_(sess->user_id, rid, body_copy, result);
        // Commands with no visible echo of their own confirm success here.
        if (result.ok)
        {
            if (auto msg = tesseract::slash_success_message(body_copy))
                post_to_ui_alive_([this, m = std::move(*msg)]
                                  { show_status_message_(m, 4000); });
        }
    });
    out.send_result = tesseract::Result{true, ""};
    return out;
}

// Unified slash-command dispatch ladder shared by every composer send path
// (the four shells' on_send handlers and RoomWindowBase::send_message_).
// Recognizes the no-arg /myroomavatar (native file picker via
// pick_and_set_room_avatar_), /leave, /join <room>, /invite <user>; any
// other input falls through to dispatch_compose_send (which itself handles
// /me, /shrug, /myroomnick, /myroomavatar <uri>, /spoiler and normal text).
// Must be called on the UI thread; the command branches enqueue async work
// via the existing ShellBase helpers.
// Called from a send worker with the result of a send that was already
// cleared from the composer. On a failure (other than cancellation) it
// shows the error and puts `body` back into that room's composer, or
// into its saved draft when the composer isn't on screen or already has
// new text. The SDK's retry row only exists for messages that reached
// the send queue, so without this an early failure lost the text.
void ShellBase::report_unsent_message_(const std::string& user_id,
                                       const std::string& room_id,
                                       const std::string& body,
                                       const tesseract::Result& result)
{
    // "cancelled": the account is being torn down (logout / shutdown), so
    // there is no composer left to restore into.
    if (result.ok || result.message == "cancelled")
        return;
    post_to_ui_(
        [this, uid = user_id, rid = room_id, text = body, err = result.message]
        {
            show_status_message_(
                tk::trf(tk::tr("Message not sent: {0}"), {err}), 8000);
            // Only the account that sent it gets its text back: its own
            // pop-out of the room first (whether or not it is active), then
            // the main window if that account is the active one.
            if (auto* w = find_secondary_(rid, uid);
                w && w->pane()->restore_unsent_text_(text))
                return;
            if (!active_account_ || active_account_->user_id != uid)
                return;
            if (current_room_id_ == rid && main_room_pane_ &&
                main_room_pane_->restore_unsent_text_(text))
                return;
            // Room not on screen (or its composer has new text): keep the
            // message as that room's draft, restored when the user returns.
            if (main_room_pane_ && current_room_id_ != rid)
                main_room_pane_->stash_unsent_draft_(rid, text);
        });
}

// Called by a platform notifier's activation/response callback when the
// user submitted inline reply text from an OS notification. Resolves the
// AccountSession that owns `user_id` via account_manager_ (does NOT touch
// active_account_ or navigate — a background reply must not disturb
// whatever account/room is currently showing, matching macOS's
// non-foregrounding action and KDE's own reply UX). Sends as a threaded
// reply when `event_id` is non-empty, else falls back to a plain
// message. Failures are reported via a follow-up notification (see
// notify_reply_failed_), not show_status_message_, since the triggering
// notification may belong to a different account/window than whichever
// one is currently focused.
// Queue a text send for `room_id` through send_pipeline_. When bundled
// URL previews are enabled and `preview_body` may contain a link, the
// previews are generated on the read pool first and handed to `send` as
// JSON; otherwise `send` gets an empty string. Sends for one room keep
// their submission order either way. UI thread only.
void ShellBase::submit_room_send_(const std::shared_ptr<AccountSession>& sess,
                                  const std::string& room_id,
                                  const std::string& preview_body,
                                  SendPipeline::Send send)
{
    // Cheap UI-thread pre-filter so plain sends skip the read-pool hop; the
    // Rust side re-checks the opt-in and does the real link detection.
    // Slash-command bodies are skipped: their sent text differs from what
    // was typed (e.g. /spoiler), so a preview could leak hidden content.
    SendPipeline::Prepare prepare;
    if (tesseract::Settings::instance().send_bundled_url_previews && sess &&
        !preview_body.empty() && preview_body[0] != '/' &&
        preview_body.find("http") != std::string::npos)
    {
        prepare = [sess, room_id, preview_body]() -> std::string
        {
            if (!sess || !sess->client)
                return {};
            return sess->client->generate_url_previews(room_id, preview_body);
        };
    }
    send_pipeline_.submit(room_id, std::move(prepare), std::move(send));
}

void ShellBase::refresh_send_busy_ui_()
{
    auto apply = [this](const std::string& room_id, views::RoomView* rv)
    {
        if (!rv || !rv->compose_bar())
            return;
        rv->compose_bar()->set_send_busy(!room_id.empty() &&
                                         send_pipeline_.is_busy(room_id));
    };
    if (main_room_pane_)
        apply(main_room_pane_->room_id(), main_room_pane_->room_view());
    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w)
            apply(rid, w->room_view());
    }
}

void ShellBase::refresh_room_list_()
{
    if (!main_app_)
        return;
    auto* rlv = main_app_->room_list_view();
    if (!rlv)
        return;

    if (is_room_search_active_())
    {
        main_app_->set_space_nav(false);
        // Search is only reachable when space_stack_ is empty, so there are
        // no unjoined-room subscriptions to cancel and no space section to clear.
        rlv->set_rooms(rooms_);
        if (!current_room_id_.empty())
            rlv->set_selected_room(current_room_id_);
        request_relayout_();
        return;
    }

    if (space_stack_.empty())
    {
        const bool group_unread =
            tesseract::Settings::instance().group_unread_rooms;

        auto filtered = views::filter_root_rooms(
            rooms_, space_children_cache_, group_unread);

        apply_space_child_counts_(filtered);

        main_app_->set_space_nav(false);
        main_app_->room_list_view()->clear_space_unjoined_rooms();
        cancel_unjoined_summaries_();

        rlv->set_rooms(filtered);
    }
    else
    {
        const std::string& space_id = space_stack_.back();
        static const std::vector<std::string> kNoChildren;
        const auto sc_it = space_children_cache_.find(space_id);
        const auto& child_ids =
            sc_it != space_children_cache_.end() ? sc_it->second : kNoChildren;

        std::vector<tesseract::RoomInfo> filtered;
        filtered.reserve(child_ids.size());
        for (const auto& r : rooms_)
        {
            if (std::find(child_ids.begin(), child_ids.end(), r.id) !=
                child_ids.end())
                filtered.push_back(r);
        }

        for (const auto& r : rooms_)
        {
            if (r.id == space_id)
            {
                ensure_room_avatar_(r);
                main_app_->set_space_nav(true, r.name, r.avatar_url);
                break;
            }
        }
        const auto& unjoined = get_cached_unjoined_summaries_(space_id);
        main_app_->room_list_view()->set_space_unjoined_rooms(
            std::vector<tesseract::RoomSummary>(unjoined));

        rlv->set_rooms(filtered);
    }

    if (!current_room_id_.empty())
        rlv->set_selected_room(current_room_id_);

    request_relayout_();
}

void ShellBase::handle_room_action_complete_ui_(std::uint64_t request_id,
                                                 bool ok,
                                                 std::string joined_room_id,
                                                 std::string message)
{
    if (auto iit = pending_invites_.find(request_id); iit != pending_invites_.end())
    {
        PendingInvite inv = std::move(iit->second);
        pending_invites_.erase(iit);
        if (inv.done)
        {
            inv.done(ok, message);
        }
        else if (!ok)
        {
            std::string status = tk::trf(tk::tr("Couldn't invite {0}"), {inv.user_id});
            if (!message.empty())
                status += ": " + message;
            show_status_message_(std::move(status));
        }
        return;
    }

    if (auto mit = pending_moderations_.find(request_id);
        mit != pending_moderations_.end())
    {
        PendingModeration mod = std::move(mit->second);
        pending_moderations_.erase(mit);
        if (!ok)
        {
            const std::string& who =
                mod.display_name.empty() ? mod.user_id : mod.display_name;
            std::string status;
            switch (mod.action)
            {
            case ModerationAction::Kick:
                status = tk::trf(tk::tr("Couldn't kick {0}"), {who});
                break;
            case ModerationAction::Ban:
                status = tk::trf(tk::tr("Couldn't ban {0}"), {who});
                break;
            case ModerationAction::Unban:
                status = tk::trf(tk::tr("Couldn't unban {0}"), {who});
                break;
            }
            if (!message.empty())
                status += ": " + message;
            show_status_message_(std::move(status));
        }
        if (mod.done)
            mod.done(ok);
        return;
    }

    auto it = pending_room_actions_.find(request_id);
    if (it == pending_room_actions_.end())
        return;
    auto [room_id, kind, action_space_id] = std::move(it->second);
    pending_room_actions_.erase(it);

    if (!ok)
    {
        std::fprintf(stderr, "[room-action] failed for %s: %s\n",
                     room_id.c_str(), message.c_str());
        // A failed join must not leave a permalink's pending event-scroll
        // resident — otherwise it would fire on some later, unrelated join of
        // the same room and yank the timeline to a stale event.
        if (kind == RoomActionKind::Join)
            pending_event_scroll_after_join_.erase(room_id);
        std::string verb = tk::tr("complete room action");
        switch (kind)
        {
        case RoomActionKind::Accept:
            verb = tk::tr("accept invite");
            break;
        case RoomActionKind::Join:
            verb = tk::tr("join room");
            break;
        case RoomActionKind::Leave:
            verb = tk::tr("leave room");
            break;
        case RoomActionKind::LeaveSpace:
            verb = tk::tr("leave space");
            break;
        case RoomActionKind::Create:
            verb = tk::tr("create room");
            break;
        case RoomActionKind::Knock:
            verb = tk::tr("send join request");
            break;
        case RoomActionKind::AcceptKnock:
            verb = tk::tr("accept join request");
            break;
        case RoomActionKind::AddSpaceChild:
            verb = tk::tr("add room to space");
            break;
        case RoomActionKind::RemoveSpaceChild:
            verb = tk::tr("remove room from space");
            break;
        }
        std::string status = tk::trf(tk::tr("Couldn't {0}"), {verb});
        if (!message.empty())
            status += ": " + message;
        show_status_message_(std::move(status));
        if (kind == RoomActionKind::Join || kind == RoomActionKind::Knock)
            on_join_room_outcome_ui_(false, room_id, message);
        else if (kind == RoomActionKind::Create)
            on_create_room_outcome_ui_(false, room_id, message);
        else if (kind == RoomActionKind::AddSpaceChild ||
                 kind == RoomActionKind::RemoveSpaceChild)
        {
            // Revert the optimistic cache mutation request_add/remove_
            // room_to/from_space_ already applied, then re-push the
            // (now-reverted) child set to the UI.
            const bool was_add = (kind == RoomActionKind::AddSpaceChild);
            if (was_add)
            {
                auto& joined = space_children_cache_[action_space_id];
                joined.erase(std::remove(joined.begin(), joined.end(), room_id),
                            joined.end());
            }
            else
            {
                // We don't know whether the removed child was joined or
                // unjoined at the time of the request — re-derive both from
                // the still-authoritative last-known-good source (a fresh
                // update_space_children_cache_() pass) rather than guessing
                // which vector to restore it to.
                update_space_children_cache_();
            }
            // refresh_space_root_children_ itself checks whether
            // action_space_id is the one currently shown.
            refresh_space_root_children_(action_space_id);
            refresh_room_list_();
        }
        return;
    }

    switch (kind)
    {
    case RoomActionKind::Accept:
        tab_select_room(room_id);
        break;
    case RoomActionKind::Join:
    {
        const std::string& effective_id =
            joined_room_id.empty() ? room_id : joined_room_id;
        if (!joined_room_id.empty())
        {
            tab_navigate_room(joined_room_id);
            // If this room was joined by following an event permalink, jump to
            // the target event now that we're in.
            auto pit = pending_event_scroll_after_join_.find(joined_room_id);
            if (pit != pending_event_scroll_after_join_.end())
            {
                const std::string ev = std::move(pit->second);
                pending_event_scroll_after_join_.erase(pit);
                if (room_view_ && room_view_->message_list())
                    room_view_->message_list()->set_highlighted_event(ev);
                try_scroll_to_room_event_(ev);
            }
        }
        on_join_room_outcome_ui_(true, effective_id, "");
        break;
    }
    case RoomActionKind::Create:
    {
        const std::string& effective_id =
            joined_room_id.empty() ? room_id : joined_room_id;
        if (!joined_room_id.empty())
            tab_navigate_room(joined_room_id);
        on_create_room_outcome_ui_(true, effective_id, "");
        break;
    }
    case RoomActionKind::Leave:
        if (tabs_.size() > 1)
        {
            tab_close(room_id);
        }
        else
        {
            // Deselecting the active room: tear down the thread panel and let
            // after_active_room_changed_() cancel the leaving room's pending
            // media downloads, mirroring the canonical deselect path.
            if (main_room_pane_)
            {
                auto _tt = compute_thread_transition_(
                    main_room_pane_->thread_panel(),
                    main_room_pane_->thread_panel_prev(),
                    main_room_pane_->thread_root(),
                    ThreadTrigger::RoomSwitch, {});
                main_room_pane_->apply_thread_transition_(_tt);
            }
            current_room_id_.clear();
            after_active_room_changed_();
            if (room_view_)
                room_view_->clear_room();
            request_relayout_();
        }
        break;
    case RoomActionKind::LeaveSpace:
        leave_space_navigate_back_(room_id);
        request_relayout_();
        break;
    case RoomActionKind::Knock:
        // Not joined yet — nothing to navigate to. Close the Join dialog
        // (mirrors a successful Join) and confirm via a status toast; the
        // "Requests to Join" list reflects the pending knock once
        // on_my_knocks_updated_ fires from the next sync tick.
        show_status_message_(tk::tr("Request sent"));
        on_join_room_outcome_ui_(true, room_id, "");
        break;
    case RoomActionKind::AcceptKnock:
        // No navigation — the admin didn't join anything. The requester
        // becomes an invited/joined member on the next sync tick, and
        // on_knock_requests_updated will drop them from the panel.
        break;
    case RoomActionKind::AddSpaceChild:
    case RoomActionKind::RemoveSpaceChild:
        // No extra work — request_add/remove_room_to/from_space_ already
        // applied the optimistic UI update before this call was even made;
        // the real m.space.child state lands via the next sync tick like
        // any other state change, which update_space_children_cache_()
        // picks up through its existing polling/refresh triggers.
        break;
    }
}

// Called after handle_room_action_complete_ui_() processes a Join action.
// ok=true means the join succeeded; room_id is the canonical joined room
// ID; message is the SDK failure message (empty on success). Resets
// RoomPreviewView's Join button on failure and — if AddRoomView's Join
// tab triggered this action — closes the dialog on success or surfaces
// the failure in JoinRoomView on failure. No shell needs to override
// this; it's virtual only so a shell could extend it if ever needed.
void ShellBase::on_join_room_outcome_ui_(bool ok, const std::string& room_id,
                                         const std::string& message)
{
    if (!ok && main_app_ && main_app_->room_preview())
        main_app_->room_preview()->set_state(views::RoomPreviewView::State::Idle);

    auto* ar = main_app_ ? main_app_->add_room_view() : nullptr;
    if (!ar || !ar->is_open())
        return;

    if (ar->active_tab() == views::AddRoomView::Tab::Directory)
    {
        if (auto* dv = ar->directory_view())
        {
            if (ok)
                ar->close();
            else
                dv->set_join_failed(message.empty() ? std::string() : message);
            request_relayout_();
        }
        return;
    }

    auto* jr = ar->join_view();
    if (!jr || ar->active_tab() != views::AddRoomView::Tab::Join)
        return;

    if (ok)
    {
        ar->close();
    }
    else
    {
        jr->set_error(message.empty() ? std::string() : message);
    }
    request_relayout_();
}

void ShellBase::on_create_room_outcome_ui_(bool ok, const std::string& room_id,
                                           const std::string& message)
{
    (void)room_id;
    auto* ar = main_app_ ? main_app_->add_room_view() : nullptr;
    auto* cr = ar ? ar->create_view() : nullptr;
    if (!cr || !ar->is_open() || ar->active_tab() != views::AddRoomView::Tab::Create)
        return;

    if (ok)
    {
        ar->close();
    }
    else
    {
        cr->set_error(message.empty() ? std::string() : message);
    }
    request_relayout_();
}

void ShellBase::handle_upload_complete_ui_(std::uint64_t request_id,
                                            bool ok,
                                            std::string message)
{
    on_upload_finished_ui_(request_id, ok);
    if (!ok)
    {
        std::fprintf(stderr, "[upload] failed: %s\n", message.c_str());
        std::string status = message.empty()
                                 ? tk::tr("Upload failed")
                                 : tk::trf(tk::tr("Upload failed: {0}"), {message});
        show_status_message_(std::move(status));
    }
}

void ShellBase::handle_upload_progress_ui_(std::uint64_t request_id,
                                            std::uint64_t current_bytes,
                                            std::uint64_t total_bytes)
{
    on_upload_progress_ui_(request_id, current_bytes, total_bytes);
}

void ShellBase::push_room_list_state_(RoomListState state)
{
    last_room_list_state_ = state;
    // client_ (the active account's alias) may still be null here during
    // startup: bridge construction + start_sync now run on a worker thread
    // ahead of switch_active_account_impl_, so a fast-restoring account's
    // sync engine can reach Running and post this callback to the UI thread
    // before the active account has been chosen at all. trigger_update_check_
    // needs *client_ to make the request, so defer — update_check_triggered_
    // is only consumed once client_ is actually available (see the matching
    // check in switch_active_account_impl_, which covers the case where this
    // one-shot Running transition already happened before client_ was set).
    if (state == RoomListState::Running && client_ &&
        tesseract::Settings::instance().check_for_updates)
        trigger_update_check_();
}

void ShellBase::handle_knock_requests_updated_ui_(std::string room_id)
{
    auto sess = knock_requests_panel_account_.lock();
    if (!sess || !sess->client || room_id != knock_requests_panel_room_id_ ||
        dispatch_account_() != sess->user_id)
        return; // stale poke from a room whose panel isn't (or is no longer) open
    current_room_knock_requests_ = sess->client->list_knock_requests(room_id);
    on_knock_requests_panel_updated_();
}

// Called after current_room_knock_requests_ changes — either a fresh
// pull from Client::list_knock_requests (handle_knock_requests_updated_ui_)
// or a local optimistic edit (decline_knock_request_async_ et al).
// Implemented directly in ShellBase.cpp (not per-shell like
// on_invites_updated_) since it only needs main_app_, which every shell
// already exposes uniformly.
void ShellBase::on_knock_requests_panel_updated_()
{
    if (!main_app_)
        return;
    if (auto* rv = main_app_->room_view())
    {
        if (auto* panel = rv->knock_requests_panel())
        {
            panel->set_requests(current_room_knock_requests_);
        }
    }
}

void ShellBase::send_current_location_(std::string room_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || !acting || !acting->client)
        return;

    if (!location_provider_)
    {
        location_provider_ = tk::create_location_provider(
            [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); });
    }
    if (!location_provider_)
    {
        show_status_message_(
            tk::tr("Location services aren't available on this device."));
        return;
    }

    // Feedback that the command was recognized — GeoClue2/CoreLocation/WinRT
    // lookups can take a few seconds. Persistent (auto_clear_ms = 0) so it
    // isn't racily cleared before the fix lands; the completion callback's
    // own show_status_message_ call (success or failure) supersedes it.
    show_status_message_(tk::tr("Fetching your location\xe2\x80\xa6"), 0);

    auto sess = acting;
    location_provider_->request_current_location(
        [this, sess, room_id](bool success, const tk::LocationFix& fix,
                              tk::LocationError error)
        {
            if (!success)
            {
                std::string msg;
                switch (error)
                {
                case tk::LocationError::PermissionDenied:
                    msg = tk::tr("Location permission denied.");
                    break;
                case tk::LocationError::Unavailable:
                    msg = tk::tr("Location is currently unavailable.");
                    break;
                case tk::LocationError::Timeout:
                    msg = tk::tr("Timed out waiting for your location.");
                    break;
                default:
                    msg = tk::tr("Couldn't get your location.");
                    break;
                }
                show_status_message_(std::move(msg));
                return;
            }
            if (!sess || !sess->client)
                return; // account torn down / switched while awaiting the fix
            std::string body = tesseract::views::osm_view_url(
                fix.latitude, fix.longitude, 15);
            // Supersede the persistent "Fetching…" message now that we have
            // a fix — send_location() itself is fire-and-forget (no
            // completion callback), so this is shown optimistically.
            show_status_message_(tk::tr("Location sent."));
            run_async_mut_([sess, room_id, lat = fix.latitude,
                            lon = fix.longitude, body]() mutable {
                if (!sess || !sess->client) return;
                sess->client->send_location(room_id, lat, lon, body);
            });
        });
}

void ShellBase::send_notification_reply_(std::string user_id,
                                         std::string room_id,
                                         std::string event_id,
                                         std::string text)
{
    text = tesseract::text::trim(text);
    if (text.empty())
        return;

    auto sess = account_manager_.find(user_id);
    if (!sess || !sess->client)
    {
        notify_reply_failed_(user_id, room_id,
                             tk::tr("You're signed out of this account"));
        return;
    }

    // Through the pipeline so a quick-reply keeps its order among this
    // room's sends and gets bundled URL previews like a composer send.
    submit_room_send_(sess, room_id, text,
                      [this, sess, room_id, event_id, text](const std::string& previews) mutable {
        auto res = event_id.empty()
            ? sess->client->send_message(room_id, text, "", previews)
            : sess->client->send_reply(room_id, event_id, text, "", previews);
        if (res)
            return;
        post_to_ui_(guarded([this, uid = sess->user_id, room_id]() mutable {
            notify_reply_failed_(
                uid, room_id,
                tk::tr("Couldn't send your reply. Try again from the app."));
        }));
    });
}
} // namespace tesseract
