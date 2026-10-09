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

void ShellBase::dispatch_timeline_reset_secondary_(
    const std::string& room_id,
    const EventList& snapshot)
{
    dispatch_to_secondary_windows_(room_id,
                                   [&](RoomWindowBase* w)
                                   {
                                       w->on_timeline_reset(
                                           build_rows_(snapshot, room_id));
                                   });
}

void ShellBase::dispatch_message_inserted_secondary_(const std::string& room_id,
                                                     std::size_t index,
                                                     const Event& ev)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w)
        {
            prep_row_media_(ev);
            if (!ev.in_reply_to_id.empty() && w->pane())
            {
                // See build_rows_'s doc comment: room_id here is w's own
                // room, not necessarily current_room_id_, so this must go
                // through the general (room_id-explicit) overload.
                w->pane()->ensure_reply_details_(room_id, ev.event_id,
                                                       std::string());
            }
            w->on_message_inserted(index,
                                   views::make_row_data(ev, w->owner_user_id()));
        });
}

void ShellBase::dispatch_message_prepended_secondary_(const std::string& room_id,
                                                       const Event& ev)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w)
        {
            prep_row_media_(ev);
            if (!ev.in_reply_to_id.empty() && w->pane())
            {
                // See build_rows_'s doc comment: room_id here is w's own
                // room, not necessarily current_room_id_, so this must go
                // through the general (room_id-explicit) overload.
                w->pane()->ensure_reply_details_(room_id, ev.event_id,
                                                       std::string());
            }
            w->on_message_prepended(views::make_row_data(ev, w->owner_user_id()));
        });
}

void ShellBase::dispatch_message_appended_secondary_(const std::string& room_id,
                                                      const Event& ev)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w)
        {
            prep_row_media_(ev);
            if (!ev.in_reply_to_id.empty() && w->pane())
            {
                // See build_rows_'s doc comment: room_id here is w's own
                // room, not necessarily current_room_id_, so this must go
                // through the general (room_id-explicit) overload.
                w->pane()->ensure_reply_details_(room_id, ev.event_id,
                                                       std::string());
            }
            w->on_message_appended(views::make_row_data(ev, w->owner_user_id()));
        });
}

void ShellBase::dispatch_message_updated_secondary_(const std::string& room_id,
                                                    std::size_t index,
                                                    const Event& ev)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w)
        {
            prep_row_media_(ev);
            if (!ev.in_reply_to_id.empty() && w->pane())
            {
                // See build_rows_'s doc comment: room_id here is w's own
                // room, not necessarily current_room_id_, so this must go
                // through the general (room_id-explicit) overload.
                w->pane()->ensure_reply_details_(room_id, ev.event_id,
                                                       std::string());
            }
            w->on_message_updated(index, views::make_row_data(ev, w->owner_user_id()));
        });
}

void ShellBase::dispatch_message_removed_secondary_(const std::string& room_id,
                                                    std::size_t index)
{
    dispatch_to_secondary_windows_(room_id,
                                   [&](RoomWindowBase* w)
                                   {
                                       w->on_message_removed(index);
                                   });
}

void ShellBase::update_secondary_room_infos_()
{
    // Each pop-out gets its room from its own account's room list.
    for (const auto& [rid, w] : secondary_windows_)
    {
        if (!w)
            continue;
        for (const auto& r : rooms_for_(w->owner_user_id()))
        {
            if (r.id == rid)
            {
                w->on_room_info_updated(r);
                break;
            }
        }
    }
}

void ShellBase::register_room_window_(RoomWindowBase* w)
{
    secondary_windows_.emplace(w->room_id(), w);
    // See active_popout_media_groups_'s doc comment: this room's ordinary
    // timeline media must not be dropped by should_deliver_ just because
    // it isn't the main window's current room.
    active_popout_media_groups_.insert(media_group_for_room_(w->room_id()));
}

void ShellBase::unregister_room_window_(RoomWindowBase* w)
{
    auto [first, last] = secondary_windows_.equal_range(w->room_id());
    for (auto it = first; it != last; ++it)
    {
        if (it->second != w)
            continue;
        secondary_windows_.erase(it);
        // Another account's pop-out of the same room still needs the group.
        if (secondary_windows_.count(w->room_id()) == 0)
            active_popout_media_groups_.erase(media_group_for_room_(w->room_id()));
        break;
    }
}

RoomWindowBase* ShellBase::find_secondary_(const std::string& room_id,
                                           const std::string& user_id) const
{
    auto [first, last] = secondary_windows_.equal_range(room_id);
    for (auto it = first; it != last; ++it)
        if (it->second && it->second->owner_user_id() == user_id)
            return it->second;
    return nullptr;
}

RoomWindowBase* ShellBase::find_event_secondary_(const std::string& room_id) const
{
    return find_secondary_(room_id, dispatch_account_());
}

std::vector<std::pair<std::string, RoomWindowBase*>>
ShellBase::active_account_popouts_() const
{
    const std::string uid = active_account_ ? active_account_->user_id : std::string{};
    std::vector<std::pair<std::string, RoomWindowBase*>> out;
    for (const auto& [rid, w] : secondary_windows_)
        if (w && w->owner_user_id() == uid)
            out.emplace_back(rid, w);
    return out;
}

RoomPane* ShellBase::reply_pane_for_(const std::string& room_id) const
{
    if (event_is_for_active_account_())
        return main_room_pane_.get();
    auto* w = find_event_secondary_(room_id);
    return w ? w->pane() : nullptr;
}

bool ShellBase::focus_secondary_window_(const std::string& room_id)
{
    if (auto* w = find_secondary_(room_id, dispatch_account_()))
    {
        w->bring_to_front();
        return true;
    }
    return false;
}

bool ShellBase::room_pinned_by_popout_(const std::string& room_id) const
{
    if (!active_account_)
        return false;
    auto it = room_subscription_refs_.find(
        subscription_key_(active_account_->user_id, room_id));
    return it != room_subscription_refs_.end() && it->second > 0;
}

void ShellBase::close_popouts_for_account_(const std::string& user_id)
{
    // Collect first: each window's destructor unregisters itself.
    std::vector<RoomWindowBase*> doomed;
    for (const auto& w : owned_secondary_windows_)
        if (w && w->owner_user_id() == user_id)
            doomed.push_back(w.get());
    for (auto* w : doomed)
        release_owned_window_(w);
}

// Subscription ref-counting for secondary windows. acquire_() starts an
// async subscribe_room when the ref goes from 0→1 (unless the main window
// already holds the subscription). release_() unsubscribes when the ref
// goes from 1→0 and the main window is not showing that room.
// `sess` is the pop-out's own account: the subscription is made on its
// client, whether or not that account is the active one.
void ShellBase::acquire_room_subscription_(
    const std::shared_ptr<AccountSession>& sess, const std::string& room_id)
{
    if (!sess)
        return;
    int& refs = room_subscription_refs_[subscription_key_(sess->user_id, room_id)];
    if (++refs > 1)
    {
        return;
    }
    // If the main window is already showing this room for this account, its
    // subscription is live.
    if (room_id == current_room_id_ && sess == active_account_)
    {
        return;
    }
    run_async_mut_(
        [sess, room_id]
        {
            if (sess && sess->client)
            {
                sess->client->subscribe_room(room_id);
            }
        });
}

void ShellBase::release_room_subscription_(
    const std::shared_ptr<AccountSession>& sess, const std::string& room_id)
{
    if (!sess)
        return;
    auto it = room_subscription_refs_.find(subscription_key_(sess->user_id, room_id));
    if (it == room_subscription_refs_.end())
    {
        return;
    }
    if (--it->second > 0)
    {
        return;
    }
    room_subscription_refs_.erase(it);
    // The room's timeline is torn down below, so its scroll-back state for
    // this account no longer applies.
    if (sess != active_account_)
    {
        if (auto acct = other_account_pagination_.find(sess->user_id);
            acct != other_account_pagination_.end())
            acct->second.erase(room_id);
    }
    if (room_id == current_room_id_ && sess == active_account_)
    {
        return;
    }
    run_async_mut_(
        [sess, room_id]
        {
            if (sess && sess->client)
            {
                sess->client->unsubscribe_room(room_id);
            }
        });
}

void ShellBase::dispatch_to_secondary_windows_(
    const std::string& room_id, const std::function<void(RoomWindowBase*)>& fn)
{
    if (auto* w = find_event_secondary_(room_id))
        fn(w);
}

bool ShellBase::popout_accepts_event_(const RoomWindowBase* w) const
{
    return w && w->owner_user_id() == dispatch_account_();
}

void ShellBase::open_room_in_new_window(
    const std::string& room_id_in, const std::shared_ptr<AccountSession>& on_behalf_of)
{
    if (room_id_in.empty())
    {
        return;
    }
    // Copy first: tab_close() below can rebuild the tab bar via
    // on_tab_state_changed_ui_(), freeing a caller-held reference that
    // aliases a TabState's room_id string (see tab_popout_room's identical
    // precedent/comment).
    const std::string room_id = room_id_in;
    // The account the window is for: the caller's pop-out account, or the
    // active one.
    const auto acting = acting_session_(on_behalf_of);
    const std::string uid = acting ? acting->user_id : std::string{};
    if (auto* existing = find_secondary_(room_id, uid))
    {
        existing->bring_to_front();
        return;
    }
    // Deactivate the tab in this window before opening the pop-out, so the
    // room isn't shown open (and active) in both places at once — mirrors
    // tab_popout_room()'s tab_close()-then-open sequence, just triggered
    // here too (e.g. the room-list row's "Open in window" action, which
    // doesn't go through tab_popout_room). tab_close() itself now handles
    // the only-tab-open case by deselecting to the empty/BrandView state
    // rather than no-op'ing.
    if (acting == active_account_ && room_open_in_tab(room_id))
    {
        tab_close(room_id);
    }
    // RoomWindowBase's constructor takes its owner from here (see
    // pending_popout_owner_): the platform factory has no parameter for it.
    pending_popout_owner_ = acting;
    RoomWindowBase* w = create_secondary_room_window_(room_id);
    pending_popout_owner_.reset();
    if (w)
    {
        // A pop-out opened while in dark mode (with no later theme change)
        // would otherwise be stuck on its constructor's default theme.
        w->apply_theme(current_theme_);
        owned_secondary_windows_.emplace_back(w);

        // Hide the call button immediately if a call is already in progress.
        if (call_session_)
        {
            if (w->room_view() && w->room_view()->header())
                w->room_view()->header()->set_call_active(true);
        }
        refresh_call_banners_(); // the pop-out may show a room with a live call

        // Record that this room is now open as a popout so it can be
        // restored on the next launch. Geometry is written by the window
        // itself on resize/move; initialise with valid=false so the first
        // save_popout_geometry_() call writes real coordinates.
        auto& pops = Settings::instance().popout_windows;
        // Same match as RoomWindowBase's own lookups: this account's entry,
        // or one saved before entries recorded an account. An old entry is
        // claimed for this account rather than duplicated — otherwise
        // closing the window would remove only one of the two, and the
        // other would reopen it on the next launch.
        auto pit = std::find_if(pops.begin(), pops.end(),
                                [&room_id, &uid](const Settings::PopoutEntry& e)
                                {
                                    return e.room_id == room_id &&
                                           (e.user_id.empty() || e.user_id == uid);
                                });
        if (pit != pops.end() && pit->user_id.empty() && !uid.empty())
        {
            pit->user_id = uid;
            save_settings_debounced_();
        }
        else if (pit == pops.end())
        {
            Settings::PopoutEntry e;
            e.room_id = room_id;
            e.user_id = uid;
            pops.push_back(std::move(e));
            save_settings_debounced_();
        }
    }
}

void ShellBase::release_owned_window_(RoomWindowBase* w)
{
    auto it = std::find_if(owned_secondary_windows_.begin(),
                           owned_secondary_windows_.end(),
                           [w](const auto& up)
                           {
                               return up.get() == w;
                           });
    if (it != owned_secondary_windows_.end())
    {
        owned_secondary_windows_.erase(it); // unique_ptr destructor runs here
    }
}

void ShellBase::notify_secondary_media_ready_(const std::string& cache_key,
                                              MediaKind kind)
{
    // The sticker picker loads through the timeline's lazy sticker path, which
    // has no picker-specific completion step: repaint it here so a sticker
    // that lands after its cell was painted shows up.
    if (kind == MediaKind::Sticker)
    {
        repaint_pickers_();
    }
    for (const auto& [rid, w] : secondary_windows_)
    {
        views::RoomView* rv = w->room_view();
        if (!rv)
        {
            continue;
        }
        switch (kind)
        {
        case MediaKind::MediaImage:
        case MediaKind::MediaThumbnail:
        case MediaKind::Sticker:
        case MediaKind::Reaction:
            rv->notify_image_ready(cache_key);
            w->request_relayout();
            break;
        case MediaKind::Tile:
            if (auto* ml = rv->message_list())
            {
                ml->invalidate_data();
            }
            w->request_relayout();
            break;
        case MediaKind::RoomAvatar:
        case MediaKind::UserAvatar:
            // No height change; a relayout/repaint pulls the new avatar from
            // the shared cache on next paint.
            w->request_relayout();
            break;
        }
    }
}

void ShellBase::dispatch_gif_to_secondary_windows_(
    std::uint64_t request_id, const std::vector<GifResult>& results)
{
    for (const auto& [rid, w] : secondary_windows_)
    {
        w->on_gif_results(request_id, results); // copy per window
    }
}

void ShellBase::dispatch_gif_failed_to_secondary_windows_(
    std::uint64_t request_id, const std::string& message)
{
    for (const auto& [rid, w] : secondary_windows_)
    {
        w->on_gif_search_failed(request_id, message);
    }
}

std::vector<std::uint8_t>
ShellBase::cached_gif_source_bytes_(const std::string& url) const
{
    return load_media_bytes_(tk::CacheKey::gif_source(url));
}

bool ShellBase::any_window_visible_() const
{
    if (is_main_window_visible_()) return true;
    for (const auto& w : owned_secondary_windows_)
        if (w && w->is_visible()) return true;
    return false;
}

// Edge-detects the main window's visibility (keyed off
// is_main_window_visible_(), NOT any_window_visible_() — pop-out windows
// don't report their own visibility yet, so any_window_visible_() would
// never see the "hidden" edge while one is open) and pauses/resumes the
// main room view's inline autoplay video accordingly. No-op if the
// visibility state hasn't changed since the last call. Each shell calls
// this from every native show/hide/minimize/restore hook it has (see
// start_anim_tick_() call sites for the existing resume-side equivalents).
void ShellBase::update_video_playback_suspension_()
{
    const bool should_suspend = !is_main_window_visible_();
    if (should_suspend == video_playback_suspended_)
    {
        return;
    }
    video_playback_suspended_ = should_suspend;
    if (room_view_)
    {
        room_view_->set_message_list_video_suspended(should_suspend);
    }
}

void ShellBase::for_each_room_view_(
    const std::function<void(views::RoomView&)>& fn)
{
    if (room_view_)
        fn(*room_view_);
    for (auto& w : owned_secondary_windows_)
        if (w && w->room_view())
            fn(*w->room_view());
}

void ShellBase::close_all_popouts_()
{
    // owned_secondary_windows_ holds the lifetime; destroying each unique_ptr
    // runs ~RoomWindowBase (→ unregister_room_window_ + release_room_subscription_
    // + remove_popout_from_settings_) while secondary_windows_ /
    // room_subscription_refs_ are still alive — the same ordering ~ShellBase
    // relies on.
    teardown_activity_monitor_();
    owned_secondary_windows_.clear();
    secondary_windows_.clear(); // defensive; unregister already emptied it
    pending_restore_popouts_.clear();

    // Forget them permanently — a Clear-cache reset is a fresh start, not a
    // session bounce, so nothing should reopen on the next launch. Each
    // ~RoomWindowBase above already dropped its own live entry; sweep any
    // remaining entry for THIS account (a stale / not-yet-restored one),
    // leaving other accounts' pop-outs (in other windows) untouched — the
    // Settings::popout_windows list is a global singleton.
    auto& pops = Settings::instance().popout_windows;
    const auto before = pops.size();
    std::erase_if(pops, [this](const Settings::PopoutEntry& e)
                  { return e.user_id == my_user_id_; });
    if (pops.size() != before)
        save_settings_debounced_();
}


views::RoomView* ShellBase::room_view_for_room_(const std::string& room_id) const
{
    if (main_window_shows_(room_id))
        return room_view_;
    auto* w = find_event_secondary_(room_id);
    return w ? w->room_view() : nullptr;
}
} // namespace tesseract
