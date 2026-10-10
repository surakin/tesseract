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

void ShellBase::register_search_backend_()
{
    if (search_backend_handle_)
        return;

    SearchBackend::ShellRegistration reg;
    reg.rooms = [this] { return rooms_; };
    reg.known_users = [this]() -> std::vector<tesseract::RoomMember>
    {
        // Best-effort: the roster is built lazily/asynchronously (see
        // build_known_users_roster_), so an early query may see it still
        // empty. Kick off a build so later queries see real results, without
        // blocking this (synchronous D-Bus) call on it.
        if (!known_users_built_ && !known_users_building_)
            build_known_users_roster_();
        std::vector<tesseract::RoomMember> v;
        v.reserve(known_users_.size());
        for (const auto& [id, m] : known_users_)
            v.push_back(m);
        return v;
    };
    reg.activate_room = [this](const std::string& room_id)
    {
        post_to_ui_alive_(
            [this, room_id]
            {
                raise_and_activate_();
                tab_select_room(room_id);
            });
    };
    reg.activate_contact = [this](const std::string& mxid)
    {
        post_to_ui_alive_(
            [this, mxid]
            {
                raise_and_activate_();
                handle_open_dm_(mxid);
            });
    };
    search_backend_handle_ =
        account_manager_.search_backend().register_shell(std::move(reg));
}

void ShellBase::handle_search_query_(const std::string& query)
{
    // Empty query → nothing to search; the overlay shows its prompt. Drop any
    // pending debounced search so a just-cleared field never fires late.
    if (query.empty() || !client_)
    {
        cancel_debounce_(DebounceSlot::MessageSearch);
        return;
    }
    // Debounce so a burst of keystrokes issues one FTS query, not one per key.
    // Correctness is still guarded by the request_id stale-drop below.
    debounce_(DebounceSlot::MessageSearch, 120, [this, query]() {
        if (query.empty() || !client_)
            return;
        const std::uint64_t id = ++search_request_id_;
        search_pending_queries_[id] = query;
        // Global search (empty room filter). Non-blocking; results arrive via
        // on_search_results → handle_search_results_ui_.
        client_->search_messages(id, query, std::string(), std::string(), 200);
    });
}

void ShellBase::handle_search_results_ui_(
    std::uint64_t request_id, std::vector<tesseract::SearchHit> results)
{
    auto it = search_pending_queries_.find(request_id);
    if (it == search_pending_queries_.end())
        return; // unknown / superseded request
    const std::string for_query = std::move(it->second);
    search_pending_queries_.erase(it);
    // Drop responses older than the latest issued request.
    if (request_id != search_request_id_)
        return;
    // Resolve room display names from the already-cached room list (the SDK
    // deliberately leaves SearchHit::room_name empty to avoid a per-result
    // member-store walk on every keystroke). Falls back to the room id.
    for (auto& hit : results)
    {
        if (const RoomInfo* ri = room_by_id_(hit.room_id))
            hit.room_name = ri->name.empty() ? hit.room_id : ri->name;
        else
            hit.room_name = hit.room_id;
    }
    if (main_app_ && main_app_->message_search())
    {
        main_app_->message_search()->set_results(std::move(results), for_query);
        schedule_relayout_();
    }
}

void ShellBase::handle_forward_done_ui_(std::uint64_t request_id)
{
    if (pending_forwards_.count(request_id))
    {
        pending_forwards_.erase(request_id);
        if (pending_forwards_.empty())
            if (auto* fp = main_app_ ? main_app_->forward_picker() : nullptr)
                fp->close();
        return;
    }
    // Not the main window's request — request_id is a process-global
    // counter, so at most one open pop-out's own pending_forwards_ can
    // recognize it (see RoomWindowBase::handle_forward_done_).
    for (auto& [rid, w] : secondary_windows_)
    {
        if (w->handle_forward_done_(request_id))
            return;
    }
}

void ShellBase::handle_forward_failed_ui_(std::uint64_t      request_id,
                                          const std::string& message)
{
    auto it = pending_forwards_.find(request_id);
    if (it != pending_forwards_.end())
    {
        const auto* room = room_by_id_(it->second);
        std::string target_name =
            (room && !room->name.empty()) ? room->name : it->second;
        pending_forwards_.erase(it);
        auto* fp = main_app_ ? main_app_->forward_picker() : nullptr;
        if (fp)
        {
            fp->add_forward_error(target_name, message);
            if (pending_forwards_.empty())
                fp->mark_complete();
        }
        return;
    }
    for (auto& [rid, w] : secondary_windows_)
    {
        if (w->handle_forward_failed_(request_id, message))
            return;
    }
}

void ShellBase::handle_search_failed_ui_(std::uint64_t request_id,
                                         const std::string& /*message*/)
{
    // Treat a failure as an empty result set for the issuing query so the
    // overlay shows "No matches" rather than spinning.
    auto it = search_pending_queries_.find(request_id);
    if (it == search_pending_queries_.end())
        return;
    const std::string for_query = std::move(it->second);
    search_pending_queries_.erase(it);
    if (request_id != search_request_id_)
        return;
    if (main_app_ && main_app_->message_search())
    {
        main_app_->message_search()->set_results({}, for_query);
        schedule_relayout_();
    }
}

void ShellBase::handle_room_directory_search_results_ui_(
    std::uint64_t request_id, std::vector<tesseract::RoomDirectoryEntry> entries,
    bool reached_end)
{
    if (auto* ar = main_app_ ? main_app_->add_room_view() : nullptr)
    {
        if (auto* dv = ar->directory_view())
        {
            dv->set_results(request_id, std::move(entries), reached_end);
            schedule_relayout_();
        }
    }
}

void ShellBase::handle_room_directory_search_failed_ui_(
    std::uint64_t request_id, const std::string& message)
{
    if (auto* ar = main_app_ ? main_app_->add_room_view() : nullptr)
    {
        if (auto* dv = ar->directory_view())
        {
            dv->set_search_failed(request_id, message);
            schedule_relayout_();
        }
    }
}

// ── Per-room "find in conversation" search (Ctrl+F / Cmd+F) ──────────────────

views::RoomSearchBar* ShellBase::in_room_search_bar_() const
{
    auto* rv = in_room_search_active_rv_
                   ? in_room_search_active_rv_
                   : (main_app_ ? main_app_->room_view() : nullptr);
    return rv ? rv->room_search_bar() : nullptr;
}

void ShellBase::handle_in_room_search_query_(const std::string& query,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (query.empty() || !acting_client)
    {
        cancel_debounce_(DebounceSlot::InRoomSearch);
        in_room_search_matches_.clear();
        in_room_search_current_ = -1;
        in_room_search_apply_highlights_();
        if (auto* bar = in_room_search_bar_())
            bar->set_match_status(0, 0, false, false);
        return;
    }
    // For the main window, use the current room. For a popout, the room_id
    // was already set by the on_room_search_query callback before this call.
    if (!in_room_search_active_rv_)
        in_room_search_room_id_ = current_room_id_;
    // Capture context so the debounce lambda can detect a stale query (main
    // window room switch, or a new search from a different window).
    const std::string search_room_id = in_room_search_room_id_;
    auto* search_rv = in_room_search_active_rv_;
    debounce_(DebounceSlot::InRoomSearch, 120,
              [this, query, search_room_id, search_rv,
               weak_acting = std::weak_ptr<AccountSession>(acting)]()
    {
        auto still = weak_acting.lock();
        Client* const acting_client = still ? still->client.get() : nullptr;
        if (query.empty() || !acting_client)
            return;
        if (in_room_search_room_id_ != search_room_id ||
            in_room_search_active_rv_ != search_rv)
            return; // room switched or different window started searching
        const std::uint64_t id = ++in_room_search_request_id_;
        in_room_search_pending_[id] = query;
        acting_client->search_messages(id, query, in_room_search_room_id_, std::string(), 200);
        if (auto* bar = in_room_search_bar_())
            bar->set_match_status(0, 0, /*searching=*/true, false);
    });
}

void ShellBase::handle_in_room_search_results_ui_(
    std::uint64_t request_id, std::vector<tesseract::SearchHit> results)
{
    auto it = in_room_search_pending_.find(request_id);
    if (it == in_room_search_pending_.end())
        return;
    in_room_search_pending_.erase(it);
    if (request_id != in_room_search_request_id_)
        return;

    // Capture paginate-rerun state before any mutations so we can decide
    // whether to trigger another paginate at the end of this handler.
    const bool from_paginate_rerun = in_room_search_paginate_rerun_;
    const bool was_going_to_oldest = in_room_search_goto_oldest_;
    const int  prev_match_count    = in_room_search_prev_match_count_;
    in_room_search_paginate_rerun_   = false;
    in_room_search_prev_match_count_ = 0;

    // Sort ascending by timestamp (index 0 = oldest match).
    std::sort(results.begin(), results.end(),
              [](const tesseract::SearchHit& a, const tesseract::SearchHit& b)
              { return a.timestamp_ms < b.timestamp_ms; });

    // Save focused event id so we can restore focus after results update.
    std::string prev_focused;
    if (in_room_search_current_ >= 0 &&
        in_room_search_current_ <
            static_cast<int>(in_room_search_matches_.size()))
    {
        prev_focused =
            in_room_search_matches_[static_cast<std::size_t>(
                in_room_search_current_)].event_id;
    }

    in_room_search_matches_ = std::move(results);
    in_room_search_apply_highlights_();

    const int total = static_cast<int>(in_room_search_matches_.size());
    if (total == 0)
    {
        in_room_search_current_ = -1;
        in_room_search_goto_oldest_ = false;
        const bool reached_start =
            pagination_.count(in_room_search_room_id_) &&
            pagination_.at(in_room_search_room_id_).reached_start;
        if (auto* bar = in_room_search_bar_())
            bar->set_match_status(0, 0, false, reached_start);
        in_room_search_maybe_paginate_(false);
        return;
    }

    if (in_room_search_goto_oldest_)
    {
        in_room_search_goto_oldest_ = false;
        in_room_search_current_ = 0;
    }
    else
    {
        // Restore previously focused event, or default to newest.
        in_room_search_current_ = total - 1;
        if (!prev_focused.empty())
        {
            for (int i = 0; i < total; ++i)
            {
                if (in_room_search_matches_[static_cast<std::size_t>(i)].event_id ==
                    prev_focused)
                {
                    in_room_search_current_ = i;
                    break;
                }
            }
        }
    }
    in_room_search_focus_current_();

    // If this search was triggered by a paginate batch and brought no NEW
    // matches (the count stayed the same or decreased), keep paginating so
    // the loop doesn't stop after the first batch that doesn't match.
    if (from_paginate_rerun && total <= prev_match_count)
        in_room_search_maybe_paginate_(was_going_to_oldest);
}

void ShellBase::handle_in_room_search_failed_ui_(std::uint64_t request_id,
                                                  const std::string&)
{
    auto it = in_room_search_pending_.find(request_id);
    if (it == in_room_search_pending_.end())
        return;
    in_room_search_pending_.erase(it);
    if (request_id != in_room_search_request_id_)
        return;
    in_room_search_matches_.clear();
    in_room_search_current_ = -1;
    in_room_search_apply_highlights_();
    const bool reached_start =
        pagination_.count(in_room_search_room_id_) &&
        pagination_.at(in_room_search_room_id_).reached_start;
    if (auto* bar = in_room_search_bar_())
        bar->set_match_status(0, 0, false, reached_start);
}

void ShellBase::in_room_search_apply_highlights_()
{
    auto* active_rv = in_room_search_active_rv_ ? in_room_search_active_rv_
                                                 : room_view_;
    auto* ml = active_rv ? active_rv->message_list() : nullptr;
    if (!ml)
        return;
    if (in_room_search_matches_.empty())
    {
        ml->clear_search_matches();
        ml->set_highlighted_event({});
        return;
    }
    std::unordered_set<std::string> ids;
    ids.reserve(in_room_search_matches_.size());
    for (const auto& hit : in_room_search_matches_)
        ids.insert(hit.event_id);
    ml->set_search_matches(std::move(ids));
}

void ShellBase::in_room_search_focus_current_()
{
    if (in_room_search_matches_.empty() || in_room_search_current_ < 0)
        return;
    const int total = static_cast<int>(in_room_search_matches_.size());
    if (in_room_search_current_ >= total)
        in_room_search_current_ = total - 1;

    const auto& hit =
        in_room_search_matches_[static_cast<std::size_t>(in_room_search_current_)];
    auto* active_rv = in_room_search_active_rv_ ? in_room_search_active_rv_
                                                 : room_view_;
    if (active_rv && active_rv->message_list())
    {
        auto* ml = active_rv->message_list();
        ml->set_highlighted_event(hit.event_id);
        if (in_room_search_active_win_)
        {
            // Popout: scroll the popout's own message list and trigger its
            // layout. If the event isn't loaded, subscribe_room_at to fetch it.
            ml->set_pending_scroll_event_id(hit.event_id);
            bool found = false;
            for (const auto& m : ml->messages())
                if (m.event_id == hit.event_id) { found = true; break; }
            if (!found && client_ && main_room_pane_)
            {
                const std::string search_room = in_room_search_room_id_;
                const std::string eid         = hit.event_id;
                main_room_pane_->begin_focused_subscription_(search_room, eid);
                auto sess = active_account_;
                run_async_mut_([sess, search_room, eid]() {
                    if (!sess || !sess->client) return;
                    sess->client->subscribe_room_at(search_room, eid);
                });
            }
            in_room_search_active_win_->request_relayout();
        }
        else
        {
            try_scroll_to_room_event_(hit.event_id);
        }
    }

    const bool reached_start =
        pagination_.count(in_room_search_room_id_) &&
        pagination_.at(in_room_search_room_id_).reached_start;
    if (auto* bar = in_room_search_bar_())
        bar->set_match_status(in_room_search_current_ + 1, total, false,
                              reached_start);
}

void ShellBase::in_room_search_navigate_(int delta)
{
    const int total = static_cast<int>(in_room_search_matches_.size());
    if (total == 0)
        return;
    if (in_room_search_current_ < 0)
        in_room_search_current_ = total - 1;

    const int next = in_room_search_current_ + delta;
    if (next < 0)
    {
        // At the oldest match, going further UP. Try to paginate back when
        // enabled and history is not exhausted; otherwise wrap to the newest.
        const bool can_paginate =
            in_room_search_paginate_ &&
            !(pagination_.count(in_room_search_room_id_) &&
              pagination_.at(in_room_search_room_id_).reached_start);
        if (can_paginate)
        {
            in_room_search_maybe_paginate_(/*at_oldest_boundary=*/true);
            return;
        }
        in_room_search_current_ = total - 1; // wrap to newest
    }
    else if (next >= total)
    {
        in_room_search_current_ = 0; // wrap to oldest
    }
    else
    {
        in_room_search_current_ = next;
    }
    in_room_search_focus_current_();
}

void ShellBase::in_room_search_maybe_paginate_(bool at_oldest_boundary)
{
    if (!in_room_search_paginate_)
    {
        // Clamp at oldest loaded match.
        if (at_oldest_boundary && !in_room_search_matches_.empty())
        {
            in_room_search_current_ = 0;
            in_room_search_focus_current_();
        }
        return;
    }

    const bool reached_start =
        pagination_.count(in_room_search_room_id_) &&
        pagination_.at(in_room_search_room_id_).reached_start;
    if (reached_start)
    {
        if (at_oldest_boundary && !in_room_search_matches_.empty())
            in_room_search_current_ = 0;
        const int total = static_cast<int>(in_room_search_matches_.size());
        if (auto* bar = in_room_search_bar_())
            bar->set_match_status(
                in_room_search_current_ + 1, total, false, /*at_start=*/true);
        // Pagination exhausted — clear the "Fetching…" override if one is active.
        if (status_override_active_)
        {
            status_override_active_ = false;
            on_restore_status_ui_();
        }
        return;
    }

    auto& state = pagination_[in_room_search_room_id_];
    if (state.in_flight)
        return; // paginate already in progress

    state.in_flight = true;
    in_room_search_rerun_on_paginate_ = true;
    in_room_search_goto_oldest_ = at_oldest_boundary;

    if (room_view_)
        room_view_->set_paginating(true);
    start_anim_tick_();

    // Status bar feedback while fetching.  Show how far back we've reached
    // using the oldest *loaded event* (front of the message list) — this
    // advances each batch even when no new matches have appeared yet.
    {
        std::string status = tk::tr("Fetching older messages\xe2\x80\xa6");
        std::uint64_t ts_ms = 0;
        {
            auto* ml = room_view_ ? room_view_->message_list() : nullptr;
            if (ml && !ml->messages().empty())
                ts_ms = ml->messages().front().timestamp_ms;
            else if (!in_room_search_matches_.empty())
                ts_ms = in_room_search_matches_[0].timestamp_ms;
        }
        if (ts_ms > 0)
        {
            const std::time_t t   = static_cast<std::time_t>(ts_ms / 1000);
            const std::time_t now = std::time(nullptr);
            std::tm tm_val{}, now_tm{};
#if defined(_WIN32)
            localtime_s(&tm_val, &t);
            localtime_s(&now_tm, &now);
#else
            localtime_r(&t, &tm_val);
            localtime_r(&now, &now_tm);
#endif
            // TRANSLATORS: date patterns, see tk::format_date.
            const std::string date =
                tm_val.tm_year == now_tm.tm_year
                    ? tk::format_date(tm_val, tk::tr("%b %-d"))
                    : tk::format_date(tm_val, tk::tr("%b %-d, %Y"));
            status += " " + tk::trf(tk::tr("(oldest: {0})"), {date});
        }
        // Set synchronously on the UI thread so the text is visible before
        // paginate_back_async() fires its completion callback via PostMessage.
        ++status_msg_gen_;
        status_override_active_          = true;
        status_message_allows_links_     = false;
        on_show_status_message_ui_(status);
    }

    const std::uint64_t req_id = next_paginate_id_++;
    pending_paginates_[req_id] = {in_room_search_room_id_, /*is_backward=*/true};
    if (client_)
        client_->paginate_back_async(req_id, in_room_search_room_id_, 50);
}

void ShellBase::set_in_room_search_paginate_(bool enabled)
{
    in_room_search_paginate_ = enabled;
    if (!enabled)
    {
        ++status_msg_gen_; // cancel any queued "Fetching…" display post
        status_override_active_ = false;
        on_restore_status_ui_();
        return;
    }
    auto* bar = in_room_search_bar_();
    if (!bar || bar->query().empty())
        return;
    // Trigger paginate immediately: no matches yet, or focused at oldest.
    if (in_room_search_matches_.empty())
        in_room_search_maybe_paginate_(false);
    else if (in_room_search_current_ == 0)
        in_room_search_maybe_paginate_(true);
}

void ShellBase::in_room_search_clear_()
{
    cancel_debounce_(DebounceSlot::InRoomSearch);
    in_room_search_pending_.clear();
    in_room_search_matches_.clear();
    in_room_search_current_           = -1;
    in_room_search_room_id_.clear();
    in_room_search_active_rv_         = nullptr;
    in_room_search_active_win_        = nullptr;
    in_room_search_rerun_on_paginate_ = false;
    in_room_search_goto_oldest_       = false;
    in_room_search_paginate_rerun_    = false;
    in_room_search_prev_match_count_  = 0;
}

// ── Find-in-thread search (ThreadView's own search bar) ──────────────────────

views::RoomSearchBar* ShellBase::thread_search_bar_() const
{
    auto* tv = room_view_ ? room_view_->thread_view() : nullptr;
    return tv ? tv->search_bar() : nullptr;
}

void ShellBase::handle_thread_search_query_(const std::string& query)
{
    const std::string current_thread_root =
        main_room_pane_ ? main_room_pane_->thread_root() : std::string();
    if (query.empty() || !client_ || current_thread_root.empty())
    {
        cancel_debounce_(DebounceSlot::ThreadSearch);
        thread_search_matches_.clear();
        thread_search_current_ = -1;
        thread_search_apply_highlights_();
        if (auto* bar = thread_search_bar_())
            bar->set_match_status(0, 0, false, true);
        return;
    }
    // Capture context so the debounce lambda can detect a stale query (the
    // user switched threads, or closed the panel, before this fires).
    const std::string search_room_id = current_room_id_;
    const std::string search_thread_root = current_thread_root;
    debounce_(DebounceSlot::ThreadSearch, 120,
              [this, query, search_room_id, search_thread_root]()
    {
        if (query.empty() || !client_)
            return;
        const std::string live_thread_root =
            main_room_pane_ ? main_room_pane_->thread_root() : std::string();
        if (current_room_id_ != search_room_id ||
            live_thread_root != search_thread_root)
            return;
        const std::uint64_t id = ++thread_search_request_id_;
        thread_search_pending_[id] = query;
        client_->search_messages(id, query, search_room_id, search_thread_root, 200);
        if (auto* bar = thread_search_bar_())
            bar->set_match_status(0, 0, /*searching=*/true, false);
    });
}

void ShellBase::handle_thread_search_results_ui_(
    std::uint64_t request_id, std::vector<tesseract::SearchHit> results)
{
    auto it = thread_search_pending_.find(request_id);
    if (it == thread_search_pending_.end())
        return;
    thread_search_pending_.erase(it);
    if (request_id != thread_search_request_id_)
        return;

    // Sort ascending by timestamp (index 0 = oldest match), mirroring the
    // in-room search's ordering.
    std::sort(results.begin(), results.end(),
              [](const tesseract::SearchHit& a, const tesseract::SearchHit& b)
              { return a.timestamp_ms < b.timestamp_ms; });

    std::string prev_focused;
    if (thread_search_current_ >= 0 &&
        thread_search_current_ < static_cast<int>(thread_search_matches_.size()))
    {
        prev_focused = thread_search_matches_[
            static_cast<std::size_t>(thread_search_current_)].event_id;
    }

    thread_search_matches_ = std::move(results);
    thread_search_apply_highlights_();

    const int total = static_cast<int>(thread_search_matches_.size());
    if (total == 0)
    {
        thread_search_current_ = -1;
        if (auto* bar = thread_search_bar_())
            bar->set_match_status(0, 0, false, /*at_start=*/true);
        return;
    }

    thread_search_current_ = total - 1;
    if (!prev_focused.empty())
    {
        for (int i = 0; i < total; ++i)
        {
            if (thread_search_matches_[static_cast<std::size_t>(i)].event_id ==
                prev_focused)
            {
                thread_search_current_ = i;
                break;
            }
        }
    }
    thread_search_focus_current_();
}

void ShellBase::handle_thread_search_failed_ui_(std::uint64_t request_id,
                                                const std::string&)
{
    auto it = thread_search_pending_.find(request_id);
    if (it == thread_search_pending_.end())
        return;
    thread_search_pending_.erase(it);
    if (request_id != thread_search_request_id_)
        return;
    thread_search_matches_.clear();
    thread_search_current_ = -1;
    thread_search_apply_highlights_();
    if (auto* bar = thread_search_bar_())
        bar->set_match_status(0, 0, false, /*at_start=*/true);
}

void ShellBase::thread_search_apply_highlights_()
{
    auto* tv = room_view_ ? room_view_->thread_view() : nullptr;
    auto* ml = tv ? tv->message_list() : nullptr;
    if (!ml)
        return;
    if (thread_search_matches_.empty())
    {
        ml->clear_search_matches();
        ml->set_highlighted_event({});
        return;
    }
    std::unordered_set<std::string> ids;
    ids.reserve(thread_search_matches_.size());
    for (const auto& hit : thread_search_matches_)
        ids.insert(hit.event_id);
    ml->set_search_matches(std::move(ids));
}

void ShellBase::thread_search_focus_current_()
{
    if (thread_search_matches_.empty() || thread_search_current_ < 0)
        return;
    const int total = static_cast<int>(thread_search_matches_.size());
    if (thread_search_current_ >= total)
        thread_search_current_ = total - 1;

    const auto& hit = thread_search_matches_[
        static_cast<std::size_t>(thread_search_current_)];
    auto* tv = room_view_ ? room_view_->thread_view() : nullptr;
    auto* ml = tv ? tv->message_list() : nullptr;
    if (ml)
    {
        ml->set_highlighted_event(hit.event_id);
        // A thread's messages are already fully loaded via subscribe_thread
        // (no lazy backfill the way the main room has), so — unlike in-room
        // search — there's nothing to paginate in if this returns false.
        ml->scroll_to_event_id(hit.event_id);
    }
    if (auto* bar = thread_search_bar_())
        bar->set_match_status(thread_search_current_ + 1, total, false,
                              /*at_start=*/true);
}

void ShellBase::thread_search_navigate_(int delta)
{
    const int total = static_cast<int>(thread_search_matches_.size());
    if (total == 0)
        return;
    if (thread_search_current_ < 0)
        thread_search_current_ = total - 1;
    int next = thread_search_current_ + delta;
    if (next < 0)
        next = total - 1; // wrap to newest
    else if (next >= total)
        next = 0; // wrap to oldest
    thread_search_current_ = next;
    thread_search_focus_current_();
}

void ShellBase::thread_search_clear_()
{
    cancel_debounce_(DebounceSlot::ThreadSearch);
    thread_search_pending_.clear();
    thread_search_matches_.clear();
    thread_search_current_ = -1;
}

// Each shell points `settings_view_` at its shared SettingsView once, and
// calls start_/stop_ when its Settings panel opens/closes. The refresh
// fetches stats from the active account's client and pushes them to the
// view, re-arming a slow poll while the history backfill runs.
// The same start_/stop_ pair also drives the About tab's live cache-size
// refresh (refresh_cache_sizes_poll_()).
void ShellBase::start_search_index_stats_poll_()
{
    search_stats_panel_open_ = true;
    refresh_search_index_stats_();
    // The shell already computed the sizes once on open, so only arm the
    // next tick here rather than computing again.
    if (stats_settings_view_ && stats_settings_view_->about_tab_selected())
        debounce_(DebounceSlot::CacheSizes, kCacheSizesPollMs,
                  [this] { refresh_cache_sizes_poll_(); });
}

void ShellBase::stop_search_index_stats_poll_()
{
    search_stats_panel_open_ = false;
    cancel_debounce_(DebounceSlot::SearchStats);
    cancel_debounce_(DebounceSlot::CacheSizes);
}

void ShellBase::refresh_cache_sizes_poll_()
{
    if (!search_stats_panel_open_ || !stats_settings_view_ ||
        !stats_settings_view_->about_tab_selected())
    {
        cancel_debounce_(DebounceSlot::CacheSizes);
        return;
    }
    compute_cache_sizes_([this](uint64_t local, uint64_t sdk, uint64_t memory,
                                uint64_t mh, uint64_t mm,
                                uint64_t dh, uint64_t dm)
    {
        if (!search_stats_panel_open_ || !stats_settings_view_)
            return;
        stats_settings_view_->set_cache_sizes(local, sdk, memory, mh, mm, dh,
                                              dm);
        // Re-arm only after the result lands, so a slow directory walk never
        // stacks up overlapping computes; the debounce generation collapses a
        // tab-switch kick and a pending tick into one loop.
        if (stats_settings_view_->about_tab_selected())
            debounce_(DebounceSlot::CacheSizes, kCacheSizesPollMs,
                      [this] { refresh_cache_sizes_poll_(); });
    });
}

void ShellBase::refresh_search_index_stats_()
{
    if (!search_stats_panel_open_ || !stats_settings_view_)
        return;
    const bool enabled = tesseract::Settings::instance().index_messages_for_search;

    // `search_index_stats()` is an aggregate scan and `search_index_size_bytes()`
    // a `dbstat` B-tree walk, both taken under the search-db lock — which the
    // history-backfill indexer holds for long INSERT batches. Running them on the
    // UI thread stalled the Settings window open (and every 2s poll tick) until
    // that lock freed. Do the reads on a worker and post the result back.
    auto sess = active_account_;
    run_async_([this, sess, enabled]
    {
        tesseract::SearchIndexStats stats =
            (sess && sess->client) ? sess->client->search_index_stats()
                                   : tesseract::SearchIndexStats{};
        // Re-measure the on-disk size on every tick, not just at panel-open:
        // while the backfill runs it grows, and the poll stops the moment it
        // finishes, so a one-shot value would never reach the final figure.
        if (sess && sess->client)
            stats.index_bytes = sess->client->search_index_size_bytes();

        post_to_ui_alive_([this, stats, enabled]
        {
            if (!search_stats_panel_open_ || !stats_settings_view_)
                return;
            stats_settings_view_->set_search_index_stats(stats, enabled);
            // Keep polling (slowly) only while the panel is open, indexing is
            // on, and the history backfill is still running — so the counts
            // tick up live but we stop once it's "up to date". The debounce
            // generation guard prevents overlapping loops if start_ is called
            // again.
            if (enabled && !stats.backfill_done)
            {
                debounce_(DebounceSlot::SearchStats, 2000,
                          [this] { refresh_search_index_stats_(); });
            }
            else
            {
                cancel_debounce_(DebounceSlot::SearchStats);
            }
        });
    });
}

void ShellBase::handle_search_result_activated_(const std::string& room_id,
                                                const std::string& event_id)
{
    // Mirror the event-permalink path: navigate (sets current_room_id_
    // synchronously), then jump to the event via the deferred-scroll +
    // focused-subscription path so it works even when the target isn't loaded.
    tab_navigate_room(room_id);
    if (!event_id.empty())
    {
        if (room_view_ && room_view_->message_list())
            room_view_->message_list()->set_highlighted_event(event_id);
        try_scroll_to_room_event_(event_id);
    }
}

void ShellBase::try_scroll_to_room_event_(const std::string& event_id)
{
    if (event_id.empty() || !room_view_ || current_room_id_.empty() || !client_)
        return;
    auto* ml = room_view_->message_list();
    if (!ml)
        return;

    pending_scroll_room_event_id_ = event_id;
    ml->set_pending_scroll_event_id(event_id);
    request_repaint_();

    // If the event is already in the loaded timeline the deferred scroll in
    // arrange() will apply it on the next paint — nothing else needed.
    for (const auto& m : ml->messages())
    {
        if (m.event_id == event_id)
            return;
    }

    // Event is not in the current window. Use subscribe_room_at to rebuild
    // the timeline centred on the target event (one /context fetch) rather
    // than paginating backwards batch-by-batch, which would flood the UI with
    // every intermediate event and can stall the main thread.
    const auto rid = current_room_id_;
    if (main_room_pane_)
        main_room_pane_->begin_focused_subscription_(rid, event_id);
    if (auto* ml = room_view_->message_list())
        ml->begin_nav_loading();
    auto sess = active_account_;
    run_async_mut_([sess, rid, event_id]()
    {
        if (!sess || !sess->client) return;
        sess->client->subscribe_room_at(rid, event_id);
    });
}

void ShellBase::clear_focused_state_(const std::string& room_id)
{
    auto& state = pagination_[room_id];
    state.is_focused = false;
    state.focus_event_id.clear();
    state.reached_end = false;
    state.fwd_in_flight = false;
}
} // namespace tesseract
