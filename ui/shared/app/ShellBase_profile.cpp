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

std::pair<bool, bool> ShellBase::compute_tray_unread(
    const std::unordered_map<std::string, std::vector<RoomInfo>>& by_account)
{
    return shell_helpers::compute_tray_unread(by_account);
}

bool ShellBase::account_has_unread(const std::vector<RoomInfo>& rooms)
{
    return shell_helpers::account_has_unread(rooms);
}

bool ShellBase::other_accounts_have_unread() const
{
    for (const auto& [uid, rooms] : per_account_rooms_)
    {
        if (uid == my_user_id_)
        {
            continue;
        }
        if (account_has_unread(rooms))
        {
            return true;
        }
    }
    return false;
}

bool ShellBase::account_has_unread_for(const std::string& user_id) const
{
    auto it = per_account_rooms_.find(user_id);
    if (it == per_account_rooms_.end())
    {
        return false;
    }
    return account_has_unread(it->second);
}

std::string ShellBase::find_existing_dm(const std::vector<RoomInfo>& rooms,
                                        const std::string&           user_id)
{
    return shell_helpers::find_existing_dm(rooms, user_id);
}

std::string ShellBase::find_existing_dm_(const std::string& user_id) const
{
    return find_existing_dm(rooms_, user_id);
}

void ShellBase::setup_link_clicked_(views::RoomView* rv)
{
    if (!rv) return;
    rv->on_link_clicked = [this](const std::string& url)
    {
        if (Client::parse_matrix_link(url).kind != Client::MatrixLink::Kind::Unknown)
            open_matrix_link(url);
        else
            Client::open_in_browser(url);
    };
}

void ShellBase::setup_dm_callbacks()
{
    if (!room_view_) return;
    room_view_->on_open_dm = [this](std::string user_id)
    {
        handle_open_dm_(std::move(user_id));
    };
    room_view_->on_has_dm = [this](const std::string& user_id)
    {
        return !find_existing_dm_(user_id).empty();
    };

    // Wire the UserProfilePanel extended-profile fetch. The panel is owned by
    // the widget tree (always live while room_view_ exists), so it is safe to
    // capture the raw pointer.
    if (auto* panel = room_view_->user_profile_panel())
    {
        panel->on_extended_profile_requested =
            [this, panel](std::string user_id)
        {
            fetch_user_extended_profile_async_(user_id, panel);
        };

        // The panel shows the avatar much larger than every other avatar
        // context (room list, message senders), so it needs the actual
        // source image rather than the shared small thumbnail — reuse the
        // image viewer's full-resolution fetch/cache instead of
        // RoomView::set_avatar_provider's shared thumbnail-only provider.
        panel->set_avatar_provider([this](const std::string& mxc) -> const tk::Image*
        {
            return viewer_image_lookup_(mxc);
        });
        panel->on_avatar_needed = [this](const std::string& mxc)
        {
            ensure_viewer_fullres_(mxc);
        };
        // The trust row's callbacks are wired per pane (RoomPane::
        // wire_room_view_, via main_room_pane_->attach()) — don't set
        // on_trust_requested / on_verify_user / on_withdraw_verification here.
    }
}

void ShellBase::handle_open_dm_(const std::string& user_id, const std::string& reason)
{
    if (user_id.empty() || !client_) return;

    // Fast path: DM already in rooms_ — navigate immediately.
    if (auto existing = find_existing_dm_(user_id); !existing.empty())
    {
        if (room_view_) room_view_->close_user_profile();
        navigate_to_room_(existing);
        return;
    }

    // In-flight guard: suppress duplicate async calls for the same user.
    if (dm_in_flight_user_ids_.count(user_id)) return;
    dm_in_flight_user_ids_.insert(user_id);

    // Show loading state while the async call runs.
    if (room_view_)
    {
        room_view_->set_dm_button_state(
            views::UserProfilePanel::DmButtonState::Sending);
        request_repaint_();
    }

    auto sess = active_account_;
    run_async_mut_([this, sess, user_id, reason]()
    {
        if (!sess || !sess->client) return;
        auto dm_id = sess->client->get_or_create_dm(user_id, reason);
        post_to_ui_alive_([this, user_id, dm_id = std::move(dm_id)]() mutable
        {
            dm_in_flight_user_ids_.erase(user_id);
            if (!dm_id.empty())
            {
                if (room_view_) room_view_->close_user_profile();
                navigate_to_room_(dm_id);
            }
            else
            {
                // Reset so the user can retry.
                if (room_view_)
                {
                    room_view_->set_dm_button_state(
                        views::UserProfilePanel::DmButtonState::Normal);
                    request_repaint_();
                }
            }
        });
    });
}

// ---------------------------------------------------------------------------
// Extended profile helpers (MSC4133)
// ---------------------------------------------------------------------------

void ShellBase::fetch_own_extended_profile_async_()
{
    if (!client_) return;
    // No entry in pending_user_profiles_ → handle_extended_profile_ready_ui_
    // treats this as the own-profile case.
    client_->get_extended_profile_async(next_request_id_++, my_user_id_);
}

void ShellBase::push_own_status_to_strip_()
{
    if (main_app_ && main_app_->user_info())
    {
        main_app_->user_info()->set_status_editable(
            server_info_.supports_profile_fields &&
            server_info_.profile_fields_enabled);
        main_app_->user_info()->set_status(own_extended_profile_.status_emoji,
                                           own_extended_profile_.status_text);
    }
}

void ShellBase::restart_app_()
{
    // Quitting drops the call; make the user end it deliberately, like
    // clear_all_caches_() does.
    if (active_call())
    {
        show_status_message_(tk::tr("End your call before restarting."));
        return;
    }
    tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
    if (!spawn_relaunch_(tesseract::relaunch_args()))
    {
        show_status_message_(tk::tr("Couldn't restart Tesseract. Please restart it yourself."));
        return;
    }
    quit_app_();
}

void ShellBase::open_settings_to_account_tab_()
{
    open_app_settings_ui_();
    if (stats_settings_view_)
        stats_settings_view_->show_account_section();
}

void ShellBase::dispatch_launch_action_(LaunchAction action, std::string room_id)
{
    if (action == LaunchAction::None)
    {
        return;
    }
    if (action == LaunchAction::Room)
    {
        // Empty when replaying a held Room action from
        // mark_main_content_ready_: the ID is already pending.
        if (!room_id.empty())
        {
            pending_launch_room_id_ = std::move(room_id);
        }
        if (pending_launch_room_id_.empty())
        {
            return;
        }
    }
    if (!main_content_ready_)
    {
        pending_launch_action_ = action;
        return;
    }
    raise_main_window_ui_();
    switch (action)
    {
    case LaunchAction::QuickSwitcher:
        open_quick_switch_ui_();
        break;
    case LaunchAction::MessageSearch:
        open_message_search_ui_();
        break;
    case LaunchAction::Settings:
        open_app_settings_ui_();
        break;
    case LaunchAction::Room:
        open_launch_room_(pending_launch_room_id_);
        break;
    case LaunchAction::None:
        break;
    }
}

void ShellBase::mark_main_content_ready_()
{
    main_content_ready_ = true;
    if (pending_launch_action_ != LaunchAction::None)
    {
        const auto action = pending_launch_action_;
        pending_launch_action_ = LaunchAction::None;
        dispatch_launch_action_(action);
    }
}

void ShellBase::retry_pending_launch_room_()
{
    // Only once a held action has run: before that, mark_main_content_ready_
    // replays it through dispatch_launch_action_ (which also raises the
    // window).
    if (main_content_ready_ && pending_launch_action_ == LaunchAction::None &&
        !pending_launch_room_id_.empty())
    {
        open_launch_room_(pending_launch_room_id_);
    }
}

void ShellBase::open_launch_room_(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    // Callers commonly pass pending_launch_room_id_ itself. Copy before
    // clearing that member so navigation never observes an invalidated alias.
    const std::string target_room_id = room_id;
    for (const auto& [user_id, rooms] : per_account_rooms_)
    {
        const bool found = std::any_of(
            rooms.begin(), rooms.end(),
            [&](const RoomInfo& room) { return room.id == target_room_id; });
        if (!found)
        {
            continue;
        }
        pending_launch_room_id_.clear();
        if (!active_account_ || active_account_->user_id != user_id)
        {
            switch_active_account_(user_id);
        }
        navigate_to_room_(target_room_id);
        return;
    }
    // Account restoration or the initial room snapshot may still be in
    // progress. push_rooms_ retries without opening a join prompt.
    pending_launch_room_id_ = target_room_id;
}

void ShellBase::handle_profile_field_change_(const std::string& key,
                                              const std::string& value_json)
{
    if (!client_) return;
    const auto request_id = next_request_id_++;
    pending_profile_field_writes_[request_id] = {key, value_json};
    client_->set_or_delete_profile_field_async(request_id, key, value_json);
}

void ShellBase::handle_profile_field_result_ui_(std::uint64_t request_id,
                                                 std::string key, bool ok,
                                                 std::string message)
{
    on_profile_field_result_ui_(key, ok, message);
    auto it = pending_profile_field_writes_.find(request_id);
    if (it == pending_profile_field_writes_.end())
        return;
    const std::string value_json = std::move(it->second.second);
    pending_profile_field_writes_.erase(it);
    // Apply the just-written value straight into the cache instead of
    // re-fetching the whole profile — see own_extended_profile_'s doc
    // comment for why a re-fetch here is racy.
    if (ok)
    {
        own_extended_profile_.apply_field(key, value_json);
        notify_own_extended_profile_changed_();
    }
}

void ShellBase::fetch_user_extended_profile_async_(const std::string& user_id,
                                                    views::UserProfilePanel* panel)
{
    if (!client_ || !panel) return;
    auto req_id = next_request_id_++;
    pending_user_profiles_[req_id] = panel;
    client_->get_extended_profile_async(req_id, user_id);
}

/// Resolve user_id's grammatical-gender pronoun word for gendered
/// membership-narration text (see the member_gender_cache_ comment
/// above). No-op if already cached or a fetch is already in flight for
/// this user_id — callers (MessageListView, via the shell) should call
/// this only from a currently-visible row that actually needs a pronoun,
/// never as a bulk/room-wide prefetch. Result arrives via
/// on_member_pronoun_ready_ui_(user_id).
void ShellBase::request_member_pronoun_ui_(const std::string& user_id)
{
    if (!client_ || user_id.empty())
        return;
    if (member_gender_cache_.count(user_id) || member_gender_inflight_.count(user_id))
        return;
    member_gender_inflight_.insert(user_id);
    auto req_id = next_request_id_++;
    pending_member_gender_requests_[req_id] = user_id;
    client_->get_extended_profile_async(req_id, user_id);
}

void ShellBase::handle_extended_profile_ready_ui_(std::uint64_t request_id,
                                                   std::string profile_json)
{
    // User-panel case: deliver to the requesting panel.
    auto pit = pending_user_profiles_.find(request_id);
    if (pit != pending_user_profiles_.end())
    {
        auto* panel = pit->second;
        pending_user_profiles_.erase(pit);
        auto p = tesseract::UserProfile::from_json(profile_json);
        tesseract::ExtendedProfile ep{p.pronouns,     p.tz,
                                      p.biography,    p.status_emoji,
                                      p.status_text,  p.call_joined_ts};
        panel->set_extended_profile(ep);
        return;
    }

    // Quick-switcher resolve case: gen check, then merge.
    auto rit = pending_resolve_requests_.find(request_id);
    if (rit != pending_resolve_requests_.end())
    {
        auto [mxid, gen] = rit->second;
        pending_resolve_requests_.erase(rit);
        if (user_resolve_gen_.load() == gen)
        {
            auto p = tesseract::UserProfile::from_json(profile_json);
            if (p.exists)
                merge_resolved_user_(p);
        }
        return;
    }

    // InviteDialog lookup: merge into the roster when found, and tell every
    // open dialog either way (not-found turns the row red).
    auto iit = pending_invite_resolves_.find(request_id);
    if (iit != pending_invite_resolves_.end())
    {
        auto [mxid, gen] = iit->second;
        pending_invite_resolves_.erase(iit);
        if (gen == 0)
            invite_resolves_inflight_.erase(mxid);
        else if (invite_resolve_gen_.load() != gen)
            return; // superseded by a later keystroke
        auto p = tesseract::UserProfile::from_json(profile_json);
        std::optional<views::InviteDialog::UserEntry> entry;
        if (p.exists)
        {
            if (p.user_id.empty())
                p.user_id = mxid;
            entry = views::InviteDialog::UserEntry{p.user_id, p.display_name,
                                                   p.avatar_url};
            // Roster insert only — merge_resolved_user_ would also re-emit
            // the quick switcher's results.
            known_users_[p.user_id] =
                tesseract::RoomMember{p.user_id, p.display_name, p.avatar_url};
        }
        for_each_invite_dialog_([&](views::InviteDialog& d)
                                { d.set_resolved_user(mxid, entry); });
        request_repaint_();
        return;
    }

    // Gendered-narration case: resolve the pronoun entry matching the app's
    // current locale, cache its possessive pronoun word, and push it into
    // every currently-open MessageListView (main window + any pop-outs)
    // showing a row that referenced this user_id — mirrors
    // notify_secondary_media_ready_'s "not room-scoped, notify all
    // secondary windows" fan-out, since a user_id isn't room-scoped either.
    auto git = pending_member_gender_requests_.find(request_id);
    if (git != pending_member_gender_requests_.end())
    {
        const std::string user_id = std::move(git->second);
        pending_member_gender_requests_.erase(git);
        member_gender_inflight_.erase(user_id);
        auto p = tesseract::UserProfile::from_json(profile_json);
        const auto* entry =
            views::select_pronoun_entry_for_locale(p.pronouns, tk::current_locale());
        const std::string pronoun = views::possessive_pronoun_for_gender(
            entry ? entry->grammatical_gender : std::string());
        member_gender_cache_[user_id] = pronoun;

        if (room_view_)
            if (auto* ml = room_view_->message_list())
                ml->update_member_pronoun(user_id, pronoun);
        for (const auto& [rid, w] : secondary_windows_)
        {
            if (auto* rv = w->room_view())
                if (auto* ml = rv->message_list())
                    ml->update_member_pronoun(user_id, pronoun);
        }
        return;
    }

    // Own-profile case (no map entry).
    auto p = tesseract::UserProfile::from_json(profile_json);
    // Only update if the fetch returned a valid result; otherwise keep stale.
    if (p.exists || own_extended_profile_.pronouns.empty())
    {
        own_extended_profile_ = {p.pronouns,    p.tz,          p.biography,
                                 p.status_emoji, p.status_text, p.call_joined_ts};
        notify_own_extended_profile_changed_();
    }
}

void ShellBase::handle_user_query_(const std::string& query)
{
    // Strip the leading '@' for substring matching; keep `query` for the
    // complete-mxid test below (the mxid includes its '@').
    last_user_query_ = query.size() > 1 ? query.substr(1) : std::string{};

    // Every query change invalidates any in-flight profile-resolve.
    const std::uint64_t gen = user_resolve_gen_.fetch_add(1) + 1;

    if (!known_users_built_ && !known_users_building_)
    {
        build_known_users_roster_();
    }

    // Local matches now (may be empty until the roster build lands).
    emit_user_results_();

    // Live-resolve a fully-typed mxid we don't already know, debounced so fast
    // typing coalesces into a single lookup.
    if (views::is_complete_mxid(query) &&
        known_users_.find(query) == known_users_.end())
    {
        if (!client_) return;
        auto req_id = next_request_id_++;
        pending_resolve_requests_[req_id] = {query, gen};
        // Capture client_ on the UI thread; don't read it from the worker.
        auto* c = client_;
        run_async_(
            [this, c, req_id, mxid = query, gen]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (user_resolve_gen_.load() != gen)
                {
                    // Superseded — remove the stale map entry on the UI thread.
                    post_to_ui_alive_([this, req_id]() {
                        pending_resolve_requests_.erase(req_id);
                    });
                    return;
                }
                // Still valid — fire the async FFI call; result arrives via
                // handle_extended_profile_ready_ui_ → pending_resolve_requests_.
                c->resolve_user_profile_async(req_id, mxid);
            });
    }
}

void ShellBase::build_known_users_roster_()
{
    if (known_users_building_ || !client_)
        return;
    known_users_building_ = true;

    // Fresh cancellation token; supersede any prior in-flight build.
    if (roster_build_cancel_)
        roster_build_cancel_->store(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    roster_build_cancel_ = cancel;

    auto sess = active_account_;
    const std::string me = my_user_id_;
    const std::size_t cap = kRosterMaxRoomMembers;

    // Seed DM partners now (instant, from rooms_) and snapshot room ids; member
    // enumeration happens on the worker. Emitting the seed immediately gives the
    // user feedback before any network/SDK work runs. Scan rooms most-recent-
    // first so the people the user talks to surface in the earliest batches.
    std::vector<const tesseract::RoomInfo*> ordered;
    ordered.reserve(rooms_.size());
    for (const auto& r : rooms_)
        ordered.push_back(&r);
    std::sort(ordered.begin(), ordered.end(),
              [](const tesseract::RoomInfo* a, const tesseract::RoomInfo* b)
              { return a->last_activity_ts > b->last_activity_ts; });

    std::vector<std::string> room_ids;
    room_ids.reserve(ordered.size());
    for (const auto* r : ordered)
    {
        room_ids.push_back(r->id);
        if (r->is_direct && !r->dm_counterpart_user_id.empty() &&
            r->dm_counterpart_user_id != me)
        {
            merge_roster_entry_(r->dm_counterpart_user_id, std::string{},
                                r->dm_avatar_url);
        }
    }
    emit_user_results_();

    run_async_(
        [this, sess, me, cap, cancel, room_ids = std::move(room_ids)]() mutable
        {
            // Per-batch accumulator handed to the UI thread; merged into
            // known_users_ there so partial results appear as the sweep runs.
            std::unordered_map<std::string, tesseract::RoomMember> batch;

            auto flush = [&](bool final)
            {
                if (batch.empty() && !final)
                    return;
                post_to_ui_alive_(
                    [this, sess, cancel, final,
                     batch = std::move(batch)]() mutable
                    {
                        // Drop if superseded (new build / invalidate / teardown)
                        // or the account switched mid-build.
                        if (cancel->load() || active_account_ != sess)
                        {
                            if (final && roster_build_cancel_ == cancel)
                                known_users_building_ = false;
                            return;
                        }
                        for (auto& [id, m] : batch)
                            merge_roster_entry_(id, std::move(m.display_name),
                                                m.avatar_url);
                        if (final)
                        {
                            known_users_built_    = true;
                            known_users_building_ = false;
                        }
                        emit_user_results_();
                    });
                batch.clear();
            };

            std::size_t scanned = 0;
            if (sess && sess->client)
            {
                for (const auto& rid : room_ids)
                {
                    if (cancel->load())
                        return; // bail early — keeps shutdown/switch responsive
                    auto members = sess->client->get_room_members(rid);
                    if (members.size() > cap)
                        continue; // skip very large rooms to keep this cheap
                    for (auto& m : members)
                    {
                        if (m.user_id.empty() || m.user_id == me)
                            continue;
                        auto& slot = batch[m.user_id];
                        if (slot.user_id.empty())
                            slot.user_id = m.user_id;
                        if (slot.display_name.empty() && !m.display_name.empty())
                            slot.display_name = std::move(m.display_name);
                        if (slot.avatar_url.empty() && !m.avatar_url.empty())
                            slot.avatar_url = std::move(m.avatar_url);
                    }
                    if (++scanned % kRosterEmitBatchRooms == 0)
                        flush(false);
                }
            }
            flush(true);
        });
}

std::vector<views::QuickSwitcher::UserEntry>
ShellBase::filter_known_users_(const std::string& needle) const
{
    std::vector<views::QuickSwitcher::UserEntry> out;
    out.reserve(known_users_.size());
    for (const auto& [id, m] : known_users_)
    {
        if (tk::ci_contains(m.display_name, needle) ||
            tk::ci_contains(m.user_id, needle))
        {
            out.push_back({m.user_id, m.display_name, m.avatar_url});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const views::QuickSwitcher::UserEntry& a,
                 const views::QuickSwitcher::UserEntry& b)
              {
                  const std::string& an =
                      a.display_name.empty() ? a.user_id : a.display_name;
                  const std::string& bn =
                      b.display_name.empty() ? b.user_id : b.display_name;
                  return std::lexicographical_compare(
                      an.begin(), an.end(), bn.begin(), bn.end(),
                      [](char x, char y)
                      {
                          return std::tolower(static_cast<unsigned char>(x)) <
                                 std::tolower(static_cast<unsigned char>(y));
                      });
              });
    constexpr std::size_t kMaxUserRows = 100;
    if (out.size() > kMaxUserRows)
        out.resize(kMaxUserRows);
    return out;
}

void ShellBase::ensure_known_users_roster_()
{
    if (!known_users_built_ && !known_users_building_)
        build_known_users_roster_();
}

void ShellBase::resolve_invite_user_(const std::string& user_id, bool debounce,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (!acting_client || !views::is_complete_mxid(user_id))
        return;
    // Already known (e.g. the roster landed since the dialog asked).
    if (auto it = known_users_.find(user_id); it != known_users_.end())
    {
        const auto& m = it->second;
        views::InviteDialog::UserEntry e{m.user_id, m.display_name, m.avatar_url};
        for_each_invite_dialog_([&](views::InviteDialog& d)
                                { d.set_resolved_user(user_id, e); });
        return;
    }
    if (!debounce)
    {
        if (!invite_resolves_inflight_.insert(user_id).second)
            return;
        auto req_id = next_request_id_++;
        pending_invite_resolves_[req_id] = {user_id, 0};
        acting_client->resolve_user_profile_async(req_id, user_id);
        return;
    }

    const std::uint64_t gen = invite_resolve_gen_.fetch_add(1) + 1;
    auto req_id = next_request_id_++;
    pending_invite_resolves_[req_id] = {user_id, gen};
    // Capture acting_client on the UI thread; don't read it from the worker.
    auto* c = acting_client;
    run_async_(
        [this, c, req_id, mxid = user_id, gen]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (invite_resolve_gen_.load() != gen)
            {
                post_to_ui_alive_([this, req_id]()
                                  { pending_invite_resolves_.erase(req_id); });
                return;
            }
            c->resolve_user_profile_async(req_id, mxid);
        });
}

void ShellBase::for_each_invite_dialog_(
    const std::function<void(views::InviteDialog&)>& fn)
{
    if (room_view_)
        if (auto* d = room_view_->invite_dialog(); d && d->is_open())
            fn(*d);
    for (const auto& [rid, w] : secondary_windows_)
    {
        if (auto* rv = w->room_view())
            if (auto* d = rv->invite_dialog(); d && d->is_open())
                fn(*d);
    }
}

void ShellBase::emit_user_results_()
{
    // Open invite dialogs filter the same roster.
    for_each_invite_dialog_([](views::InviteDialog& d) { d.refresh_candidates(); });
    if (!main_app_)
        return;
    if (auto* qs = main_app_->quick_switcher())
    {
        qs->set_user_results(filter_known_users_(last_user_query_));
        request_repaint_();
    }
}

void ShellBase::merge_roster_entry_(const std::string& id,
                                    std::string display_name,
                                    const std::string& avatar_url)
{
    if (id.empty())
        return;
    auto& slot = known_users_[id];
    if (slot.user_id.empty())
        slot.user_id = id;
    if (slot.display_name.empty() && !display_name.empty())
        slot.display_name = std::move(display_name);
    if (slot.avatar_url.empty() && !avatar_url.empty())
        slot.avatar_url = avatar_url;
}

void ShellBase::merge_resolved_user_(const tesseract::UserProfile& p)
{
    // A live-resolved profile is authoritative — overwrite any stale entry.
    known_users_[p.user_id] =
        tesseract::RoomMember{p.user_id, p.display_name, p.avatar_url};
    emit_user_results_();
}

void ShellBase::invalidate_known_users_()
{
    // Cancel any in-flight roster build so it stops scanning and won't merge
    // into the cleared map.
    if (roster_build_cancel_)
    {
        roster_build_cancel_->store(true);
        roster_build_cancel_.reset();
    }
    known_users_.clear();
    known_users_built_    = false;
    known_users_building_ = false;
    // Drop any in-flight resolve targeting the old roster/account.
    user_resolve_gen_.fetch_add(1);
    pending_resolve_requests_.clear();
    pending_user_profiles_.clear();
}

uint64_t ShellBase::compute_dock_notification_count_() const
{
    uint64_t total = 0;
    for (const auto& [uid, rooms] : per_account_rooms_)
        for (const auto& r : rooms)
            total += r.notification_count;
    return total;
}

void ShellBase::notify_tray_unread_()
{
    const uint64_t badge = compute_dock_notification_count_();
    if (badge != last_dock_badge_count_)
    {
        last_dock_badge_count_ = badge;
        on_dock_badge_changed_(badge);
    }

    // Computed unconditionally (not gated behind the tray early-return below)
    // because it excludes the active account while the tray aggregate
    // includes it: the tray aggregate can stay unchanged (e.g. the active
    // account already has an unread room) while a *different* account's
    // unread state flips, which is exactly the case this hook exists to
    // catch.
    const bool other_unread = other_accounts_have_unread();
    if (other_unread != last_other_accounts_unread_)
    {
        last_other_accounts_unread_ = other_unread;
        on_account_badges_changed_(other_unread);
    }

    auto [u, h] = compute_tray_unread(per_account_rooms_);
    if (u == last_tray_unread_ && h == last_tray_highlight_)
    {
        return;
    }
    last_tray_unread_    = u;
    last_tray_highlight_ = h;
    on_tray_unread_changed_(u, h);
}

const RoomInfo* ShellBase::best_unread_room_() const
{
    const RoomInfo* best = nullptr;
    for (const auto& r : rooms_)
    {
        if (r.notification_count == 0 && r.highlight_count == 0)
            continue;
        if (!best
            || (r.highlight_count > 0 && best->highlight_count == 0)
            || (r.highlight_count == best->highlight_count
                && r.last_activity_ts > best->last_activity_ts))
            best = &r;
    }
    return best;
}

void ShellBase::open_matrix_link(const std::string& uri)
{
    if (!client_ || rooms_.empty())
    {
        // Not yet logged in or rooms haven't arrived yet — replay on first update.
        pending_matrix_link_ = uri;
        return;
    }
    pending_matrix_link_.clear();

    auto link = Client::parse_matrix_link(uri);
    using Kind = Client::MatrixLink::Kind;

    // Remember the permalink's `?via=` routing hints so a later join / knock /
    // preview of this room reaches a homeserver that doesn't already know it.
    if (!link.via.empty() &&
        (link.kind == Kind::Room || link.kind == Kind::RoomAlias ||
         link.kind == Kind::Event))
        pending_join_via_[link.primary] = link.via;

    switch (link.kind)
    {
    case Kind::User:
        if (room_view_)
            room_view_->open_user_profile(link.primary);
        break;

    case Kind::Room:
    {
        auto it = std::find_if(rooms_.begin(), rooms_.end(),
                               [&](const RoomInfo& r) { return r.id == link.primary; });
        if (it != rooms_.end())
            tab_navigate_room(link.primary);
        else
            // Possibly an upgraded room we're still in but no longer list.
            open_room_version_(link.primary, link.via, {}, /*join_if_missing=*/false);
        break;
    }

    case Kind::RoomAlias:
    {
        auto it = std::find_if(rooms_.begin(), rooms_.end(),
                               [&](const RoomInfo& r)
                               { return r.canonical_alias == link.primary; });
        if (it != rooms_.end())
            tab_navigate_room(it->id);
        else if (main_app_)
        {
            main_app_->add_room_view()->open_join_with_prefill(link.primary);
            request_relayout_();
        }
        break;
    }

    case Kind::Event:
    {
        auto it = std::find_if(rooms_.begin(), rooms_.end(),
                               [&](const RoomInfo& r) { return r.id == link.primary; });
        if (it != rooms_.end())
        {
            // Navigate (sets current_room_id_ synchronously), then jump to the
            // event via the deferred-scroll + focused-subscription path so it
            // works even when the target isn't in the loaded window.
            tab_navigate_room(link.primary);
            if (!link.event_id.empty())
            {
                if (room_view_ && room_view_->message_list())
                    room_view_->message_list()->set_highlighted_event(
                        link.event_id);
                try_scroll_to_room_event_(link.event_id);
            }
        }
        else
        {
            // Not listed: either an upgraded room we're still in (opened in
            // place), or one we haven't joined (the Join dialog, with the target
            // event remembered so the post-join hook jumps to it).
            open_room_version_(link.primary, link.via, link.event_id,
                               /*join_if_missing=*/false);
        }
        break;
    }

    default:
        // Not a matrix link — ignore.
        break;
    }
}

void ShellBase::navigate_tray_unread_()
{
    if (const RoomInfo* best = best_unread_room_())
        tab_navigate_room(best->id);
}

bool ShellBase::focus_tray_unread_popout_()
{
    const RoomInfo* best = best_unread_room_();
    return best && focus_secondary_window_(best->id);
}

void ShellBase::broadcast_rebuild_tray_()
{
    for (ShellBase* win : account_manager_.all_windows())
        win->rebuild_tray_();
}

std::vector<std::pair<std::string, std::function<void()>>>
ShellBase::build_tray_items_()
{
    std::vector<std::pair<std::string, std::function<void()>>> items;
    for (const auto& acc : account_manager_.accounts())
    {
        std::string label = acc->display_name.empty()
                                 ? acc->user_id
                                 : acc->display_name + " (" + acc->user_id + ")";
        const std::string uid = acc->user_id;
        // Mirrors on_account_picker_select_(): raise the account's dedicated
        // popout window if it has one, otherwise switch it into this window
        // (the tray owner — the only window whose menu this really is).
        items.emplace_back(
            std::move(label),
            [this, uid]
            {
                if (auto* win = account_manager_.dedicated_window(uid))
                {
                    win->raise_and_activate_();
                    return;
                }
                switch_active_account_(uid);
            });
    }
    return items;
}

// Build the canonical user-strip context-menu item list. The order and
// Log Out label are defined here; platform shells supply the action
// callbacks and iterate the result to build their native menu. The QR
// item is omitted automatically when server_info_.supports_qr_grant is
// false. show_qr_grant may be a null std::function even when QR is
// supported — the item will still be omitted. Likewise, verify_session
// is omitted when null — pass it only when the active session is
// currently unverified (mirrors UserInfo's warning-dot condition), so
// the item lets the user restart verification mid-session and
// disappears once verified.
std::vector<ShellBase::UserMenuItem> ShellBase::build_user_menu_items_(
    std::function<void()> open_settings,
    std::function<void()> add_account,
    std::function<void()> show_qr_grant,
    std::function<void()> logout,
    std::function<void()> quit,
    std::function<void()> verify_session) const
{
    std::vector<UserMenuItem> items;
    items.push_back({tk::tr("Settings\xe2\x80\xa6"),    std::move(open_settings)});
    // Opened in the shared tree, so no shell needs its own callback for it.
    if (main_app_)
        items.push_back({tk::tr("Keyboard Shortcuts\xe2\x80\xa6"),
                         [app = main_app_] { app->show_keyboard_shortcuts(); }});
    if (verify_session)
        items.push_back({tk::tr("Verify this session\xe2\x80\xa6"), std::move(verify_session)});
    items.push_back({tk::tr("Add Account\xe2\x80\xa6"), std::move(add_account)});
    if (server_info_.supports_qr_grant && show_qr_grant)
        items.push_back({tk::tr("Add device via QR\xe2\x80\xa6"), std::move(show_qr_grant)});
    const std::string& name =
        my_display_name_.empty() ? my_user_id_ : my_display_name_;
    items.push_back({tk::trf(tk::tr("Log Out {0}"), {name}), std::move(logout)});
    items.push_back({"", nullptr}); // separator
    items.push_back({tk::tr("Quit"), std::move(quit)});
    return items;
}


void ShellBase::handle_account_prefs_updated_ui_(std::string user_id,
                                                 std::string json)
{
    // Only the active account's prefs set the pending restore rooms.
    if (!active_account_ || active_account_->user_id != user_id)
    {
        return;
    }
    auto prefs = tesseract::Prefs::parse(json);
    active_account_->prefs_json = json;

    // Another device (or our own save's echo) may have changed the default
    // emoji skin tone; pickers and autocomplete read it on their next use.
    tesseract::apply_synced_emoji_skin_tone(
        *active_account_, tesseract::emoji::skin_tone_from_key(prefs.emoji_skin_tone),
        std::chrono::steady_clock::now());

    // Bridge overrides can change from another device (or echo our own
    // save); keep this account's copy and every room referencing it in sync,
    // the same way a local toggle would via set_bridge_override_.
    if (active_account_->bridge_not_bridged_overrides != prefs.bridge_not_bridged_overrides)
    {
        active_account_->bridge_not_bridged_overrides = prefs.bridge_not_bridged_overrides;
        apply_bridge_overrides_(rooms_, active_account_->bridge_not_bridged_overrides);
        if (auto it = per_account_rooms_.find(my_user_id_); it != per_account_rooms_.end())
            apply_bridge_overrides_(it->second, active_account_->bridge_not_bridged_overrides);
        if (!current_room_id_.empty())
            refresh_bridge_dependent_ui_(current_room_id_);
    }

    // Adopt the synced MRU only while ours is still empty (fresh login, before
    // the first visit): once the user has visited a room the local order wins,
    // so another device's save can't reorder the list mid Ctrl+Tab cycle.
    if (recent_room_ids_.empty() && !prefs.recent_rooms.empty())
    {
        recent_room_ids_ = prefs.recent_rooms;
        if (recent_room_ids_.size() > kRecentRoomsMax)
            recent_room_ids_.resize(kRecentRoomsMax);
        active_account_->recent_rooms = recent_room_ids_;
    }

    if (!prefs.open_rooms.empty() && pending_restore_rooms_.empty() &&
        current_room_id_.empty())
    {
        restore_gate_ticks_ = 0;
        pending_restore_rooms_ = prefs.open_rooms;
        // Ensure last_room (active tab) is at [0].
        if (!prefs.last_room.empty() && pending_restore_rooms_[0] != prefs.last_room)
        {
            auto it = std::find(pending_restore_rooms_.begin(),
                                pending_restore_rooms_.end(), prefs.last_room);
            if (it != pending_restore_rooms_.end())
                std::rotate(pending_restore_rooms_.begin(), it, it + 1);
        }
        // Populate popouts-to-restore from local settings (device-local, not
        // synced). Only do this on the first account-prefs arrival so that a
        // manual pop-out/close during a session doesn't re-queue stale entries.
        populate_pending_restore_popouts_();
    }
}

void ShellBase::handle_own_profile_changed_ui_(
    std::string user_id, std::optional<std::string> display_name,
    std::optional<std::string> avatar_url)
{
    if (!active_account_ || active_account_->user_id != user_id)
    {
        return;
    }

    bool identity_changed = false;
    if (display_name && *display_name != my_display_name_)
    {
        my_display_name_ = std::move(*display_name);
        active_account_->display_name = my_display_name_;
        identity_changed = true;
    }
    if (avatar_url && *avatar_url != my_avatar_url_)
    {
        my_avatar_url_ = std::move(*avatar_url);
        active_account_->avatar_url = my_avatar_url_;
        identity_changed = true;
    }
    if (identity_changed)
    {
        refresh_user_strip_();
    }

    // Timezone, status, pronouns and bio come from the MSC4133 fetch; its
    // own-profile branch in handle_extended_profile_ready_ui_ applies them.
    if (server_info_.supports_profile_fields &&
        server_info_.profile_fields_enabled)
    {
        fetch_own_extended_profile_async_();
    }
}

void ShellBase::handle_identity_status_changed_ui_(
    std::string room_id, std::vector<tesseract::IdentityWarning> warnings)
{
    if (main_window_shows_(room_id) && main_room_pane_)
    {
        main_room_pane_->on_identity_status_changed(room_id, warnings);
    }
    dispatch_to_secondary_windows_(room_id,
                                   [&](RoomWindowBase* w)
                                   {
                                       w->on_identity_status_changed(room_id,
                                                                     warnings);
                                   });
}

void ShellBase::handle_user_identities_changed_ui_(std::vector<std::string> user_ids)
{
    // Every pane of this account (main window and pop-outs) re-reads its
    // open profile's trust row if it shows one of these users.
    if (event_is_for_active_account_() && main_room_pane_)
        main_room_pane_->on_user_identities_changed(user_ids);
    for (const auto& w : owned_secondary_windows_)
        if (w && w->pane() && popout_accepts_event_(w.get()))
            w->pane()->on_user_identities_changed(user_ids);
}

void ShellBase::handle_presence_changed_ui_(const std::string& user_id,
                                            PresenceState state)
{
    // poll_presence_once (sdk/src/client/sync.rs) reports every successful
    // presence poll unconditionally, not just changes — with N DM
    // counterparts polled every 60s, most ticks report a state identical to
    // the last one. Rebuilding the room list is not cheap (a full
    // space-child-count pass over every room), so skip it when nothing
    // actually changed.
    auto [it, inserted] = user_presence_.try_emplace(user_id, state);
    if (!inserted)
    {
        if (it->second == state)
            return;
        it->second = state;
    }
    // The room list may show a DM dot, and the RoomInfoPanel (main or
    // pop-out) may show a member dot — both read presence_provider_ on every
    // paint, so an actual state change still needs a repaint.
    on_rooms_updated_();
    update_secondary_room_infos_();
}

PresenceState ShellBase::presence_for_(const std::string& user_id) const
{
    auto it = user_presence_.find(user_id);
    return it != user_presence_.end() ? it->second : PresenceState::Offline;
}

// ── Presence (send-side) ──────────────────────────────────────────────────────

void ShellBase::notify_user_activity_()
{
    // Feed the image GC's idle gate — while the user is interacting, the GC
    // runs and reclaims off-screen decodes; once they stop, it freezes so
    // on-screen images are never evicted from under them.
    account_manager_.note_image_gc_activity();

    if (!presence_tracker_)
    {
        // Lazily start tracking on the first activity we see *after* sync is
        // up and running. This avoids publishing Online before the homeserver
        // has acknowledged our access token via the sliding-sync handshake.
        if (last_room_list_state_ != RoomListState::Running)
        {
            return;
        }
        start_presence_tracking_();
        // Stays null when presence sending is disabled.
        if (!presence_tracker_)
        {
            return;
        }
    }
    presence_tracker_->notify_input();
}

void ShellBase::notify_window_active_(bool active)
{
    // Alt-tabbing/switching to another program shouldn't leave a popup menu
    // (or any other registered popup — ComboBox dropdowns, the date picker,
    // etc.) floating on top of a now-background window.
    if (!active && main_app_ && main_app_->host())
        main_app_->host()->dismiss_active_popup();

    if (presence_tracker_)
    {
        presence_tracker_->notify_window_active(active);
    }
    last_window_active_ = active;
    // The DM-presence polling loop in the Rust SDK only produces data that's
    // visible to the user while the window is on-screen, so suspend it while
    // the window is hidden/minimized/unfocused (and while low power mode is
    // on). resolve_presence_polling_() folds those together with the
    // `send_presence` Privacy setting and issues the FFI call.
    resolve_presence_polling_();
}

void ShellBase::notify_presence_tick_()
{
    auto activity_scope = activity_.begin("housekeeping-tick", "UI housekeeping", "periodic");
    // Backstop GC tick (media_sweep_timer_ is the primary ~2 s driver).
    run_image_gc_();
    // Same 30 s cadence reclaims rooms/threads that haven't been on-screen in
    // a while — see the "Idle-TTL timeline eviction" block in ShellBase.h.
    sweep_idle_timelines_();

    if (presence_tracker_)
    {
        presence_tracker_->notify_tick();
    }

    // Commit a settled low-power debounce (armed by refresh_low_power_signals_).
    power_policy_.notify_tick();
}

void ShellBase::notify_presence_logout_()
{
    if (!presence_tracker_)
    {
        return;
    }
    // Tear down the tracker first so its on_state_change can't fire and
    // spawn a worker that races with the shell's imminent
    // accounts_.erase() / client destruction. Then PUT Offline
    // *synchronously* on the UI thread: this is a user-initiated logout
    // (already a high-latency action), the brief freeze is acceptable, and
    // doing it inline guarantees we never hold a raw Client* across the
    // destruction boundary.
    presence_tracker_->on_state_change = nullptr;
    presence_tracker_.reset();
    if (client_)
    {
        (void) client_->set_presence(PresenceState::Offline);
    }
}

void ShellBase::start_presence_tracking_()
{
    if (presence_tracker_)
    {
        return;
    }
    if (!tesseract::Settings::instance().send_presence)
    {
        return;
    }
    presence_tracker_ = std::make_unique<PresenceTracker>();
    presence_tracker_->on_state_change =
        [this](PresenceTracker::State s)
    {
        // Online ↔ Unavailable transitions: send the PUT through run_async_
        // so app shutdown drains it (WorkerPool destructor joins threads
        // before ~ShellBase completes, protecting Client lifetime).
        // Capture a strong ref to the account active at dispatch time so the
        // PUT targets that account's Client (and keeps it alive) even if the
        // user logs out / switches before the worker runs.
        const auto target = shell_helpers::to_client_presence(s);
        if (client_)
            client_->set_presence_async(target);
    };
    presence_tracker_->notify_sync_started();
}
} // namespace tesseract
