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

void ShellBase::update_space_children_cache_()
{
    if (!client_)
    {
        space_children_cache_.clear();
        unjoined_space_children_cache_.clear();
        unjoined_summaries_cache_.clear();
        unjoined_fetch_pending_.clear();
        pending_summaries_.clear();
        return;
    }
    std::vector<std::string> space_ids;
    for (const auto& r : rooms_)
    {
        if (r.is_space)
        {
            space_ids.push_back(r.id);
        }
    }
    if (space_ids.empty())
    {
        space_children_cache_.clear();
        unjoined_space_children_cache_.clear();
        unjoined_summaries_cache_.clear();
        unjoined_fetch_pending_.clear();
        pending_summaries_.clear();
        return;
    }
    auto sess = active_account_;
    run_async_(
        [this, sess, space_ids = std::move(space_ids)]()
        {
            if (!sess || !sess->client) return;

            std::unordered_map<std::string, std::vector<std::string>> fresh_joined;
            std::unordered_map<std::string, std::vector<std::string>> fresh_unjoined;

            for (const auto& id : space_ids)
            {
                auto all    = sess->client->space_children_all(id);
                auto joined = sess->client->space_children(id);

                std::unordered_set<std::string> joined_set(
                    joined.begin(), joined.end());
                std::vector<std::string> unjoined;
                for (const auto& child : all)
                {
                    if (!joined_set.count(child))
                        unjoined.push_back(child);
                }
                fresh_joined[id]   = std::move(joined);
                fresh_unjoined[id] = std::move(unjoined);
            }

            post_to_ui_alive_(
                [this,
                 fresh_joined   = std::move(fresh_joined),
                 fresh_unjoined = std::move(fresh_unjoined)]() mutable
                {
                    if (fresh_joined   != space_children_cache_ ||
                        fresh_unjoined != unjoined_space_children_cache_)
                    {
                        space_children_cache_          = std::move(fresh_joined);
                        unjoined_space_children_cache_ = std::move(fresh_unjoined);

                        // Evict summaries for rooms that are now joined or are no
                        // longer listed as space children (e.g. via-less tombstones).
                        for (auto& [space_id, summaries] : unjoined_summaries_cache_)
                        {
                            const auto child_it =
                                unjoined_space_children_cache_.find(space_id);
                            const std::unordered_set<std::string> child_set =
                                child_it != unjoined_space_children_cache_.end()
                                    ? std::unordered_set<std::string>(
                                          child_it->second.begin(),
                                          child_it->second.end())
                                    : std::unordered_set<std::string>{};
                            summaries.erase(
                                std::remove_if(
                                    summaries.begin(), summaries.end(),
                                    [this, &child_set](const tesseract::RoomSummary& s) {
                                        return room_by_id_(s.room_id) != nullptr ||
                                               !child_set.count(s.room_id);
                                    }),
                                summaries.end());
                        }

                        on_space_children_cache_ready_ui_();
                        refresh_space_root_children_(space_root_shown_id_);
                    }
                });
        });
}

// Every space (direct and ancestor) containing `room_id`, derived by
// repeatedly inverting space_children_cache_ (space_id -> joined
// children): find room_id's direct parent space(s), then treat each as
// a "room" and find *their* parent space(s), and so on, so a Space
// nested inside another Space is included too. A visited-set guards
// against a cyclical (misconfigured) space hierarchy. Empty if room_id
// is empty or in no cached space's children. Synchronous, no I/O — safe
// to call from the UI thread.
std::vector<std::string>
ShellBase::parent_spaces_for_room_(const std::string& room_id) const
{
    std::vector<std::string> out;
    if (room_id.empty())
    {
        return out;
    }

    std::unordered_set<std::string> visited;
    std::vector<std::string> frontier{room_id};
    while (!frontier.empty())
    {
        std::vector<std::string> next;
        for (const auto& [space_id, children] : space_children_cache_)
        {
            if (visited.count(space_id))
            {
                continue;
            }
            const bool contains_frontier_member =
                std::any_of(frontier.begin(), frontier.end(), [&](const std::string& id) {
                    return std::find(children.begin(), children.end(), id) != children.end();
                });
            if (!contains_frontier_member)
            {
                continue;
            }
            visited.insert(space_id);
            out.push_back(space_id);
            next.push_back(space_id);
        }
        frontier = std::move(next);
    }
    return out;
}

void ShellBase::fetch_single_room_summary_(const std::string& space_id,
                                           const std::string& room_id)
{
    auto sess = active_account_;
    const std::uint64_t gen = unjoined_fetch_gen_;

    // Phase 1: show SQLite-cached summary while the async network fetch is
    // in-flight. SQLite reads acquire ffi_mu, so they run on the I/O pool.
    run_async_(
        [this, sess, space_id, room_id, gen]()
        {
            if (!sess || !sess->client) return;
            if (auto cached = sess->client->get_cached_room_summary(room_id))
            {
                post_to_ui_alive_(
                    [this, space_id, room_id, gen,
                     cached = std::move(*cached)]() mutable
                    {
                        if (gen != unjoined_fetch_gen_) return;
                        auto& summaries = unjoined_summaries_cache_[space_id];
                        bool found = false;
                        for (auto& entry : summaries)
                        {
                            if (entry.room_id == room_id)
                            {
                                entry = cached;
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                            summaries.push_back(cached);
                        if (main_app_)
                            if (auto* rl = main_app_->room_list_view())
                                rl->set_space_unjoined_rooms(
                                    std::vector<tesseract::RoomSummary>(summaries));
                        refresh_space_root_children_(space_id);
                        if (!cached.avatar_url.empty())
                            ensure_media_thumbnail_(cached.avatar_url, 64, 64, false);
                    });
            }
        });

    // Phase 2: async network fetch — no thread is pinned during HTTP.
    // Register the pending entry before calling _async so the callback can
    // resolve it even if the tokio task fires before this function returns.
    if (!sess || !sess->client)
    {
        unjoined_fetch_pending_.erase(room_id);
        return;
    }
    const auto req_id = next_request_id_++;
    pending_summaries_[req_id] = {space_id, room_id, gen};
    sess->client->get_space_child_summary_async(req_id, space_id, room_id);
}

void ShellBase::handle_space_child_summary_ready_ui_(std::uint64_t request_id,
                                                      std::string summary_json)
{
    auto it = pending_summaries_.find(request_id);
    if (it == pending_summaries_.end())
        return;
    auto [space_id, room_id, gen] = std::move(it->second);
    pending_summaries_.erase(it);

    // Always free the in-flight slot so the room can be retried on re-entry.
    unjoined_fetch_pending_.erase(room_id);

    if (gen != unjoined_fetch_gen_) return;

    auto summary = tesseract::RoomSummary::from_json(summary_json);
    auto sess    = active_account_;

    if (!summary.ok())
    {
        // Exponential backoff: 5s, 10s, 20s … capped at 5 min.
        auto& rs = unjoined_fetch_retry_[room_id];
        ++rs.attempts;
        using SC = std::chrono::system_clock;
        using sc = std::chrono::steady_clock;
        using s  = std::chrono::seconds;
        const auto delay =
            std::min(s(5) * (1 << std::min(rs.attempts - 1, 6)), s(300));
        rs.next_retry       = sc::now() + delay;
        const auto deadline = SC::now() + delay;
        const auto attempts = rs.attempts;
        const auto deadline_s =
            std::chrono::duration_cast<s>(deadline.time_since_epoch()).count();
        run_async_([sess, room_id, attempts, deadline_s]()
        {
            if (sess && sess->client)
                sess->client->note_room_summary_backoff_failed(
                    room_id, static_cast<std::uint32_t>(attempts), deadline_s);
        });
        return;
    }

    auto& cached = unjoined_summaries_cache_[space_id];
    bool found = false;
    for (auto& entry : cached)
    {
        if (entry.room_id == room_id)
        {
            entry = std::move(summary);
            found = true;
            break;
        }
    }
    if (!found)
        cached.push_back(std::move(summary));

    run_async_([sess, room_id]()
    {
        if (sess && sess->client)
            sess->client->note_room_summary_backoff_ok(room_id);
    });

    if (main_app_)
        if (auto* rl = main_app_->room_list_view())
            rl->set_space_unjoined_rooms(
                std::vector<tesseract::RoomSummary>(cached));
    refresh_space_root_children_(space_id);
}

void ShellBase::handle_server_info_async_ready_ui_(std::uint64_t /*request_id*/,
                                                    std::string info_json)
{
    server_info_ = tesseract::ServerInfo::from_json(info_json);
    push_own_status_to_strip_(); // profile-field support gates the placeholder
    on_server_info_ready_ui_();
    for (auto& [rid, w] : active_account_popouts_())
    {
        if (auto* rv = w->room_view())
            if (auto* h = rv->header())
                h->set_jump_to_date_enabled(server_info_.supports_msc3030);
    }
    if (room_view_ && room_view_->header())
        update_call_btn_visibility_(room_view_->header(), current_room_id_);
    for (auto& [rid, w] : active_account_popouts_())
        if (auto* rv = w->room_view())
            if (auto* h = rv->header())
                update_call_btn_visibility_(h, rid);
    refresh_call_banners_(); // depends on server_info_.supports_calls
    if (server_info_.supports_profile_fields &&
        server_info_.profile_fields_enabled)
        fetch_own_extended_profile_async_();
}

void ShellBase::show_space_root_(const std::string& space_id)
{
    if (!main_app_ || space_id.empty())
        return;

    const RoomInfo* space = room_by_id_(space_id);
    if (!space || !space->is_space)
        return;

    ensure_room_avatar_(*space);

    std::size_t joined_children = 0;
    if (auto it = space_children_cache_.find(space_id);
        it != space_children_cache_.end())
    {
        joined_children = it->second.size();
    }

    std::size_t unjoined_children = 0;
    if (auto it = unjoined_space_children_cache_.find(space_id);
        it != unjoined_space_children_cache_.end())
    {
        unjoined_children = it->second.size();
    }

    main_app_->show_space_root(*space, joined_children, unjoined_children,
                               make_avatar_image_provider_());
    space_root_shown_id_ = space_id;
    refresh_space_root_children_(space_id);
    request_relayout_();
}

// Rebuilds the room-management section's data (SpaceAddRoomList's
// exclusion set + SpaceChildRoomGrid's children) from the current
// space_children_cache_/unjoined_space_children_cache_/rooms_ and pushes
// it into main_app_->space_root(). No-op if that space isn't the one
// currently shown. Called after show_space_root_() itself, after
// space_children_cache_/unjoined summaries refresh, after rooms_
// changes, and — optimistically — immediately by
// request_add/remove_room_to/from_space_ before their async result
// even returns.
void ShellBase::refresh_space_root_children_(const std::string& space_id)
{
    if (!main_app_ || !main_app_->space_root() || space_id.empty() ||
        space_id != space_root_shown_id_)
    {
        return;
    }

    std::vector<views::SpaceChildRoomGrid::ChildRoomEntry> entries;

    if (auto it = space_children_cache_.find(space_id);
        it != space_children_cache_.end())
    {
        // Mirrors refresh_room_list_()'s identical drilled-into-space
        // filter: iterate rooms_ (the same source RoomListView itself is
        // built from) and keep only those the cache says are children —
        // NOT the reverse (iterating child ids and looking each one up),
        // which would let a stale id space_children() still reports (e.g.
        // a room since left/abandoned, no longer in rooms_) leak through
        // as a blank placeholder entry.
        const auto& child_ids = it->second;
        for (const auto& r : rooms_)
        {
            if (std::find(child_ids.begin(), child_ids.end(), r.id) ==
                child_ids.end())
                continue;
            views::SpaceChildRoomGrid::ChildRoomEntry entry;
            entry.joined = true;
            entry.info = r;
            entries.push_back(std::move(entry));
        }
    }

    if (auto it = unjoined_space_children_cache_.find(space_id);
        it != unjoined_space_children_cache_.end())
    {
        // Also proactively kicks off a fetch for every summary not yet
        // cached — the same mechanism RoomListView's own "Available to
        // join" section already relies on (see its doc comment). Mirroring
        // that section exactly: only ids with an already-resolved summary
        // are shown — RoomListView's own set_space_unjoined_rooms() is fed
        // solely from unjoined_summaries_cache_, never a placeholder for a
        // still-pending or perpetually-failing (e.g. dead/unreachable)
        // child — so a child id with no summary yet simply doesn't appear
        // here either, rather than as a permanent blank/"Loading…" row.
        const auto& cached_summaries = get_cached_unjoined_summaries_(space_id);
        for (const auto& id : it->second)
        {
            auto sit = std::find_if(
                cached_summaries.begin(), cached_summaries.end(),
                [&](const tesseract::RoomSummary& s) { return s.room_id == id; });
            if (sit == cached_summaries.end())
                continue;
            views::SpaceChildRoomGrid::ChildRoomEntry entry;
            entry.joined = false;
            entry.info.id = id;
            entry.info.name = sit->name;
            entry.info.topic = sit->topic;
            entry.info.avatar_url = sit->avatar_url;
            entries.push_back(std::move(entry));
        }
    }

    main_app_->space_root()->set_children(std::move(entries));
    main_app_->space_root()->set_can_manage_children(
        client_ && client_->can_edit_space_children(space_id));
}

void ShellBase::request_add_room_to_space_(const std::string& space_id,
                                           const std::string& room_id)
{
    if (!client_ || space_id.empty() || room_id.empty())
        return;

    auto& joined = space_children_cache_[space_id];
    if (std::find(joined.begin(), joined.end(), room_id) == joined.end())
        joined.push_back(room_id);
    auto& unjoined = unjoined_space_children_cache_[space_id];
    unjoined.erase(std::remove(unjoined.begin(), unjoined.end(), room_id),
                  unjoined.end());
    refresh_space_root_children_(space_id);
    // The main sidebar's root-level grouping (filter_root_rooms) and any
    // drilled-into-space view both key off space_children_cache_ too — not
    // just the space-root section above.
    refresh_room_list_();

    const auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id, RoomActionKind::AddSpaceChild, space_id};
    client_->add_room_to_space_async(req_id, space_id, room_id, {});
}

void ShellBase::request_remove_room_from_space_(const std::string& space_id,
                                                const std::string& room_id)
{
    if (!client_ || space_id.empty() || room_id.empty())
        return;

    auto& joined = space_children_cache_[space_id];
    joined.erase(std::remove(joined.begin(), joined.end(), room_id), joined.end());
    auto& unjoined = unjoined_space_children_cache_[space_id];
    unjoined.erase(std::remove(unjoined.begin(), unjoined.end(), room_id),
                  unjoined.end());
    refresh_space_root_children_(space_id);
    refresh_room_list_();

    const auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id, RoomActionKind::RemoveSpaceChild, space_id};
    client_->remove_room_from_space_async(req_id, space_id, room_id);
}

void ShellBase::cancel_unjoined_summaries_()
{
    if (!active_space_id_.empty() && client_)
        client_->cancel_space_summaries(active_space_id_);
    ++unjoined_fetch_gen_;
    unjoined_fetch_pending_.clear();
    pending_summaries_.clear();
    unjoined_fetch_retry_.clear();
    active_space_id_.clear();
}

const std::vector<tesseract::RoomSummary>&
ShellBase::get_cached_unjoined_summaries_(const std::string& space_id)
{
    if (active_space_id_ != space_id)
    {
        if (!active_space_id_.empty() && client_)
            client_->cancel_space_summaries(active_space_id_);
        active_space_id_ = space_id;
        ++unjoined_fetch_gen_;
        unjoined_fetch_pending_.clear();
        pending_summaries_.clear();
        unjoined_fetch_retry_.clear();
    }

    auto& summaries = unjoined_summaries_cache_[space_id];

    // Prune any leftover stubs (name empty) from previous behaviour.
    summaries.erase(
        std::remove_if(summaries.begin(), summaries.end(),
            [](const tesseract::RoomSummary& s) { return s.name.empty(); }),
        summaries.end());

    auto child_it = unjoined_space_children_cache_.find(space_id);
    if (child_it != unjoined_space_children_cache_.end())
    {
        // Build set of already-loaded room IDs so we don't double-fetch.
        std::unordered_set<std::string> loaded;
        loaded.reserve(summaries.size());
        for (const auto& s : summaries)
            loaded.insert(s.room_id);

        // Proactively kick off a fetch for every unloaded child.
        // Rooms that are already in-flight or currently in backoff are skipped;
        // they will be retried on the next space entry.
        for (const auto& id : child_it->second)
        {
            if (loaded.count(id) || unjoined_fetch_pending_.count(id))
                continue;
            auto retry_it = unjoined_fetch_retry_.find(id);
            if (retry_it != unjoined_fetch_retry_.end() &&
                std::chrono::steady_clock::now() < retry_it->second.next_retry)
                continue;
            unjoined_fetch_pending_.insert(id);
            fetch_single_room_summary_(space_id, id);
        }
    }
    return summaries;
}

void ShellBase::apply_space_child_counts_(std::vector<RoomInfo>& rooms) const
{
    if (space_children_cache_.empty())
        return;

    struct ChildCounts
    {
        uint64_t notification_count;
        uint64_t highlight_count;
        uint64_t unread_count;
        uint64_t last_activity_ts;
    };
    std::unordered_map<std::string, ChildCounts> counts;
    counts.reserve(rooms_.size());
    for (const auto& r : rooms_)
        counts[r.id] = {r.notification_count, r.highlight_count,
                        r.unread_count, r.last_activity_ts};

    for (auto& r : rooms)
    {
        if (!r.is_space)
            continue;
        auto it = space_children_cache_.find(r.id);
        if (it == space_children_cache_.end())
            continue;
        uint64_t nc = 0, hc = 0, uc = 0;
        uint64_t newest_unread_ts = 0, newest_quiet_ts = 0;
        for (const auto& child_id : it->second)
        {
            auto ci = counts.find(child_id);
            if (ci != counts.end())
            {
                nc += ci->second.notification_count;
                hc += ci->second.highlight_count;
                uc += ci->second.unread_count;
                // Track the most recent activity among *unread* children so the
                // room list can treat the space as a recency-ranked unread
                // candidate (its own last_activity_ts is not meaningful).
                if (ci->second.notification_count > 0)
                    newest_unread_ts =
                        std::max(newest_unread_ts, ci->second.last_activity_ts);
                if (ci->second.unread_count > 0)
                    newest_quiet_ts =
                        std::max(newest_quiet_ts, ci->second.last_activity_ts);
            }
        }
        r.notification_count = nc;
        r.highlight_count    = hc;
        r.unread_count       = uc;
        if (nc > 0)
            r.last_activity_ts = std::max(r.last_activity_ts, newest_unread_ts);
        else if (uc > 0)
            r.last_activity_ts = std::max(r.last_activity_ts, newest_quiet_ts);
    }
}

// ---------------------------------------------------------------------------
// SpaceNavFrame helpers
// ---------------------------------------------------------------------------

ShellBase::SpaceNavFrame
ShellBase::SpaceNavFrame::capture(views::RoomListView* rlv)
{
    SpaceNavFrame f;
    if (rlv)
    {
        f.collapsed = rlv->collapsed_state();
        f.scroll_fraction = rlv->scroll_fraction();
    }
    return f;
}


void ShellBase::SpaceNavFrame::restore(views::RoomListView* rlv) const
{
    if (!rlv)
        return;
    for (int i = 0; i < views::RoomListView::kNumSections; ++i)
        rlv->set_section_collapsed(i, collapsed[i]);
    rlv->scroll_to_offset(scroll_fraction);
}

void ShellBase::SpaceNavFrame::enter(views::RoomListView* rlv)
{
    if (!rlv)
        return;
    for (int i = 0; i < views::RoomListView::kNumSections; ++i)
        rlv->set_section_collapsed(i, false);
    rlv->scroll_to_offset(0.f);
}
} // namespace tesseract
