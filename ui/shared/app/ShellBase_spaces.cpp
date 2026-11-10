#include "app/ShellBase.h"
#include "views/MainAppWidget.h"
#include <algorithm>
#include <unordered_map>

namespace tesseract
{

void ShellBase::update_space_children_cache_()
{
    if (!client_)
    {
        space_children_cache_.clear();
        unjoined_space_children_cache_.clear();
        unjoined_summaries_cache_.clear();
        unjoined_fetch_.clear();
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
        unjoined_fetch_.clear();
        pending_summaries_.clear();
        return;
    }
    auto sess = active_account_;
    run_async_(
        [this, sess, space_ids = std::move(space_ids)]()
        {
            if (!sess || !sess->client)
            {
                return;
            }

            std::unordered_map<std::string, std::vector<std::string>> fresh_joined;
            std::unordered_map<std::string, std::vector<std::string>> fresh_unjoined;

            for (const auto& id : space_ids)
            {
                auto all = sess->client->space_children_all(id);
                auto joined = sess->client->space_children(id);

                std::unordered_set<std::string> joined_set(joined.begin(), joined.end());
                std::vector<std::string> unjoined;
                for (const auto& child : all)
                {
                    if (!joined_set.count(child))
                    {
                        unjoined.push_back(child);
                    }
                }
                fresh_joined[id] = std::move(joined);
                fresh_unjoined[id] = std::move(unjoined);
            }

            post_to_ui_alive_(
                [this, sess, fresh_joined = std::move(fresh_joined),
                 fresh_unjoined = std::move(fresh_unjoined)]() mutable
                {
                    // Results from an account that is no longer active would
                    // overwrite the active account's caches. Drop them; the next
                    // refresh for the active account rebuilds these.
                    if (sess != active_account_)
                    {
                        return;
                    }
                    if (fresh_joined != space_children_cache_ ||
                        fresh_unjoined != unjoined_space_children_cache_)
                    {
                        space_children_cache_ = std::move(fresh_joined);
                        unjoined_space_children_cache_ = std::move(fresh_unjoined);

                        // Evict summaries for rooms that are now joined or are no
                        // longer listed as space children (e.g. via-less tombstones).
                        for (auto& [space_id, summaries] : unjoined_summaries_cache_)
                        {
                            const auto child_it = unjoined_space_children_cache_.find(space_id);
                            const std::unordered_set<std::string> child_set =
                                child_it != unjoined_space_children_cache_.end()
                                    ? std::unordered_set<std::string>(child_it->second.begin(),
                                                                      child_it->second.end())
                                    : std::unordered_set<std::string>{};
                            summaries.erase(
                                std::remove_if(summaries.begin(), summaries.end(),
                                               [this, &child_set](const tesseract::RoomSummary& s)
                                               {
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
std::vector<std::string> ShellBase::parent_spaces_for_room_(const std::string& room_id) const
{
    std::vector<std::string> out;
    if (room_id.empty())
    {
        return out;
    }

    // Invert the cache once (child -> the spaces that list it), so each step is
    // a hash probe rather than a scan of every space's children.
    std::unordered_map<std::string, std::vector<std::string>> parents;
    for (const auto& [space_id, children] : space_children_cache_)
    {
        for (const auto& child : children)
        {
            parents[child].push_back(space_id);
        }
    }

    std::unordered_set<std::string> visited;
    std::vector<std::string> frontier{room_id};
    while (!frontier.empty())
    {
        std::vector<std::string> next;
        for (const auto& id : frontier)
        {
            const auto it = parents.find(id);
            if (it == parents.end())
            {
                continue;
            }
            for (const auto& space_id : it->second)
            {
                if (visited.insert(space_id).second)
                {
                    out.push_back(space_id);
                    next.push_back(space_id);
                }
            }
        }
        frontier = std::move(next);
    }
    return out;
}

void ShellBase::fetch_single_room_summary_(const std::string& space_id, const std::string& room_id)
{
    auto sess = active_account_;
    const std::uint64_t gen = unjoined_fetch_[space_id].gen;

    // Phase 1: show SQLite-cached summary while the async network fetch is
    // in-flight. SQLite reads acquire ffi_mu, so they run on the I/O pool.
    run_async_(
        [this, sess, space_id, room_id, gen]()
        {
            if (!sess || !sess->client)
            {
                return;
            }
            if (auto cached = sess->client->get_cached_room_summary(room_id))
            {
                post_to_ui_alive_(
                    [this, space_id, room_id, gen, cached = std::move(*cached)]() mutable
                    {
                        if (!fetch_gen_current_(space_id, gen))
                        {
                            return;
                        }
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
                        {
                            summaries.push_back(cached);
                        }
                        push_unjoined_to_sidebar_(space_id);
                        refresh_space_root_children_(space_id);
                        if (!cached.avatar_url.empty())
                        {
                            ensure_media_thumbnail_(cached.avatar_url, 64, 64, false);
                        }
                    });
            }
        });

    // Phase 2: async network fetch — no thread is pinned during HTTP.
    // Register the pending entry before calling _async so the callback can
    // resolve it even if the tokio task fires before this function returns.
    if (!sess || !sess->client)
    {
        unjoined_fetch_[space_id].pending.erase(room_id);
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
    {
        return;
    }
    auto [space_id, room_id, gen] = std::move(it->second);
    pending_summaries_.erase(it);

    // Always free the in-flight slot so the room can be retried on re-entry.
    if (auto st = unjoined_fetch_.find(space_id); st != unjoined_fetch_.end())
    {
        st->second.pending.erase(room_id);
    }

    if (!fetch_gen_current_(space_id, gen))
    {
        return;
    }

    auto summary = tesseract::RoomSummary::from_json(summary_json);
    auto sess = active_account_;

    if (!summary.ok())
    {
        // Exponential backoff: 5s, 10s, 20s … capped at 5 min.
        auto& rs = unjoined_fetch_retry_[room_id];
        ++rs.attempts;
        using SC = std::chrono::system_clock;
        using sc = std::chrono::steady_clock;
        using s = std::chrono::seconds;
        const auto delay = std::min(s(5) * (1 << std::min(rs.attempts - 1, 6)), s(300));
        rs.next_retry = sc::now() + delay;
        const auto deadline = SC::now() + delay;
        const auto attempts = rs.attempts;
        const auto deadline_s = std::chrono::duration_cast<s>(deadline.time_since_epoch()).count();
        run_async_(
            [sess, room_id, attempts, deadline_s]()
            {
                if (sess && sess->client)
                {
                    sess->client->note_room_summary_backoff_failed(
                        room_id, static_cast<std::uint32_t>(attempts), deadline_s);
                }
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
    {
        cached.push_back(std::move(summary));
    }

    run_async_(
        [sess, room_id]()
        {
            if (sess && sess->client)
            {
                sess->client->note_room_summary_backoff_ok(room_id);
            }
        });

    push_unjoined_to_sidebar_(space_id);
    refresh_space_root_children_(space_id);
}

bool ShellBase::sidebar_shows_space_(const std::string& space_id) const
{
    return !space_stack_.empty() && space_stack_.back() == space_id;
}

// The room list has a single "Available to join" section, owned by whichever
// space the sidebar is drilled into. Summaries also arrive for the space the
// space-root panel is showing (per-space fetches run independently), so a
// result for any other space must stay in the cache only.
void ShellBase::push_unjoined_to_sidebar_(const std::string& space_id)
{
    if (!main_app_ || !sidebar_shows_space_(space_id))
    {
        return;
    }
    if (auto* rl = main_app_->room_list_view())
    {
        rl->set_space_unjoined_rooms(
            std::vector<tesseract::RoomSummary>(unjoined_summaries_cache_[space_id]));
    }
}

void ShellBase::mark_server_info_stale_()
{
    server_info_stale_accounts_.insert(event_account_);
}

void ShellBase::refresh_server_info_after_reconnect_()
{
    if (server_info_stale_accounts_.erase(event_account_) != 0 && event_is_for_active_account_())
    {
        server_info_fetch_started_ = false;
    }
}

void ShellBase::handle_server_info_async_ready_ui_(std::uint64_t /*request_id*/,
                                                   tesseract::ServerInfo info)
{
    // A half-up server right after an upgrade can fail /versions; taking that
    // result would hide calls, jump-to-date, etc. Keep what we already know.
    if (info.spec_versions.empty() && !server_info_.spec_versions.empty())
    {
        return;
    }
    server_info_ = std::move(info);
    push_own_status_to_strip_(); // profile-field support gates the placeholder
    on_server_info_ready_ui_();
    for (auto& [rid, w] : active_account_popouts_())
    {
        if (auto* rv = w->room_view())
        {
            if (auto* h = rv->header())
            {
                h->set_jump_to_date_enabled(server_info_.supports_msc3030);
            }
        }
    }
    if (room_view_ && room_view_->header())
    {
        update_call_btn_visibility_(room_view_->header(), current_room_id_);
    }
    for (auto& [rid, w] : active_account_popouts_())
    {
        if (auto* rv = w->room_view())
        {
            if (auto* h = rv->header())
            {
                update_call_btn_visibility_(h, rid);
            }
        }
    }
    refresh_call_banners_(); // depends on server_info_.supports_calls
    if (server_info_.supports_profile_fields && server_info_.profile_fields_enabled)
    {
        fetch_own_extended_profile_async_();
    }
}

void ShellBase::show_space_root_(const std::string& space_id)
{
    if (!main_app_ || space_id.empty())
    {
        return;
    }

    const RoomInfo* space = room_by_id_(space_id);
    if (!space || !space->is_space)
    {
        return;
    }

    ensure_room_avatar_(*space);

    std::size_t joined_children = 0;
    if (auto it = space_children_cache_.find(space_id); it != space_children_cache_.end())
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

    if (auto it = space_children_cache_.find(space_id); it != space_children_cache_.end())
    {
        // Mirrors refresh_room_list_()'s identical drilled-into-space
        // filter: iterate rooms_ (the same source RoomListView itself is
        // built from) and keep only those the cache says are children —
        // NOT the reverse (iterating child ids and looking each one up),
        // which would let a stale id space_children() still reports (e.g.
        // a room since left/abandoned, no longer in rooms_) leak through
        // as a blank placeholder entry.
        const std::unordered_set<std::string> child_set(it->second.begin(), it->second.end());
        for (const auto& r : rooms_)
        {
            if (!child_set.count(r.id))
            {
                continue;
            }
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
            auto sit = std::find_if(cached_summaries.begin(), cached_summaries.end(),
                                    [&](const tesseract::RoomSummary& s)
                                    {
                                        return s.room_id == id;
                                    });
            if (sit == cached_summaries.end())
            {
                continue;
            }
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
    main_app_->space_root()->set_can_manage_children(client_ &&
                                                     client_->can_edit_space_children(space_id));
}

void ShellBase::request_add_room_to_space_(const std::string& space_id, const std::string& room_id)
{
    if (!client_ || space_id.empty() || room_id.empty())
    {
        return;
    }

    auto& joined = space_children_cache_[space_id];
    if (std::find(joined.begin(), joined.end(), room_id) == joined.end())
    {
        joined.push_back(room_id);
    }
    auto& unjoined = unjoined_space_children_cache_[space_id];
    unjoined.erase(std::remove(unjoined.begin(), unjoined.end(), room_id), unjoined.end());
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
    {
        return;
    }

    auto& joined = space_children_cache_[space_id];
    joined.erase(std::remove(joined.begin(), joined.end(), room_id), joined.end());
    auto& unjoined = unjoined_space_children_cache_[space_id];
    unjoined.erase(std::remove(unjoined.begin(), unjoined.end(), room_id), unjoined.end());
    refresh_space_root_children_(space_id);
    refresh_room_list_();

    const auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id, RoomActionKind::RemoveSpaceChild, space_id};
    client_->remove_room_from_space_async(req_id, space_id, room_id);
}

void ShellBase::cancel_unjoined_summaries_(const std::string& keep_a, const std::string& keep_b)
{
    const auto kept = [&](const std::string& id)
    {
        return !id.empty() && (id == keep_a || id == keep_b);
    };

    for (auto it = unjoined_fetch_.begin(); it != unjoined_fetch_.end();)
    {
        if (kept(it->first))
        {
            ++it;
            continue;
        }
        if (client_)
        {
            client_->cancel_space_summaries(it->first);
        }
        it = unjoined_fetch_.erase(it);
    }
    for (auto it = pending_summaries_.begin(); it != pending_summaries_.end();)
    {
        if (kept(it->second.space_id))
        {
            ++it;
        }
        else
        {
            it = pending_summaries_.erase(it);
        }
    }
}

void ShellBase::reset_unjoined_fetch_state_()
{
    unjoined_fetch_.clear();
    pending_summaries_.clear();
    unjoined_fetch_retry_.clear();
}

bool ShellBase::fetch_gen_current_(const std::string& space_id, std::uint64_t gen) const
{
    const auto it = unjoined_fetch_.find(space_id);
    return it != unjoined_fetch_.end() && gen != 0 && it->second.gen == gen;
}

void ShellBase::request_summary_fetch_(const std::string& space_id, const std::string& room_id)
{
    auto& st = unjoined_fetch_[space_id];
    if (st.gen == 0)
    {
        st.gen = ++unjoined_fetch_gen_counter_;
    }
    if (st.pending.count(room_id))
    {
        return;
    }
    const auto retry_it = unjoined_fetch_retry_.find(room_id);
    if (retry_it != unjoined_fetch_retry_.end() &&
        std::chrono::steady_clock::now() < retry_it->second.next_retry)
    {
        return;
    }
    st.pending.insert(room_id);
    fetch_single_room_summary_(space_id, room_id);
}

const std::vector<tesseract::RoomSummary>&
ShellBase::get_cached_unjoined_summaries_(const std::string& space_id)
{
    auto& fetch = unjoined_fetch_[space_id];
    if (fetch.gen == 0)
    {
        fetch.gen = ++unjoined_fetch_gen_counter_;
    }

    auto& summaries = unjoined_summaries_cache_[space_id];

    // Prune any leftover stubs (name empty) from previous behaviour.
    summaries.erase(std::remove_if(summaries.begin(), summaries.end(),
                                   [](const tesseract::RoomSummary& s)
                                   {
                                       return s.name.empty();
                                   }),
                    summaries.end());

    auto child_it = unjoined_space_children_cache_.find(space_id);
    if (child_it != unjoined_space_children_cache_.end())
    {
        // Build set of already-loaded room IDs so we don't double-fetch.
        std::unordered_set<std::string> loaded;
        loaded.reserve(summaries.size());
        for (const auto& s : summaries)
        {
            loaded.insert(s.room_id);
        }

        // Proactively kick off a fetch for every unloaded child.
        // Rooms that are already in-flight or currently in backoff are skipped
        // (request_summary_fetch_); they will be retried on the next refresh.
        for (const auto& id : child_it->second)
        {
            if (!loaded.count(id))
            {
                request_summary_fetch_(space_id, id);
            }
        }
    }
    return summaries;
}

bool ShellBase::is_space_pinned_(const std::string& space_id) const
{
    if (!active_account_)
    {
        return false;
    }
    const auto& pins = active_account_->pinned_spaces;
    return std::find(pins.begin(), pins.end(), space_id) != pins.end();
}

bool ShellBase::is_space_pin_active_(const std::string& space_id) const
{
    return (!main_app_ || main_app_->space_pins_shown()) && is_space_pinned_(space_id);
}

void ShellBase::pin_space_(const std::string& space_id)
{
    if (!active_account_ || space_id.empty() || is_space_pinned_(space_id))
    {
        return;
    }
    active_account_->pinned_spaces.push_back(space_id);
    schedule_account_data_save_();
    refresh_room_list_(); // also refreshes the picker
}

void ShellBase::unpin_space_(const std::string& space_id)
{
    if (!active_account_ || !is_space_pinned_(space_id))
    {
        return;
    }
    auto& pins = active_account_->pinned_spaces;
    pins.erase(std::remove(pins.begin(), pins.end(), space_id), pins.end());
    schedule_account_data_save_();
    // Its tab is gone: if the sidebar is inside that space, step out to Home.
    if (!space_stack_.empty() && space_stack_.front() == space_id)
    {
        select_picker_home_();
    }
    else
    {
        refresh_room_list_();
    }
}

void ShellBase::reorder_pinned_space_(const std::string& space_id,
                                      const std::string& before_space_id)
{
    if (!active_account_ || space_id == before_space_id)
    {
        return;
    }
    auto& pins = active_account_->pinned_spaces;
    const auto from = std::find(pins.begin(), pins.end(), space_id);
    if (from == pins.end())
    {
        return;
    }
    pins.erase(from);
    auto to = std::find(pins.begin(), pins.end(), before_space_id);
    pins.insert(to, space_id); // not found (or empty) → end
    schedule_account_data_save_();
    refresh_space_picker_();
}

void ShellBase::select_picker_space_(const std::string& space_id)
{
    if (!main_app_ || !room_by_id_(space_id))
    {
        return;
    }
    if (space_stack_.size() == 1 && space_stack_.front() == space_id)
    {
        show_space_root_(space_id); // the header that does this is hidden
        return;
    }

    views::RoomListView* rlv = main_app_->room_list_view();
    space_stack_.clear();
    space_nav_frames_.clear();
    main_app_->hide_room_preview();
    if (const auto* cur = room_by_id_(current_room_id_); cur && cur->is_space)
    {
        show_space_root_(current_room_id_);
    }
    else
    {
        hide_space_root_();
    }

    space_nav_frames_.push_back(SpaceNavFrame::capture(rlv));
    space_stack_.push_back(space_id);
    refresh_room_list_();
    SpaceNavFrame::enter(rlv);
}

void ShellBase::select_picker_home_()
{
    if (!main_app_)
    {
        return;
    }
    SpaceNavFrame root_frame;
    const bool have_frame = !space_nav_frames_.empty();
    if (have_frame)
    {
        root_frame = space_nav_frames_.front();
    }
    space_stack_.clear();
    space_nav_frames_.clear();
    main_app_->hide_room_preview();
    if (const auto* cur = room_by_id_(current_room_id_); cur && cur->is_space)
    {
        show_space_root_(current_room_id_);
    }
    else
    {
        hide_space_root_();
    }
    refresh_room_list_();
    if (have_frame)
    {
        root_frame.restore(main_app_->room_list_view());
    }
}

void ShellBase::refresh_space_picker_()
{
    if (!main_app_)
    {
        return;
    }
    std::vector<views::SpacePickerEntry> entries;
    if (active_account_ && main_app_->space_pins_shown())
    {
        std::vector<RoomInfo> spaces;
        for (const auto& id : active_account_->pinned_spaces)
        {
            if (const auto* r = room_by_id_(id); r && r->is_space)
            {
                spaces.push_back(*r);
            }
        }
        apply_space_child_counts_(spaces); // aggregated child unread
        for (const auto& s : spaces)
        {
            views::SpacePickerEntry e;
            e.space_id = s.id;
            e.name = s.name;
            e.avatar_url = s.avatar_url;
            e.notification_count = s.notification_count;
            e.highlight_count = s.highlight_count;
            e.unread_count = s.unread_count;
            e.muted = s.muted;
            entries.push_back(std::move(e));
        }
    }
    main_app_->set_space_picker_entries(std::move(entries));
    if (auto* picker = main_app_->space_picker())
    {
        picker->set_active_space(space_stack_.empty() ? std::string{} : space_stack_.front());
    }
}

void ShellBase::hide_pinned_spaces_(std::vector<RoomInfo>& rooms) const
{
    if (!active_account_ || active_account_->pinned_spaces.empty())
    {
        return;
    }
    rooms.erase(std::remove_if(rooms.begin(), rooms.end(),
                               [this](const RoomInfo& r)
                               {
                                   return r.is_space && is_space_pin_active_(r.id);
                               }),
                rooms.end());
}

void ShellBase::apply_space_child_counts_(std::vector<RoomInfo>& rooms) const
{
    if (space_children_cache_.empty())
    {
        return;
    }

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
    {
        counts[r.id] = {r.notification_count, r.highlight_count, r.unread_count,
                        r.last_activity_ts};
    }

    for (auto& r : rooms)
    {
        if (!r.is_space)
        {
            continue;
        }
        auto it = space_children_cache_.find(r.id);
        if (it == space_children_cache_.end())
        {
            continue;
        }
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
                {
                    newest_unread_ts = std::max(newest_unread_ts, ci->second.last_activity_ts);
                }
                if (ci->second.unread_count > 0)
                {
                    newest_quiet_ts = std::max(newest_quiet_ts, ci->second.last_activity_ts);
                }
            }
        }
        r.notification_count = nc;
        r.highlight_count = hc;
        r.unread_count = uc;
        if (nc > 0)
        {
            r.last_activity_ts = std::max(r.last_activity_ts, newest_unread_ts);
        }
        else if (uc > 0)
        {
            r.last_activity_ts = std::max(r.last_activity_ts, newest_quiet_ts);
        }
    }
}

// ---------------------------------------------------------------------------
// SpaceNavFrame helpers
// ---------------------------------------------------------------------------

ShellBase::SpaceNavFrame ShellBase::SpaceNavFrame::capture(views::RoomListView* rlv)
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
    {
        return;
    }
    for (int i = 0; i < views::RoomListView::kNumSections; ++i)
    {
        rlv->set_section_collapsed(i, collapsed[i]);
    }
    rlv->scroll_to_offset(scroll_fraction);
}

void ShellBase::SpaceNavFrame::enter(views::RoomListView* rlv)
{
    if (!rlv)
    {
        return;
    }
    for (int i = 0; i < views::RoomListView::kNumSections; ++i)
    {
        rlv->set_section_collapsed(i, false);
    }
    rlv->scroll_to_offset(0.f);
}
} // namespace tesseract
