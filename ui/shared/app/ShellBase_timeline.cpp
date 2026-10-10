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

std::vector<views::MessageRowData>
ShellBase::build_rows_(const EventList& snapshot, const std::string& room_id)
{
    std::vector<views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    // The event's own account: its "is this mine" flag and reply lookups
    // must not use the active account's for another account's pop-out.
    const std::string own_user_id = dispatch_account_();
    RoomPane* reply_pane = reply_pane_for_(room_id);
    // Only prefetch media for the trailing window — events at the top of the
    // snapshot are above the initial viewport and their media is fetched lazily
    // as the user scrolls up (via on_visible_rows_changed_).
    const std::size_t media_prefetch_window = media_prefetch_window_();
    const std::size_t n = snapshot.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& ev = snapshot[i];
        if (!ev)
            continue;
        if (i + media_prefetch_window >= n)
        {
            prep_row_media_(*ev, /*fetch_avatars=*/false);
            media_prepped_event_ids_.insert(ev->event_id);
        }
        // room_id may not be current_room_id_ (see doc comment) — go through
        // the general (room_id-explicit) overload rather than the pane's own
        // implicit-current-room one. It fetches through the pane's account,
        // so reply_pane is a pane of the event's own account (see
        // reply_pane_for_); it never touches that pane's own room_id_.
        if (!ev->in_reply_to_id.empty() && reply_pane)
            reply_pane->ensure_reply_details_(room_id, ev->event_id,
                                                   std::string());
        rows.push_back(views::make_row_data(*ev, own_user_id));
    }
    return rows;
}

std::vector<views::MessageRowData>
ShellBase::build_rows_(const std::vector<Event*>& snapshot,
                       const std::string& room_id)
{
    std::vector<views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    // The event's own account: its "is this mine" flag and reply lookups
    // must not use the active account's for another account's pop-out.
    const std::string own_user_id = dispatch_account_();
    RoomPane* reply_pane = reply_pane_for_(room_id);
    const std::size_t media_prefetch_window = media_prefetch_window_();
    const std::size_t n = snapshot.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        auto* ev = snapshot[i];
        if (!ev)
            continue;
        if (i + media_prefetch_window >= n)
        {
            prep_row_media_(*ev, /*fetch_avatars=*/false);
            media_prepped_event_ids_.insert(ev->event_id);
        }
        if (!ev->in_reply_to_id.empty() && reply_pane)
            reply_pane->ensure_reply_details_(room_id, ev->event_id,
                                                   std::string());
        rows.push_back(views::make_row_data(*ev, own_user_id));
    }
    return rows;
}

void ShellBase::push_paginate_result_(std::string room_id, bool reached_start,
                                      const std::string& user_id)
{
    auto& state = pagination_for_(user_id, room_id);
    state.in_flight = false;
    state.reached_start = reached_start;
    const bool for_active = user_id.empty() ||
                            (active_account_ && active_account_->user_id == user_id);
    if (for_active && room_id == current_room_id_ && room_view_)
    {
        room_view_->set_paginating(false);
        schedule_relayout_();
    }
}

void ShellBase::handle_room_export_progress_ui_(
    const tesseract::RoomExportProgress& progress)
{
    if (history_export_controller_)
        history_export_controller_->handle_progress(progress);
}

void ShellBase::handle_room_export_complete_ui_(
    std::uint64_t request_id, bool ok, bool cancelled, bool reached_start,
    std::string out_path, std::uint64_t events_written,
    std::uint64_t bytes_written, std::string message)
{
    if (history_export_controller_)
        history_export_controller_->handle_complete(
            request_id, ok, cancelled, reached_start, std::move(out_path),
            events_written, bytes_written, std::move(message));
}

void ShellBase::handle_paginate_result_ui_(std::uint64_t request_id, bool ok,
                                           bool reached_start, bool reached_end,
                                           std::string /*message*/)
{
    auto it = pending_paginates_.find(request_id);
    if (it == pending_paginates_.end())
        return;
    auto [room_id, is_backward] = it->second;
    pending_paginates_.erase(it);

    // The result comes from the account that paginated, which for a pop-out
    // may not be the active one.
    auto& state = pagination_for_(dispatch_account_(), room_id);
    if (is_backward)
    {
        state.in_flight = false;
        if (ok)
            state.reached_start = reached_start;

        if (main_window_shows_(room_id) && room_view_)
        {
            room_view_->set_paginating(false);
            schedule_relayout_();
        }
        // If the in-room search bar triggered this paginate, re-run the query
        // so newly-indexed older events appear in the results.
        if (in_room_search_rerun_on_paginate_ &&
            room_id == in_room_search_room_id_ &&
            room_view_ && room_view_->room_search_open())
        {
            in_room_search_rerun_on_paginate_ = false;
            // Bypass the user-typing debounce: fire the re-index search
            // immediately so the next paginate can start without a 120 ms gap.
            if (auto* bar = in_room_search_bar_())
            {
                const std::string& q = bar->query();
                if (!q.empty() && client_)
                {
                    cancel_debounce_(DebounceSlot::InRoomSearch);
                    // Record state so the results handler knows this search
                    // was triggered by a paginate batch and can keep looping
                    // if no new matches appeared.
                    in_room_search_paginate_rerun_   = true;
                    in_room_search_prev_match_count_ =
                        static_cast<int>(in_room_search_matches_.size());
                    const std::uint64_t id = ++in_room_search_request_id_;
                    in_room_search_pending_[id] = q;
                    client_->search_messages(id, q, in_room_search_room_id_, std::string(), 200);
                    // Don't reset the label to "Searching…" during pagination;
                    // the paginating spinner is sufficient feedback.
                }
            }
        }
    }
    else
    {
        state.fwd_in_flight = false;
        if (ok)
        {
            state.reached_end = reached_end;
            if (reached_end && main_room_pane_)
                main_room_pane_->return_to_live_(room_id);
        }
    }
}

// The gallery reuses the room's already-active Timeline subscription
// (no dedicated Rust/FFI surface) and filters raw pagination batches to
// Image/Video client-side, so a single scroll-to-top gesture may need
// several backend round-trips in a media-sparse room. Opening/closing,
// pagination, and retry/accumulate state all live on RoomPane now
// (RoomPane::open_room_media_view_ etc.) — used identically by the main
// window's main_room_pane_ and every pop-out's own pane_, so this class
// only needs the one thing a per-pane object structurally can't provide
// itself: routing IEventHandler::on_media_view_paginate_result (which
// has no per-window addressing of its own) back to whichever RoomPane
// actually issued the request.
//
// Completion callback for paginate_media_view_back_async. Looks up
// media_view_paginate_owners_ and forwards to the owning RoomPane's
// handle_media_view_paginate_result_, which decides whether to fire
// another round based on an authoritative Image/Video count read
// directly from the SDK's timeline — see RoomPane.cpp and
// paginate_media_view_back_async's doc comment for why this replaced an
// earlier design that raced against the separate diff-streaming task.
void ShellBase::handle_media_view_paginate_result_ui_(
    std::uint64_t request_id, bool ok, bool reached_start,
    std::uint64_t media_count, std::string /*message*/)
{
    auto it = pending_paginates_.find(request_id);
    if (it == pending_paginates_.end())
        return;
    const std::string room_id = it->second.first;
    pending_paginates_.erase(it);

    auto& state = pagination_for_(dispatch_account_(), room_id);
    state.in_flight = false;
    if (ok)
        state.reached_start = reached_start;

    auto owner_it = media_view_paginate_owners_.find(request_id);
    if (owner_it == media_view_paginate_owners_.end())
        return;
    RoomPane* owner = owner_it->second;
    media_view_paginate_owners_.erase(owner_it);
    owner->handle_media_view_paginate_result_(request_id, ok,
                                              state.reached_start, media_count);
}

void ShellBase::handle_room_media_page_ui_(
    std::uint64_t request_id, std::vector<tesseract::MediaIndexRow> rows,
    bool reached_db_end, std::uint64_t total)
{
    // DB-page requests share the media_view_paginate_owners_ map + the
    // next_paginate_id_ id space with the network path, but are deliberately
    // NOT in pending_paginates_ (they touch no SDK timeline / tokio task).
    // Route purely by owner; the RoomPane guards on its own request_id.
    auto owner_it = media_view_paginate_owners_.find(request_id);
    if (owner_it == media_view_paginate_owners_.end())
        return;
    RoomPane* owner = owner_it->second;
    owner->handle_room_media_page_(request_id, std::move(rows), reached_db_end,
                                   total);
}

void ShellBase::maybe_send_read_receipt_(const std::string& room_id,
                                         const std::string& event_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || event_id.empty())
    {
        return;
    }
    auto& last = last_sent_receipt_[room_id];
    if (last == event_id)
    {
        return;
    }
    last = event_id;
    auto sess = acting;
    run_async_mut_(
        [sess, room_id, event_id]()
        {
            if (sess && sess->client)
            {
                sess->client->send_read_receipt(room_id, event_id);
            }
        });

    // Defer the m.fully_read move so the "New messages" divider lingers. Only
    // the first receipt of a batch arms the timer; later ones advance the
    // target without restarting it, so a busy room still clears on time.
    const std::string key = (sess ? sess->user_id : std::string{}) + "\x1F" + room_id;
    auto [it, inserted] = pending_fully_read_.try_emplace(key);
    it->second.room_id  = room_id;
    it->second.event_id = event_id;
    it->second.sess     = sess;
    if (inserted)
    {
        post_to_ui_after_(kFullyReadLingerMs,
                          guarded([this, key] { flush_fully_read_(key); }));
    }
}

void ShellBase::flush_fully_read_(const std::string& key)
{
    auto it = pending_fully_read_.find(key);
    if (it == pending_fully_read_.end())
    {
        return;
    }
    PendingFullyRead p = std::move(it->second);
    pending_fully_read_.erase(it);
    run_async_mut_(
        [this, p = std::move(p)]()
        {
            if (!p.sess || !p.sess->client)
            {
                return;
            }
            if (!p.sess->client->set_fully_read_marker(p.room_id, p.event_id).ok)
            {
                return;
            }
            post_to_ui_alive_(
                [this, room_id = p.room_id, user_id = p.sess->user_id]()
                {
                    notify_fully_read_moved_(user_id, room_id);
                });
        });
}

void ShellBase::flush_pending_fully_read_now_(const std::string& user_id)
{
    for (auto it = pending_fully_read_.begin(); it != pending_fully_read_.end();)
    {
        const PendingFullyRead& p = it->second;
        if (!user_id.empty() && (!p.sess || p.sess->user_id != user_id))
        {
            ++it;
            continue;
        }
        // Best-effort and synchronous: the caller is about to stop this
        // client, which would cancel a queued worker.
        if (p.sess && p.sess->client)
        {
            (void) p.sess->client->set_fully_read_marker(p.room_id, p.event_id);
        }
        it = pending_fully_read_.erase(it); // the armed timer finds nothing
    }
}

void ShellBase::notify_fully_read_moved_(const std::string& user_id,
                                         const std::string& room_id)
{
    if (main_room_pane_ && active_account_ &&
        active_account_->user_id == user_id && current_room_id_ == room_id)
    {
        main_room_pane_->on_fully_read_moved();
    }
    for (const auto& w : owned_secondary_windows_)
    {
        if (w->room_id() == room_id && w->owner_user_id() == user_id && w->pane())
        {
            w->pane()->on_fully_read_moved();
        }
    }
}

bool ShellBase::room_is_shown_(const std::string& user_id,
                               const std::string& room_id) const
{
    if (room_id.empty())
    {
        return false;
    }
    if (room_id == current_room_id_ && active_account_ &&
        active_account_->user_id == user_id)
    {
        return true;
    }
    for (const auto& w : owned_secondary_windows_)
    {
        if (w->room_id() == room_id && w->owner_user_id() == user_id)
        {
            return true;
        }
    }
    return false;
}

void ShellBase::forget_thread_receipts_(const std::string& room_id)
{
    // Keys are "room_id\x1Fthread_root" — drop every thread of this room.
    const std::string prefix = room_id + "\x1F";
    for (auto it = last_sent_thread_receipt_.begin();
         it != last_sent_thread_receipt_.end();)
    {
        if (it->first.rfind(prefix, 0) == 0)
            it = last_sent_thread_receipt_.erase(it);
        else
            ++it;
    }
}

void ShellBase::maybe_send_thread_read_receipt_(const std::string& room_id,
                                                const std::string& thread_root,
                                                const std::string& event_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty() || thread_root.empty() || event_id.empty())
        return;
    auto& last = last_sent_thread_receipt_[room_id + "\x1F" + thread_root];
    if (last == event_id)
        return;
    last = event_id;
    auto sess = acting;
    run_async_mut_(
        [sess, room_id, thread_root, event_id]()
        {
            if (sess && sess->client)
                sess->client->send_thread_read_receipt(room_id, thread_root, event_id);
        });
}

void ShellBase::mark_all_threads_read_(const std::string& room_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (room_id.empty())
        return;
    // The Rust side writes optimistic markers + fires on_threads_updated, so
    // the panel/header dots clear on the next list_room_threads re-query
    // without any local bookkeeping here.
    auto sess = acting;
    run_async_mut_(
        [sess, room_id]()
        {
            if (sess && sess->client)
                sess->client->mark_all_threads_read(room_id);
        });
}

void ShellBase::mark_room_read_(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    for (auto& r : rooms_)
    {
        if (r.id == room_id)
        {
            r.notification_count = 0;
            r.highlight_count    = 0;
            break;
        }
    }
    auto it = per_account_rooms_.find(my_user_id_);
    if (it != per_account_rooms_.end())
    {
        for (auto& r : it->second)
        {
            if (r.id == room_id)
            {
                r.notification_count = 0;
                r.highlight_count    = 0;
                break;
            }
        }
    }
    on_rooms_updated_();
    notify_tray_unread_();
    auto sess = active_account_;
    const bool include_fully_read =
        sess && !room_is_shown_(sess->user_id, room_id);
    if (include_fully_read && sess)
    {
        // The marker jumps to the latest event now; a pending deferred move
        // would only drag it back.
        pending_fully_read_.erase(sess->user_id + "\x1F" + room_id);
    }
    run_async_mut_(
        [sess, room_id, include_fully_read]()
        {
            if (sess && sess->client)
            {
                sess->client->mark_room_as_read(room_id, include_fully_read);
            }
        });
}

void ShellBase::handle_voice_waveform_ready_ui_(
    std::string room_id, std::string event_id,
    std::vector<std::uint16_t> waveform)
{
    if (room_id != current_room_id_)
    {
        return;
    }
    if (room_view_)
    {
        if (auto* ml = room_view_->message_list())
        {
            ml->update_voice_waveform(event_id, std::move(waveform));
        }
    }
}

bool ShellBase::tick_anim_()
{
    // Stop immediately when every shell window is hidden or minimized.
    if (!any_window_visible_())
    {
        stop_anim_tick_();
        return false;
    }

    const std::int64_t now = monotonic_ms_();

    // Stop once nothing animated is on-screen — entries linger in the cache
    // after scrolling away / switching rooms, so checking emptiness would keep
    // the 60 Hz timer (and its repaints) running forever.
    // Also keep running while the back-pagination spinner is visible: its
    // rotation phase is computed from elapsed wall-clock time at paint time
    // (MessageListView::draw_pagination_spinner_), so it only advances on
    // screen when something actually repaints it — nothing else drives that
    // on its own, hence gif_frame || spinner_active below.
    const bool spinner_active = room_view_ && room_view_->message_list() &&
                                room_view_->message_list()->paginating();
    if (!account_manager_.anim_cache().any_visible() && !spinner_active)
    {
        stop_anim_tick_();
        return false;
    }
    const bool gif_frame = account_manager_.anim_cache().advance(now);

    // Dispatch any windowed entries' pending decode-ahead requests off the
    // UI thread. On a backend with no windowed AnimDecodeSession yet (see
    // ShellBase::decode_image_streamed_windowed_'s default), this is always
    // empty — zero behavior change until a shell actually produces a
    // session. Each request's session shared_ptr travels with the lambda,
    // so it stays valid even if the owning cache entry is evicted mid-decode
    // (see AnimDecodeSession's ownership contract).
    for (auto& req : account_manager_.anim_cache().collect_topups())
    {
        run_async_(
            [this, req]() mutable
            {
                req.session->decode_next_batch(
                    req.batch_size,
                    [this, key = req.key, session = req.session](
                        int /*frame_index*/, std::unique_ptr<tk::Image> frame,
                        int delay_ms) mutable
                    {
                        auto boxed = std::make_shared<std::unique_ptr<tk::Image>>(
                            std::move(frame));
                        post_to_ui_(
                            [this, key, delay_ms, boxed, session]() mutable
                            {
                                // append_frame_from_session, not
                                // append_frame: see its doc comment — drops
                                // this frame if `key`'s entry has since been
                                // replaced by a different decode (a
                                // redundant concurrent fetch/decode race for
                                // the same key) rather than splicing it in.
                                account_manager_.anim_cache()
                                    .append_frame_from_session(
                                        key, session, std::move(*boxed),
                                        delay_ms);
                            });
                    });
                post_to_ui_(
                    [this, key = req.key]() mutable
                    { account_manager_.anim_cache().finish_topup(key); });
            });
    }

    if (gif_frame || spinner_active)
    {
        repaint_anim_frame_();
        // Pop-out windows have their own surfaces (and pickers) the shell's
        // repaint_anim_frame_ doesn't know about — advance them too.
        for (auto& w : owned_secondary_windows_)
        {
            if (w)
                w->repaint_anim_frame();
        }
    }
    return true;
}

bool ShellBase::inflight_tick_()
{
    if (!any_window_visible_() || !inflight_needs_anim_())
    {
        stop_inflight_tick_();
        return false;
    }
    spin_tick_(monotonic_ms_());
    repaint_inflight_spinner_();
    return true;
}

// Coalescing relayout. Instead of running a synchronous measure+arrange of
// the whole widget tree on every call (which a sync burst does N times),
// this posts a single deferred flush to the UI thread; further calls before
// that flush runs are folded into it. The flush still calls the synchronous
// request_relayout_() exactly once, so native-overlay positioning timing is
// unchanged — only the redundant per-message passes are eliminated. Use for
// hot, high-frequency paths (incoming-message handlers); keep
// request_relayout_() where a later step in the same turn reads geometry.
void ShellBase::schedule_relayout_()
{
    if (relayout_scheduled_)
    {
        return; // a flush is already queued — fold this request into it
    }
    relayout_scheduled_ = true;
    post_to_ui_alive_(
        [this]
        {
            relayout_scheduled_ = false;
            request_relayout_();
        });
}

void ShellBase::handle_timeline_reset_ui_(std::string room_id,
                                          EventList snapshot)
{
    if (main_window_shows_(room_id) && room_view_)
    {
        // RoomPane::on_timeline_reset applies the same display gate (genuine
        // switch, OR re-population of an emptied view — e.g. logout ->
        // login -> same room), calls set_messages()/relayout, and re-asserts
        // this pane's own focused/historical/return-to-live scroll state
        // (keyed by pagination_[room_id]) — the same method every pop-out
        // already uses via dispatch_timeline_reset_secondary_ below. The
        // main-window-only richness that follows (pinned-banner refresh,
        // pending-scroll re-arm, the tabs_ saved-scroll-offset restore —
        // pop-outs have no tabs_ concept) stays here.
        const bool room_switch =
            main_room_pane_->on_timeline_reset(build_rows_(snapshot, room_id));
        // A full reset can land a reply row and its quoted target in the same
        // snapshot without ever going through the insert/prepend/append paths
        // that already retry a stale placeholder (see
        // retry_stale_reply_previews_ call sites below). It's also how a
        // room returns after aging out of the warm-subscription LRU, whose
        // rebuilt SDK-side timeline resets any prior TimelineDetails back to
        // Unavailable — so without this, reply_details_requested_ would keep
        // silently skipping the re-fetch a stuck row now needs.
        {
            std::vector<std::string> ids;
            ids.reserve(snapshot.size());
            for (const auto& ev : snapshot)
            {
                if (ev)
                    ids.push_back(ev->event_id);
            }
            if (main_room_pane_)
                main_room_pane_->retry_stale_reply_previews_(ids);
        }
        // Re-assert the pin banner/permission state on a genuine switch.
        // after_active_room_changed_() already did this for the common case,
        // but a room_switch can also happen here without that call running
        // (e.g. re-population of an emptied view without current_room_id_
        // changing) — refresh_pinned_for_current_room_() is idempotent, so
        // calling it again is cheap and removes any dependency on ordering
        // between this async callback and that synchronous call.
        if (room_switch)
            refresh_pinned_for_current_room_();
        // set_messages() clears MessageListView::pending_scroll_event_id_.
        // Re-arm it so arrange() still applies the deferred scroll with
        // up-to-date row_offsets_ (handles the subscribe_room_at reset path).
        if (!pending_scroll_room_event_id_.empty() &&
            room_view_->message_list())
        {
            room_view_->message_list()->set_pending_scroll_event_id(
                pending_scroll_room_event_id_);
        }
        // Restore saved scroll offset when returning to a tab that had been
        // scrolled up from the bottom. Main-window-only — tabs_ is a
        // ShellBase (not RoomPane) concept, since pop-outs show exactly one
        // room for their lifetime and have no tab list to restore from.
        // (Focused-gate/historical-mode/return-to-live scroll handling now
        // lives on RoomPane::on_timeline_reset, called just above via
        // main_room_pane_ — shared with pop-outs, see there.)
        if (auto* list = room_view_->message_list())
        {
            const auto& pstate = pagination_[room_id];
            if (room_switch && !pstate.is_focused &&
                active_tab_idx_ < tabs_.size() &&
                tabs_[active_tab_idx_].room_id == room_id &&
                tabs_[active_tab_idx_].scroll_offset > 0.f)
            {
                list->scroll_to_offset(tabs_[active_tab_idx_].scroll_offset);
            }
        }
    }

    // Room-media gallery: a reset replaces the whole known set (e.g. a
    // reconnect re-subscribe), not just a prepend. main_room_pane_ checks
    // room_id against its own media_view_room_id() (may differ from
    // current_room_id_ if the gallery is pinned open on a room the user has
    // since navigated away from) and no-ops if the gallery isn't open for it.
    if (main_room_pane_ && event_is_for_active_account_())
    {
        main_room_pane_->feed_gallery_reset_(room_id,
                                             build_rows_(snapshot, room_id));
    }

    dispatch_timeline_reset_secondary_(room_id, snapshot);
}

void ShellBase::handle_message_inserted_ui_(std::string room_id,
                                            std::size_t index,
                                            std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }
    note_member_event_(room_id, *ev);
    // In-thread replies belong to a thread, not the main timeline. The main
    // window's list excludes them; pop-out main lists must do the same, or
    // their rows diverge from the main window and later update/remove indices
    // (which arrive in main-timeline coordinates) land on the wrong rows.
    const bool in_thread = !ev->thread_root_id.empty();
    // The room_view_ insert below is NOT delegated to
    // main_room_pane_->on_message_inserted() (unlike handle_timeline_reset_ui_):
    // that method's own room_view_ path calls deps_.relayout() directly
    // (the immediate, uncoalesced main-window relayout), which would revert
    // the schedule_relayout_() burst-coalescing below. The gallery-append
    // part is delegated instead, via feed_gallery_live_ further down —
    // that one has no coalescing concern and correctly targets
    // main_room_pane_'s own media_view_room_id() rather than room_id_, so it
    // still fires when the gallery is pinned open on a room other than
    // current_room_id_.
    // index is relative to the SDK's full, untrimmed timeline. A room switch
    // may have withheld the oldest rows from room_view_ (see
    // ShellBase::kSwitchDisplayCap / RoomPane::withheld_older_rows_) without
    // the SDK ever finding out, so it must be translated into room_view_'s
    // own (shorter) index space before use — an index landing inside the
    // withheld region goes into that buffer instead, keeping it (and
    // withheld_count()) in step with the SDK.
    bool into_withheld = false;
    if (main_window_shows_(room_id) && !in_thread && main_room_pane_ &&
        index < main_room_pane_->withheld_count())
    {
        auto row = tesseract::views::make_row_data(*ev, dispatch_account_());
        into_withheld = main_room_pane_->withheld_insert(index, row);
    }
    const std::size_t withheld =
        main_room_pane_ ? main_room_pane_->withheld_count() : 0;
    if (main_window_shows_(room_id) && !in_thread && room_view_ &&
        !into_withheld)
    {
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty() && main_room_pane_)
        {
            main_room_pane_->ensure_reply_details_(ev->event_id);
        }
        room_view_->insert_message(
            index - withheld, tesseract::views::make_row_data(*ev, dispatch_account_()));
        if (main_room_pane_)
            main_room_pane_->retry_stale_reply_previews_({ev->event_id});
        schedule_relayout_(); // coalesce bursts into one layout pass
    }
    // Room-media gallery: append newly-arrived live media (e.g. someone
    // just posted an image while the gallery is open). Edits/redactions to
    // an existing gallery item are intentionally not propagated live for
    // v1 — the count and grid are already documented as reflecting what was
    // known as of the last (re)load, not a strictly live view.
    // feed_gallery_live_ checks room_id against main_room_pane_'s own
    // media_view_room_id() and self-filters to Image/Video.
    if (!in_thread && main_room_pane_ && event_is_for_active_account_())
    {
        main_room_pane_->feed_gallery_live_(
            room_id, tesseract::views::make_row_data(*ev, dispatch_account_()),
            /*prepend=*/false);
    }
    if (!in_thread)
    {
        dispatch_message_inserted_secondary_(room_id, index, *ev);
    }
}

void ShellBase::handle_message_updated_ui_(std::string room_id,
                                           std::size_t index,
                                           std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }
    note_member_event_(room_id, *ev);
    // See handle_message_inserted_ui_: in-thread replies are excluded from the
    // main timeline on both the main window and pop-outs, keeping their rows
    // aligned with the main-timeline indices used by updates/removals.
    const bool in_thread = !ev->thread_root_id.empty();
    // See handle_message_inserted_ui_: index is relative to the SDK's full
    // timeline and must be translated past any withheld (not-yet-displayed)
    // rows. An index still inside the withheld region updates that buffer
    // instead (nothing displayed yet, so no prep_row_media_ /
    // ensure_reply_details_ to pay for).
    bool in_withheld = false;
    if (main_window_shows_(room_id) && !in_thread && main_room_pane_ &&
        index < main_room_pane_->withheld_count())
    {
        auto row = tesseract::views::make_row_data(*ev, dispatch_account_());
        in_withheld = main_room_pane_->withheld_update(index, row);
    }
    const std::size_t withheld =
        main_room_pane_ ? main_room_pane_->withheld_count() : 0;
    if (main_window_shows_(room_id) && !in_thread && room_view_ &&
        !in_withheld)
    {
        // NOT delegated to main_room_pane_->on_message_updated() — that
        // calls deps_.relayout(), which for the main window is the
        // immediate, uncoalesced request_relayout_(). schedule_relayout_()
        // below coalesces bursts (many edits/redactions landing in the same
        // sync batch) into one deferred layout pass; pop-outs never had
        // this optimization and are unaffected either way, since
        // dispatch_message_updated_secondary_ still calls
        // RoomWindowBase::on_message_updated -> RoomPane::on_message_updated
        // directly, unchanged.
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty() && main_room_pane_)
        {
            main_room_pane_->ensure_reply_details_(ev->event_id);
        }
        room_view_->update_message(
            index - withheld, tesseract::views::make_row_data(*ev, dispatch_account_()));
        if (!ev->in_reply_to_id.empty() && !ev->in_reply_to_sender_name.empty())
        {
            // Reply metadata just resolved (or this is a subsequent update
            // after resolution — notify_reply_ready on an already-cleared/
            // absent gate key is a harmless no-op). Clears the room-switch
            // gate's pending entry for this row so a reveal isn't held up
            // once the quoted content is actually ready.
            room_view_->notify_reply_ready(ev->event_id);
        }
        // This event's reply-quote just (re)resolved in the main list. If
        // it's also the root of the currently-open thread, its copy there
        // was waiting on exactly this — see
        // RoomPane::sync_thread_root_reply_from_main_list_.
        if (!ev->in_reply_to_id.empty() && main_room_pane_ &&
            main_room_pane_->thread_root() == ev->event_id)
        {
            main_room_pane_->sync_thread_root_reply_from_main_list_();
        }
        schedule_relayout_(); // coalesce bursts into one layout pass
    }
    if (!in_thread)
    {
        dispatch_message_updated_secondary_(room_id, index, *ev);
    }
}

void ShellBase::handle_message_removed_ui_(std::string room_id,
                                           std::size_t index)
{
    // NOT delegated to main_room_pane_->on_message_removed() — see
    // handle_message_updated_ui_ above for why (relayout coalescing).
    // See handle_message_inserted_ui_ for the withheld-region index
    // translation this needs.
    const bool in_withheld = main_window_shows_(room_id) && main_room_pane_ &&
                             main_room_pane_->withheld_remove(index);
    const std::size_t withheld =
        main_room_pane_ ? main_room_pane_->withheld_count() : 0;
    if (main_window_shows_(room_id) && room_view_ && !in_withheld)
    {
        room_view_->remove_message(index - withheld);
        schedule_relayout_(); // coalesce bursts into one layout pass
    }
    dispatch_message_removed_secondary_(room_id, index);
}

// room_view_'s own batch prepend/append below is NOT delegated to RoomPane
// (unlike timeline_reset/message_updated/message_removed above): RoomPane
// has no batch prepend/append method for room_view_ itself — pop-outs
// receive these as a series of per-event on_message_inserted calls (see the
// dispatch loops below), which is the pre-existing design this method's own
// pop-out dispatch already mirrors. Unifying the main window's batch
// prepend_messages()/append_messages() call onto that per-event path would
// risk changing its relayout/scroll-adjustment behavior (one batch op vs. N
// inserts) — left alone pending a dedicated look, not part of this pass.
// The gallery-rendering half below IS delegated, via
// feed_gallery_prepend_batch_ (a real batch method, since the gallery has
// no per-row relayout/scroll concern to preserve).
void ShellBase::handle_messages_prepended_ui_(std::string room_id,
                                              EventList events)
{
    const bool in_thread = !events.empty() && events.front() &&
                           !events.front()->thread_root_id.empty();
    if (main_window_shows_(room_id) && !in_thread && room_view_)
    {
        std::vector<views::MessageRowData> rows;
        std::vector<std::string> new_ids;
        rows.reserve(events.size());
        new_ids.reserve(events.size());
        for (auto& ev : events)
        {
            if (!ev || ev->type == tesseract::EventType::Unhandled)
                continue;
            // Prepended events land above the current viewport. Skip eager
            // media fetch; on_visible_rows_changed_ handles them lazily when
            // the user scrolls up to reveal them.
            if (!ev->in_reply_to_id.empty() && main_room_pane_)
                main_room_pane_->ensure_reply_details_(ev->event_id);
            new_ids.push_back(ev->event_id);
            rows.push_back(tesseract::views::make_row_data(*ev, dispatch_account_()));
        }
        if (!rows.empty())
        {
            room_view_->prepend_messages(std::move(rows));
            // Backward pagination is exactly how older, previously-unloaded
            // history reaches the client — retry any already-rendered reply
            // row still waiting on one of these newly-loaded events.
            if (main_room_pane_)
                main_room_pane_->retry_stale_reply_previews_(new_ids);
            schedule_relayout_();
        }
    }

    // Room-media gallery row rendering: filter this raw, unfiltered batch to
    // Image/Video and hand it to the gallery widget. events is oldest-first,
    // matching prepend_media's documented batch order. This is purely for
    // rendering — the retry/accumulate loop's stop decision (see
    // RoomPane::request_media_view_pagination_back_ /
    // handle_media_view_paginate_result_) uses an authoritative count read
    // directly from the SDK's timeline instead, since this delivery path
    // runs on a separate task that can lag behind how fast pagination
    // rounds resolve. Filtering to Image/Video here (not inside
    // feed_gallery_prepend_batch_) avoids converting every non-media event
    // in the batch just to have it dropped there.
    if (!in_thread && main_room_pane_ && event_is_for_active_account_())
    {
        std::vector<views::MessageRowData> media_rows;
        for (auto& ev : events)
        {
            if (!ev || (ev->type != tesseract::EventType::Image &&
                       ev->type != tesseract::EventType::Video))
                continue;
            media_rows.push_back(tesseract::views::make_row_data(*ev, dispatch_account_()));
        }
        if (!media_rows.empty())
        {
            main_room_pane_->feed_gallery_prepend_batch_(room_id,
                                                          std::move(media_rows));
        }
    }

    if (!in_thread)
    {
        // Events are oldest-first; replicate original PushFront-at-0 order by
        // dispatching newest-first so each secondary window sees the same
        // sequence of prepend calls as the pre-batching code path.
        for (auto it = events.crbegin(); it != events.crend(); ++it)
        {
            if (*it)
                dispatch_message_prepended_secondary_(room_id, **it);
        }
    }
}

void ShellBase::handle_messages_appended_ui_(std::string room_id,
                                             EventList events)
{
    for (const auto& e : events)
        if (e)
            note_member_event_(room_id, *e);
    const bool in_thread = !events.empty() && events.front() &&
                           !events.front()->thread_root_id.empty();
    if (main_window_shows_(room_id) && !in_thread && room_view_)
    {
        std::vector<views::MessageRowData> rows;
        std::vector<std::string> new_ids;
        rows.reserve(events.size());
        new_ids.reserve(events.size());
        for (auto& ev : events)
        {
            if (!ev || ev->type == tesseract::EventType::Unhandled)
                continue;
            // Suppress avatar fetches — on_visible_avatars_changed handles
            // lazy avatar loading for whatever is actually visible.
            prep_row_media_(*ev, /*fetch_avatars=*/false);
            if (!ev->in_reply_to_id.empty() && main_room_pane_)
                main_room_pane_->ensure_reply_details_(ev->event_id);
            new_ids.push_back(ev->event_id);
            rows.push_back(tesseract::views::make_row_data(*ev, dispatch_account_()));
        }
        if (!rows.empty())
        {
            room_view_->append_messages(std::move(rows));
            if (main_room_pane_)
                main_room_pane_->retry_stale_reply_previews_(new_ids);
            schedule_relayout_();
        }
    }
    if (!in_thread)
    {
        for (auto& ev : events)
        {
            if (ev)
                dispatch_message_appended_secondary_(room_id, *ev);
        }
    }
}

void ShellBase::handle_messages_updated_batch_ui_(std::string room_id,
                                                  std::vector<std::size_t> indices,
                                                  EventList events)
{
    for (const auto& e : events)
        if (e)
            note_member_event_(room_id, *e);
    const bool in_thread = !events.empty() && events.front() &&
                           !events.front()->thread_root_id.empty();
    // See handle_message_inserted_ui_ for the withheld-region index
    // translation this needs — indices here are individually just as
    // full-timeline-relative as the single-update path's.
    const std::size_t withheld =
        main_room_pane_ ? main_room_pane_->withheld_count() : 0;
    if (main_window_shows_(room_id) && !in_thread && room_view_)
    {
        for (std::size_t i = 0; i < indices.size() && i < events.size(); ++i)
        {
            auto& ev = events[i];
            if (!ev || ev->type == tesseract::EventType::Unhandled)
                continue;
            if (indices[i] < withheld)
            {
                auto row = tesseract::views::make_row_data(*ev, dispatch_account_());
                main_room_pane_->withheld_update(indices[i], row);
                continue;
            }
            // Batch updates can affect off-screen rows; suppress avatar fetches
            // so we don't bulk-request every sender across the entire history.
            prep_row_media_(*ev, /*fetch_avatars=*/false);
            if (!ev->in_reply_to_id.empty() && main_room_pane_)
                main_room_pane_->ensure_reply_details_(ev->event_id);
            room_view_->update_message(
                indices[i] - withheld,
                tesseract::views::make_row_data(*ev, dispatch_account_()));
        }
        if (!indices.empty())
            schedule_relayout_();
    }
    if (!in_thread)
    {
        for (std::size_t i = 0; i < indices.size() && i < events.size(); ++i)
        {
            if (events[i])
                dispatch_message_updated_secondary_(room_id, indices[i], *events[i]);
        }
    }
}

void ShellBase::handle_thread_messages_prepended_ui_(std::string room_id,
                                                     std::string thread_root,
                                                     EventList events)
{
    // Fan out to secondary window for this room if it has this thread open.
    {
        RoomWindowBase* sw = find_event_secondary_(room_id);
        if (sw &&
            sw->popout_thread_root() == thread_root)
        {
            std::vector<views::MessageRowData> rows;
            std::vector<std::string> new_ids;
            std::vector<std::string> reply_ids;
            rows.reserve(events.size());
            new_ids.reserve(events.size());
            for (auto& ev : events)
            {
                if (!ev || ev->type == tesseract::EventType::Unhandled)
                    continue;
                prep_row_media_(*ev);
                if (!ev->in_reply_to_id.empty())
                    reply_ids.push_back(ev->event_id);
                new_ids.push_back(ev->event_id);
                rows.push_back(tesseract::views::make_row_data(*ev, dispatch_account_()));
            }
            if (!rows.empty())
            {
                sw->apply_thread_prepend_(std::move(rows));
                // Resolve AFTER the rows exist in the thread's own list —
                // ensure_thread_reply_details_'s root-row case needs to find
                // its row there to copy an already-resolved main-list
                // preview into it.
                for (const auto& rid : reply_ids)
                    sw->ensure_thread_reply_details_(rid);
                // Backward pagination is exactly how older thread history
                // reaches the client — retry any already-rendered reply row
                // still waiting on one of these newly-loaded events.
                sw->retry_stale_thread_reply_previews_(new_ids);
            }
        }
    }
    if (!main_window_shows_(room_id) || !main_room_pane_ ||
        thread_root != main_room_pane_->thread_root())
        return;
    if (!room_view_)
        return;
    auto* tl = room_view_->thread_view();
    if (!tl)
        return;
    std::vector<views::MessageRowData> rows;
    std::vector<std::string> new_ids;
    std::vector<std::string> reply_ids;
    rows.reserve(events.size());
    new_ids.reserve(events.size());
    for (auto& ev : events)
    {
        if (!ev || ev->type == tesseract::EventType::Unhandled)
            continue;
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
            reply_ids.push_back(ev->event_id);
        new_ids.push_back(ev->event_id);
        rows.push_back(tesseract::views::make_row_data(*ev, dispatch_account_()));
    }
    if (!rows.empty())
    {
        tl->prepend_messages(std::move(rows));
        for (const auto& rid : reply_ids)
            main_room_pane_->ensure_thread_reply_details_(rid);
        main_room_pane_->retry_stale_thread_reply_previews_(new_ids);
        schedule_relayout_();
    }
}

void ShellBase::handle_thread_messages_appended_ui_(std::string room_id,
                                                    std::string thread_root,
                                                    EventList events)
{
    // Fan out to secondary window for this room if it has this thread open.
    {
        RoomWindowBase* sw = find_event_secondary_(room_id);
        if (sw &&
            sw->popout_thread_root() == thread_root)
        {
            std::vector<views::MessageRowData> rows;
            std::vector<std::string> new_ids;
            std::vector<std::string> reply_ids;
            rows.reserve(events.size());
            new_ids.reserve(events.size());
            for (auto& ev : events)
            {
                if (!ev || ev->type == tesseract::EventType::Unhandled)
                    continue;
                prep_row_media_(*ev);
                if (!ev->in_reply_to_id.empty())
                    reply_ids.push_back(ev->event_id);
                new_ids.push_back(ev->event_id);
                rows.push_back(tesseract::views::make_row_data(*ev, dispatch_account_()));
            }
            if (!rows.empty())
            {
                sw->apply_thread_append_(std::move(rows));
                for (const auto& rid : reply_ids)
                    sw->ensure_thread_reply_details_(rid);
                sw->retry_stale_thread_reply_previews_(new_ids);
            }
        }
    }
    if (!main_window_shows_(room_id) || !main_room_pane_ ||
        thread_root != main_room_pane_->thread_root())
        return;
    if (!room_view_)
        return;
    auto* tl = room_view_->thread_view();
    if (!tl)
        return;
    std::vector<views::MessageRowData> rows;
    std::vector<std::string> new_ids;
    std::vector<std::string> reply_ids;
    rows.reserve(events.size());
    new_ids.reserve(events.size());
    for (auto& ev : events)
    {
        if (!ev || ev->type == tesseract::EventType::Unhandled)
            continue;
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
            reply_ids.push_back(ev->event_id);
        new_ids.push_back(ev->event_id);
        rows.push_back(tesseract::views::make_row_data(*ev, dispatch_account_()));
    }
    if (!rows.empty())
    {
        tl->append_messages(std::move(rows));
        for (const auto& rid : reply_ids)
            main_room_pane_->ensure_thread_reply_details_(rid);
        main_room_pane_->retry_stale_thread_reply_previews_(new_ids);
        schedule_relayout_();
    }
}

void ShellBase::handle_thread_reset_ui_(std::string room_id,
                                        std::string thread_root,
                                        EventList snapshot)
{
    // Determine whether main window and/or a secondary window need this update.
    const bool main_matches =
        (main_window_shows_(room_id) && main_room_pane_ &&
         thread_root == main_room_pane_->thread_root());
    RoomWindowBase* popout_win = nullptr;
    {
        RoomWindowBase* sw = find_event_secondary_(room_id);
        if (sw &&
            sw->popout_thread_root() == thread_root)
            popout_win = sw;
    }
    if (!main_matches && !popout_win)
        return;

    // Prepare rows once; they may be delivered to main window, popout, or both.
    std::vector<views::MessageRowData> rows;
    std::vector<std::string> ids;
    std::vector<std::string> reply_ids;
    rows.reserve(snapshot.size());
    ids.reserve(snapshot.size());
    for (auto& ev : snapshot)
    {
        if (!ev || ev->type == tesseract::EventType::Unhandled)
            continue;
        prep_row_media_(*ev, /*fetch_avatars=*/false);
        if (!ev->in_reply_to_id.empty())
            reply_ids.push_back(ev->event_id);
        ids.push_back(ev->event_id);
        rows.push_back(tesseract::views::make_row_data(*ev, dispatch_account_()));
    }

    // Every thread reset comes from a freshly-built subscribe_thread timeline
    // with no reply details resolved yet. Dedup entries for these rows were
    // recorded against the previous (now dropped) timeline, so leaving them
    // would skip the fetch and leave a reopened thread's quotes unresolved.
    for (const auto& rid : reply_ids)
        reply_details_requested_.erase(reply_details_key_(dispatch_account_(), rid));

    // A full reset can land a reply row and its quoted target in the same
    // snapshot (see handle_timeline_reset_ui_'s identical rationale for the
    // main timeline) — retry both delivery targets against the whole
    // snapshot, not just the individually-resolved events above.
    if (popout_win)
    {
        popout_win->apply_thread_reset_(rows); // copies for popout
        // Resolve AFTER the rows exist in the thread's own list — see
        // handle_thread_inserted_ui_'s identical ordering rationale.
        for (const auto& rid : reply_ids)
            popout_win->ensure_thread_reply_details_(rid);
        popout_win->retry_stale_thread_reply_previews_(ids);
    }
    if (main_matches)
    {
        apply_thread_messages_(thread_root, std::move(rows), /*room_switch=*/true);
        for (const auto& rid : reply_ids)
            main_room_pane_->ensure_thread_reply_details_(rid);
        main_room_pane_->retry_stale_thread_reply_previews_(ids);
    }
}

void ShellBase::handle_thread_inserted_ui_(std::string room_id,
                                           std::string thread_root,
                                           std::size_t index,
                                           std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
        return;
    // Fan out to secondary window if it has this thread open.
    {
        RoomWindowBase* sw = find_event_secondary_(room_id);
        if (sw &&
            sw->popout_thread_root() == thread_root)
        {
            prep_row_media_(*ev);
            sw->apply_thread_insert_(
                index, tesseract::views::make_row_data(*ev, dispatch_account_()));
            // Resolve/sync AFTER the row exists in the thread's own list —
            // ensure_thread_reply_details_'s root-row case needs to find it
            // there to copy an already-resolved main-list preview into it.
            if (!ev->in_reply_to_id.empty())
                sw->ensure_thread_reply_details_(ev->event_id);
            sw->retry_stale_thread_reply_previews_({ev->event_id});
        }
    }
    if (!main_window_shows_(room_id) || !main_room_pane_ ||
        thread_root != main_room_pane_->thread_root())
        return;
    prep_row_media_(*ev);
    apply_thread_message_insert_(
        thread_root, index,
        tesseract::views::make_row_data(*ev, dispatch_account_()));
    if (!ev->in_reply_to_id.empty())
        main_room_pane_->ensure_thread_reply_details_(ev->event_id);
    main_room_pane_->retry_stale_thread_reply_previews_({ev->event_id});
}

void ShellBase::handle_thread_updated_ui_(std::string room_id,
                                          std::string thread_root,
                                          std::size_t index,
                                          std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
        return;
    // Fan out to secondary window if it has this thread open.
    {
        RoomWindowBase* sw = find_event_secondary_(room_id);
        if (sw &&
            sw->popout_thread_root() == thread_root)
        {
            prep_row_media_(*ev);
            sw->apply_thread_update_(
                index, tesseract::views::make_row_data(*ev, dispatch_account_()));
            // Resolve AFTER apply_thread_update_ — it overwrites the row
            // with freshly-converted (possibly still-unresolved) data, which
            // would otherwise clobber a sync done beforehand.
            if (!ev->in_reply_to_id.empty())
                sw->ensure_thread_reply_details_(ev->event_id);
        }
    }
    if (!main_window_shows_(room_id) || !main_room_pane_ ||
        thread_root != main_room_pane_->thread_root())
        return;
    prep_row_media_(*ev);
    apply_thread_message_update_(
        thread_root, index,
        tesseract::views::make_row_data(*ev, dispatch_account_()));
    if (!ev->in_reply_to_id.empty())
        main_room_pane_->ensure_thread_reply_details_(ev->event_id);
}

void ShellBase::handle_thread_removed_ui_(std::string room_id,
                                          std::string thread_root,
                                          std::size_t index)
{
    // Fan out to secondary window if it has this thread open.
    {
        RoomWindowBase* sw = find_event_secondary_(room_id);
        if (sw &&
            sw->popout_thread_root() == thread_root)
            sw->apply_thread_remove_(index);
    }
    if (!main_window_shows_(room_id) || !main_room_pane_ ||
        thread_root != main_room_pane_->thread_root())
        return;
    apply_thread_message_remove_(thread_root, index);
}

void ShellBase::handle_threads_updated_ui_(std::string room_id)
{
    if (!client_)
        return;

    // Always update the threads button on any secondary window showing this
    // room (a popout may have a different room_id than current_room_id_).
    {
        RoomWindowBase* sw = find_event_secondary_(room_id);
        auto owner = sw ? sw->owner_session() : nullptr;
        if (sw && sw->room_view() && owner && owner->client)
        {
            auto threads = owner->client->list_room_threads(room_id);
            const auto agg = views::aggregate_threads(threads);
            sw->room_view()->set_show_threads_button(!threads.empty());
            sw->room_view()->set_threads_unread(agg.any_unread,
                                                        agg.any_mention);
        }
    }

    // Update visibility regardless of panel state — the threads button needs
    // the latest list to decide whether to render. apply_threads_list_ no-ops
    // cheaply when the thread-list panel widget isn't around.
    if (!main_window_shows_(room_id))
        return;
    apply_threads_list_(client_->list_room_threads(room_id));
    // on_near_bottom only fires on user scroll, so it can't bootstrap the
    // initial fill when the first SDK page fits within the viewport. Drive
    // pagination here instead: each completed page triggers this callback,
    // which requests the next one — stopping when the controller reports
    // reached_start.
    if (main_room_pane_ && main_room_pane_->thread_panel() == ThreadPanel::List)
        main_room_pane_->paginate_threads_();
}

void ShellBase::handle_typing_changed_ui_(std::string room_id,
                                          std::vector<std::string> names)
{
    const std::string text = shell_helpers::format_typing_text(names);
    const bool visible = !names.empty();
    if (main_window_shows_(room_id))
    {
        update_typing_bar_(text, visible);
    }
    dispatch_to_secondary_windows_(room_id,
                                   [&](RoomWindowBase* w)
                                   {
                                       w->on_typing_changed(text, visible);
                                   });
}

void ShellBase::handle_compose_text_changed_(const std::string& text)
{
    bool typing = !text.empty();
    if (typing == compose_typing_active_)
    {
        return;
    }
    compose_typing_active_ = typing;
    if (!current_room_id_.empty())
    {
        auto sess = active_account_;
        run_async_([sess, room_id = current_room_id_, typing]()
        {
            if (sess && sess->client)
                sess->client->send_typing_notice(room_id, typing);
        });
    }
}

void ShellBase::handle_compose_room_leaving_(const std::string& old_room_id)
{
    if (!compose_typing_active_ || old_room_id.empty())
    {
        return;
    }
    compose_typing_active_ = false;
    auto sess = active_account_;
    run_async_([sess, old_room_id]()
    {
        if (sess && sess->client)
            sess->client->send_typing_notice(old_room_id, false);
    });
}

// ── Concrete apply_thread_*_ virtuals (route into room_view_->thread_view) ─

void ShellBase::apply_thread_messages_(const std::string& /*thread_root*/,
                                       std::vector<views::MessageRowData> rows,
                                       bool room_switch)
{
    if (room_view_ && room_view_->thread_view())
    {
        room_view_->thread_view()->set_messages(std::move(rows), room_switch);
        request_relayout_();
    }
}

void ShellBase::apply_thread_message_insert_(const std::string& /*thread_root*/,
                                             std::size_t index,
                                             views::MessageRowData row)
{
    if (room_view_ && room_view_->thread_view())
    {
        room_view_->thread_view()->insert_message(index, std::move(row));
        request_relayout_();
    }
}

void ShellBase::apply_thread_message_update_(const std::string& /*thread_root*/,
                                             std::size_t index,
                                             views::MessageRowData row)
{
    if (room_view_ && room_view_->thread_view())
    {
        room_view_->thread_view()->update_message(index, std::move(row));
        request_relayout_();
    }
}

void ShellBase::apply_thread_message_remove_(const std::string& /*thread_root*/,
                                             std::size_t index)
{
    if (room_view_ && room_view_->thread_view())
    {
        room_view_->thread_view()->remove_message(index);
        request_relayout_();
    }
}

void ShellBase::apply_threads_list_(std::vector<ThreadInfo> threads)
{
    if (!room_view_)
        return;

    // Drive header-button visibility off the latest snapshot. This runs on
    // every on_threads_updated tick (including the initial empty tick after
    // subscribe), so the button reveals when the SDK paginates non-empty and
    // hides when the list goes empty (e.g., redactions, room switch).
    // Bridged rooms (MSC2346) hide the threads button regardless of thread
    // count, unless the bridge advertises thread support via
    // com.beeper.room_features (otherwise it cannot relay them).
    const auto* cur_room = room_by_id_(current_room_id_);
    const bool cur_room_bridged =
        cur_room && cur_room->is_bridged && !cur_room->bridge_overridden;
    const bool show_threads =
        !threads.empty() &&
        (!cur_room_bridged || cur_room->bridge_capabilities.threads);
    room_view_->set_show_threads_button(show_threads);

    // Unread-thread indicator: fold the per-thread flags into the header dot.
    const auto agg = views::aggregate_threads(threads);
    room_view_->set_threads_unread(agg.any_unread, agg.any_mention);

    // Fan out to any popout window currently showing the same room.
    for (auto& [rid, w] : active_account_popouts_())
    {
        if (rid == current_room_id_ && w->room_view())
        {
            w->room_view()->set_show_threads_button(show_threads);
            w->room_view()->set_threads_unread(agg.any_unread, agg.any_mention);
        }
    }

    if (room_view_->thread_list_view())
    {
        room_view_->thread_list_view()->set_threads(std::move(threads));
        request_relayout_();
    }
}
} // namespace tesseract
