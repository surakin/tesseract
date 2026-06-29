#include "app/EventHandlerBase.h"
#include "app/ShellBase.h"
#include <tesseract/session_store.h>

namespace tesseract
{

void EventHandlerBase::on_timeline_reset(
    const std::string& room_id, EventList snapshot)
{
    // Wrap in shared_ptr so the move-only lambda is copy-constructible
    // (std::function requires a copyable target).
    struct Payload
    {
        std::string rid;
        EventList snap;
    };
    auto p = std::make_shared<Payload>(Payload{room_id, std::move(snapshot)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_timeline_reset_ui_(std::move(p->rid),
                                             std::move(p->snap));
        });
}

void EventHandlerBase::on_message_inserted(const std::string& room_id,
                                           std::size_t index,
                                           std::unique_ptr<Event> event)
{
    struct Payload
    {
        std::string rid;
        std::size_t idx;
        std::unique_ptr<Event> ev;
    };
    auto p =
        std::make_shared<Payload>(Payload{room_id, index, std::move(event)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_message_inserted_ui_(std::move(p->rid), p->idx,
                                               std::move(p->ev));
        });
}

void EventHandlerBase::on_message_updated(const std::string& room_id,
                                          std::size_t index,
                                          std::unique_ptr<Event> event)
{
    struct Payload
    {
        std::string rid;
        std::size_t idx;
        std::unique_ptr<Event> ev;
    };
    auto p =
        std::make_shared<Payload>(Payload{room_id, index, std::move(event)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_message_updated_ui_(std::move(p->rid), p->idx,
                                              std::move(p->ev));
        });
}

void EventHandlerBase::on_message_removed(const std::string& room_id,
                                          std::size_t index)
{
    shell()->post_to_ui_(
        [shell = shell(), rid = room_id, idx = index]() mutable
        {
            shell->handle_message_removed_ui_(std::move(rid), idx);
        });
}

void EventHandlerBase::on_thread_reset(
    const std::string& room_id, const std::string& thread_root,
    EventList snapshot)
{
    struct Payload
    {
        std::string rid;
        std::string root;
        EventList snap;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, thread_root, std::move(snapshot)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_thread_reset_ui_(std::move(p->rid),
                                           std::move(p->root),
                                           std::move(p->snap));
        });
}

void EventHandlerBase::on_thread_inserted(
    const std::string& room_id, const std::string& thread_root,
    std::size_t index, std::unique_ptr<Event> event)
{
    struct Payload
    {
        std::string rid;
        std::string root;
        std::size_t idx;
        std::unique_ptr<Event> ev;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, thread_root, index, std::move(event)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_thread_inserted_ui_(
                std::move(p->rid), std::move(p->root), p->idx,
                std::move(p->ev));
        });
}

void EventHandlerBase::on_thread_updated(
    const std::string& room_id, const std::string& thread_root,
    std::size_t index, std::unique_ptr<Event> event)
{
    struct Payload
    {
        std::string rid;
        std::string root;
        std::size_t idx;
        std::unique_ptr<Event> ev;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, thread_root, index, std::move(event)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_thread_updated_ui_(
                std::move(p->rid), std::move(p->root), p->idx,
                std::move(p->ev));
        });
}

void EventHandlerBase::on_thread_removed(
    const std::string& room_id, const std::string& thread_root,
    std::size_t index)
{
    shell()->post_to_ui_(
        [shell = shell(), rid = room_id, root = thread_root,
         idx = index]() mutable
        {
            shell->handle_thread_removed_ui_(std::move(rid), std::move(root),
                                             idx);
        });
}

void EventHandlerBase::on_messages_prepended(const std::string& room_id,
                                             EventList events)
{
    struct Payload
    {
        std::string rid;
        EventList evs;
    };
    auto p = std::make_shared<Payload>(Payload{room_id, std::move(events)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_messages_prepended_ui_(std::move(p->rid),
                                                 std::move(p->evs));
        });
}

void EventHandlerBase::on_messages_appended(const std::string& room_id,
                                            EventList events)
{
    struct Payload
    {
        std::string rid;
        EventList evs;
    };
    auto p = std::make_shared<Payload>(Payload{room_id, std::move(events)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_messages_appended_ui_(std::move(p->rid),
                                                std::move(p->evs));
        });
}

void EventHandlerBase::on_messages_updated_batch(const std::string& room_id,
                                                 std::vector<std::size_t> indices,
                                                 EventList events)
{
    struct Payload
    {
        std::string rid;
        std::vector<std::size_t> idxs;
        EventList evs;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, std::move(indices), std::move(events)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_messages_updated_batch_ui_(std::move(p->rid),
                                                     std::move(p->idxs),
                                                     std::move(p->evs));
        });
}

void EventHandlerBase::on_thread_messages_prepended(
    const std::string& room_id, const std::string& thread_root,
    EventList events)
{
    struct Payload
    {
        std::string rid;
        std::string root;
        EventList evs;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, thread_root, std::move(events)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_thread_messages_prepended_ui_(std::move(p->rid),
                                                        std::move(p->root),
                                                        std::move(p->evs));
        });
}

void EventHandlerBase::on_thread_messages_appended(
    const std::string& room_id, const std::string& thread_root,
    EventList events)
{
    struct Payload
    {
        std::string rid;
        std::string root;
        EventList evs;
    };
    auto p = std::make_shared<Payload>(
        Payload{room_id, thread_root, std::move(events)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_thread_messages_appended_ui_(std::move(p->rid),
                                                       std::move(p->root),
                                                       std::move(p->evs));
        });
}

void EventHandlerBase::on_threads_updated(const std::string& room_id)
{
    shell()->post_to_ui_(
        [shell = shell(), rid = room_id]() mutable
        {
            shell->handle_threads_updated_ui_(std::move(rid));
        });
}

void EventHandlerBase::on_media_ready(std::uint64_t request_id,
                                      const std::vector<std::uint8_t>& bytes)
{
    auto b = std::make_shared<std::vector<std::uint8_t>>(bytes);
    shell()->post_to_ui_(
        [shell = shell(), request_id, b]() mutable
        {
            shell->handle_media_ready_ui_(request_id, std::move(*b));
        });
}

void EventHandlerBase::on_url_preview_ready(std::uint64_t request_id,
                                            const std::string& preview_json)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, json = preview_json]() mutable
        {
            shell->handle_url_preview_ready_ui_(request_id, std::move(json));
        });
}

void EventHandlerBase::on_gif_results(std::uint64_t request_id,
                                      const std::vector<GifResult>& results)
{
    auto r = std::make_shared<std::vector<GifResult>>(results);
    shell()->post_to_ui_(
        [shell = shell(), request_id, r]() mutable
        {
            // Fan out to every open pop-out first (copy), then the main window's
            // own controller (move). request_ids are process-global, so only the
            // composer that issued the search matches.
            shell->dispatch_gif_to_secondary_windows_(request_id, *r);
            shell->handle_gif_results_ui_(request_id, std::move(*r));
        });
}

void EventHandlerBase::on_gif_search_failed(std::uint64_t request_id,
                                            const std::string& message)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, msg = message]() mutable
        {
            shell->dispatch_gif_failed_to_secondary_windows_(request_id, msg);
            shell->handle_gif_search_failed_ui_(request_id, std::move(msg));
        });
}

void EventHandlerBase::on_forward_done(std::uint64_t request_id)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id]()
        {
            shell->handle_forward_done_ui_(request_id);
        });
}

void EventHandlerBase::on_forward_failed(std::uint64_t     request_id,
                                         const std::string& message)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, msg = message]() mutable
        {
            shell->handle_forward_failed_ui_(request_id, std::move(msg));
        });
}

void EventHandlerBase::on_space_child_summary_ready(std::uint64_t request_id,
                                                    const std::string& summary_json)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, json = summary_json]() mutable
        {
            shell->handle_space_child_summary_ready_ui_(request_id, std::move(json));
        });
}

void EventHandlerBase::on_server_info_ready(std::uint64_t request_id,
                                            const std::string& info_json)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, json = info_json]() mutable
        {
            shell->handle_server_info_async_ready_ui_(request_id, std::move(json));
        });
}

void EventHandlerBase::on_search_results(std::uint64_t request_id,
                                         const std::vector<SearchHit>& results)
{
    auto r = std::make_shared<std::vector<SearchHit>>(results);
    shell()->post_to_ui_(
        [shell = shell(), request_id, r]() mutable
        {
            // Route by which pending map owns this id.
            if (shell->in_room_search_pending_.count(request_id))
            {
                shell->handle_in_room_search_results_ui_(request_id, std::move(*r));
            }
            else
            {
                shell->handle_search_results_ui_(request_id, std::move(*r));
            }
        });
}

void EventHandlerBase::on_search_failed(std::uint64_t request_id,
                                        const std::string& message)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, msg = message]() mutable
        {
            if (shell->in_room_search_pending_.count(request_id))
            {
                shell->handle_in_room_search_failed_ui_(request_id, std::move(msg));
            }
            else
            {
                shell->handle_search_failed_ui_(request_id, std::move(msg));
            }
        });
}

void EventHandlerBase::on_paginate_result(std::uint64_t request_id, bool ok,
                                          bool reached_start, bool reached_end,
                                          const std::string& message)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, ok, reached_start, reached_end,
         msg = message]() mutable
        {
            shell->handle_paginate_result_ui_(request_id, ok, reached_start,
                                             reached_end, std::move(msg));
        });
}

void EventHandlerBase::on_rooms_updated(const std::vector<RoomInfo>& rooms)
{
    auto rs = rooms; // one copy; moved into the lambda below
    shell()->post_to_ui_(
        [shell = shell(), uid = user_id_, rs = std::move(rs)]() mutable
        {
            shell->push_rooms_(std::move(uid), std::move(rs));
        });
}

void EventHandlerBase::on_invites_updated(const std::vector<InviteInfo>& invites)
{
    auto inv = invites;  // one copy; moved into the lambda below
    shell()->post_to_ui_(
        [shell = shell(), uid = user_id_, inv = std::move(inv)]() mutable
        {
            shell->push_invites_(std::move(uid), std::move(inv));
        });
}

void EventHandlerBase::on_sync_error(const std::string& context,
                                     const std::string& description,
                                     bool soft_logout)
{
    shell()->post_to_ui_(
        [shell = shell(), uid = user_id_, ctx = context, desc = description,
         sl = soft_logout]() mutable
        {
            if (ctx == "sync_offline" || ctx == "sync_error")
            {
                shell->handle_offline_ui_();
            }
            shell->handle_sync_error_ui_(std::move(ctx), std::move(uid),
                                         std::move(desc), sl);
        });
}

void EventHandlerBase::on_session_saved(const std::string& session_json)
{
    if (!user_id_.empty())
    {
        SessionStore::save_account(user_id_, session_json);
    }
}

void EventHandlerBase::on_backup_progress(const BackupProgress& progress)
{
    shell()->post_to_ui_(
        [shell = shell(), p = progress]()
        {
            shell->handle_backup_progress_ui_(p);
        });
}

void EventHandlerBase::on_enable_recovery_progress(uint8_t step,
                                                    const std::string& recovery_key,
                                                    uint32_t backed_up,
                                                    uint32_t total)
{
    auto* s = shell();
    std::string key = recovery_key;
    s->post_to_ui_([s, step, key, backed_up, total]()
    {
        s->handle_enable_recovery_progress_ui_(step, key, backed_up, total);
    });
}

void EventHandlerBase::on_crypto_reset_result(bool ok, const std::string& message)
{
    auto* s = shell();
    std::string msg = message;
    s->post_to_ui_([s, ok, msg]()
    {
        s->handle_crypto_reset_result_ui_(ok, msg);
    });
}

void EventHandlerBase::on_room_list_state(RoomListState state)
{
    shell()->post_to_ui_(
        [shell = shell(), s = state]()
        {
            shell->push_room_list_state_(s);
            shell->on_room_list_state_ui_();
            shell->on_inflight_ui_();
            if (s == RoomListState::Running)
            {
                shell->begin_server_info_fetch_();
                if (shell->offline_)
                {
                    shell->handle_online_ui_();
                }
            }
        });
}

void EventHandlerBase::on_inflight_changed(uint32_t count)
{
    shell()->post_to_ui_(
        [shell = shell(), n = count]()
        {
            // Use the count that Rust passed at callback time.  A live re-read
            // via in_flight_count() (SH_FFI) would block the UI thread whenever
            // a MUT_FFI caller is queued, causing visible freezes.  Out-of-order
            // delivery is possible in theory but self-corrects on the next
            // callback; a briefly stale spinner is far preferable to a freeze.
            shell->last_inflight_ = n;
            if (shell->inflight_needs_anim_())
                shell->start_inflight_tick_();
            else
                shell->stop_inflight_tick_();
            shell->on_inflight_ui_();
        });
}

#ifndef NDEBUG
void EventHandlerBase::on_inflight_changed_debug(uint32_t count, std::string urls)
{
    shell()->post_to_ui_(
        [shell = shell(), n = count, u = std::move(urls)]() mutable
        {
            shell->last_inflight_ = n;
            shell->last_inflight_urls_ = std::move(u);
            if (shell->inflight_needs_anim_())
                shell->start_inflight_tick_();
            else
                shell->stop_inflight_tick_();
            shell->on_inflight_ui_();
        });
}
#endif

void EventHandlerBase::on_image_packs_updated()
{
    shell()->post_to_ui_(
        [shell = shell()]()
        {
            shell->handle_image_packs_updated_ui_();
        });
}

void EventHandlerBase::on_account_prefs_updated(const std::string& json)
{
    shell()->post_to_ui_(
        [shell = shell(), uid = user_id_, j = json]() mutable
        {
            shell->handle_account_prefs_updated_ui_(std::move(uid),
                                                    std::move(j));
        });
}

void EventHandlerBase::on_media_preview_config_updated(const std::string& json)
{
    shell()->post_to_ui_(
        [shell = shell(), uid = user_id_, j = json]() mutable
        {
            shell->handle_media_preview_config_updated_ui_(std::move(uid),
                                                           std::move(j));
        });
}

void EventHandlerBase::on_media_preview_config_ready(std::uint64_t request_id,
                                                     const std::string& config_json)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, j = config_json]() mutable
        {
            shell->handle_media_preview_config_fetched_ui_(request_id, std::move(j));
        });
}

void EventHandlerBase::on_room_preview_override_ready(std::uint64_t request_id,
                                                      const std::string& override_json)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, j = override_json]() mutable
        {
            shell->handle_room_preview_override_ready_ui_(request_id, std::move(j));
        });
}

void EventHandlerBase::on_notification(const std::string& room_id,
                                       const std::string& room_name,
                                       const std::string& sender,
                                       const std::string& body, bool is_mention,
                                       const std::vector<uint8_t>& avatar_bytes,
                                       const std::vector<uint8_t>& image_bytes)
{
    shell()->post_to_ui_(
        [shell = shell(), uid = user_id_, rid = room_id, rn = room_name,
         s = sender, b = body, im = is_mention, av = avatar_bytes,
         img = image_bytes]() mutable
        {
            shell->handle_notification_ui_(
                std::move(uid), std::move(rid), std::move(rn), std::move(s),
                std::move(b), im, std::move(av), std::move(img));
        });
}

void EventHandlerBase::on_verification_request(const std::string& flow_id,
                                               const std::string& user_id,
                                               const std::string& device_id,
                                               bool incoming)
{
    shell()->post_to_ui_(
        [shell = shell(), fid = flow_id, uid = user_id, did = device_id,
         inc = incoming]() mutable
        {
            shell->handle_verification_request_ui_(
                std::move(fid), std::move(uid), std::move(did), inc);
        });
}

void EventHandlerBase::on_sas_ready(const std::string& flow_id,
                                    std::vector<VerificationEmoji> emojis)
{
    struct Payload
    {
        std::string fid;
        std::vector<VerificationEmoji> em;
    };
    auto p = std::make_shared<Payload>(Payload{flow_id, std::move(emojis)});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_sas_ready_ui_(std::move(p->fid), std::move(p->em));
        });
}

void EventHandlerBase::on_verification_done(const std::string& flow_id)
{
    shell()->post_to_ui_(
        [shell = shell(), fid = flow_id]() mutable
        {
            shell->handle_verification_done_ui_(std::move(fid));
        });
}

void EventHandlerBase::on_verification_cancelled(const std::string& flow_id,
                                                 const std::string& reason)
{
    shell()->post_to_ui_(
        [shell = shell(), fid = flow_id, r = reason]() mutable
        {
            shell->handle_verification_cancelled_ui_(std::move(fid),
                                                     std::move(r));
        });
}

void EventHandlerBase::on_verification_state_changed(bool is_verified)
{
    shell()->post_to_ui_(
        [shell = shell(), uid = user_id_, v = is_verified]()
        {
            // Always persist the state so switch_active_account can restore it.
            for (const auto& a : shell->account_manager_.accounts())
            {
                if (a->user_id == uid)
                {
                    a->unverified = !v;
                    break;
                }
            }
            // Only touch the live UI when this is the active account.
            if (shell->active_account_ && shell->active_account_->user_id == uid)
            {
                shell->handle_verification_state_ui_(v);
            }
        });
}

void EventHandlerBase::on_typing_changed(const std::string& room_id,
                                         const std::vector<std::string>& names)
{
    struct Payload
    {
        std::string rid;
        std::vector<std::string> ns;
    };
    auto p = std::make_shared<Payload>(Payload{room_id, names});
    shell()->post_to_ui_(
        [shell = shell(), p]() mutable
        {
            shell->handle_typing_changed_ui_(std::move(p->rid),
                                             std::move(p->ns));
        });
}

void EventHandlerBase::on_upload_complete(std::uint64_t request_id, bool ok,
                                           const std::string& message)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, ok, msg = message]() mutable
        {
            shell->handle_upload_complete_ui_(request_id, ok, std::move(msg));
        });
}

void EventHandlerBase::on_profile_field_result(std::uint64_t request_id,
                                               const std::string& key, bool ok,
                                               const std::string& message)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, k = key, ok, msg = message]() mutable
        {
            shell->handle_profile_field_result_ui_(request_id, std::move(k),
                                                   ok, std::move(msg));
        });
}

void EventHandlerBase::on_extended_profile_ready(std::uint64_t request_id,
                                                  const std::string& profile_json)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, j = profile_json]() mutable
        {
            shell->handle_extended_profile_ready_ui_(request_id, std::move(j));
        });
}

void EventHandlerBase::on_room_action_complete(std::uint64_t request_id,
                                               bool ok,
                                               const std::string& joined_room_id,
                                               const std::string& message)
{
    shell()->post_to_ui_(
        [shell = shell(), request_id, ok, rid = joined_room_id,
         msg = message]() mutable
        {
            shell->handle_room_action_complete_ui_(request_id, ok,
                                                   std::move(rid),
                                                   std::move(msg));
        });
}

void EventHandlerBase::on_presence_changed(const std::string& user_id,
                                           PresenceState state)
{
    shell()->post_to_ui_(
        [shell = shell(), uid = user_id, s = state]()
        {
            shell->handle_presence_changed_ui_(uid, s);
        });
}

#ifdef TESSERACT_CALLS_ENABLED

void EventHandlerBase::on_call_invitation(const std::string& room_id,
                                           const std::string& slot_id,
                                           const std::string& caller_user_id,
                                           const std::string& call_intent,
                                           std::uint64_t      lifetime_ms,
                                           const std::string& notification_event_id)
{
    shell()->post_to_ui_(
        [shell = shell(), rid = room_id, sid = slot_id, caller = caller_user_id,
         intent = call_intent, lms = lifetime_ms, notif_id = notification_event_id]()
        {
            shell->handle_rtc_invitation_ui_(rid, sid, caller, intent, lms, notif_id);
        });
}

void EventHandlerBase::on_call_participant_joined(std::uint64_t session_id,
                                                   const RtcParticipantInfo& info)
{
    shell()->post_to_ui_(
        [shell = shell(), session_id, p = info]()
        {
            shell->handle_rtc_participant_joined_ui_(session_id, p);
        });
}

void EventHandlerBase::on_call_participant_left(std::uint64_t session_id,
                                                 const std::string& participant_id)
{
    shell()->post_to_ui_(
        [shell = shell(), session_id, pid = participant_id]()
        {
            shell->handle_rtc_participant_left_ui_(session_id, pid);
        });
}

void EventHandlerBase::on_call_participant_updated(std::uint64_t session_id,
                                                    const RtcParticipantInfo& info)
{
    shell()->post_to_ui_(
        [shell = shell(), session_id, p = info]()
        {
            shell->handle_rtc_participant_updated_ui_(session_id, p);
        });
}

void EventHandlerBase::on_call_ended(std::uint64_t session_id,
                                      const std::string& reason)
{
    shell()->post_to_ui_(
        [shell = shell(), session_id, r = reason]()
        {
            shell->handle_rtc_session_ended_ui_(session_id, r);
        });
}

void EventHandlerBase::on_call_video_frame(std::uint64_t session_id,
                                            const std::string& participant_id,
                                            std::uint32_t width,
                                            std::uint32_t height,
                                            const std::uint8_t* rgba,
                                            std::size_t rgba_size)
{
    // Convert RGBA → premultiplied BGRA on this worker thread, then hand the
    // shared_ptr to the UI thread — no second memcpy in push_video_frame().
    // Video frames are always fully opaque (α=255 from the I420 decoder), so
    // premultiplication is an identity and we only need a channel swap.
    struct Payload
    {
        std::uint64_t session_id;
        std::string   participant_id;
        std::uint32_t width;
        std::uint32_t height;
        std::shared_ptr<std::vector<std::uint8_t>> bgra; // premultiplied
    };
    const std::size_t n_px = static_cast<std::size_t>(width) * height;
    auto bgra = std::make_shared<std::vector<std::uint8_t>>(rgba_size);
    const bool opaque = (rgba_size > 0 && rgba[3] == 255);
    if (opaque)
    {
        for (std::size_t i = 0; i < n_px; ++i)
        {
            (*bgra)[i * 4 + 0] = rgba[i * 4 + 2]; // B
            (*bgra)[i * 4 + 1] = rgba[i * 4 + 1]; // G
            (*bgra)[i * 4 + 2] = rgba[i * 4 + 0]; // R
            (*bgra)[i * 4 + 3] = 255u;
        }
    }
    else
    {
        for (std::size_t i = 0; i < n_px; ++i)
        {
            const unsigned a = rgba[i * 4 + 3];
            (*bgra)[i * 4 + 0] = static_cast<std::uint8_t>((rgba[i*4+2] * a + 127u) / 255u);
            (*bgra)[i * 4 + 1] = static_cast<std::uint8_t>((rgba[i*4+1] * a + 127u) / 255u);
            (*bgra)[i * 4 + 2] = static_cast<std::uint8_t>((rgba[i*4+0] * a + 127u) / 255u);
            (*bgra)[i * 4 + 3] = static_cast<std::uint8_t>(a);
        }
    }
    auto p = std::make_shared<Payload>(
        Payload{session_id, participant_id, width, height, std::move(bgra)});
    shell()->post_to_ui_(
        [shell = shell(), p]()
        {
            shell->handle_rtc_video_frame_ui_(p->session_id, p->participant_id,
                                              p->width, p->height, p->bgra);
        });
}

void EventHandlerBase::on_call_screen_frame(std::uint64_t session_id,
                                             const std::string& participant_id,
                                             std::uint32_t width,
                                             std::uint32_t height,
                                             const std::uint8_t* rgba,
                                             std::size_t rgba_size)
{
    struct Payload
    {
        std::uint64_t session_id;
        std::string   participant_id;
        std::uint32_t width;
        std::uint32_t height;
        std::shared_ptr<std::vector<std::uint8_t>> bgra;
    };
    const std::size_t n_px = static_cast<std::size_t>(width) * height;
    auto bgra = std::make_shared<std::vector<std::uint8_t>>(rgba_size);
    const bool opaque = (rgba_size > 0 && rgba[3] == 255);
    if (opaque)
    {
        for (std::size_t i = 0; i < n_px; ++i)
        {
            (*bgra)[i * 4 + 0] = rgba[i * 4 + 2]; // B
            (*bgra)[i * 4 + 1] = rgba[i * 4 + 1]; // G
            (*bgra)[i * 4 + 2] = rgba[i * 4 + 0]; // R
            (*bgra)[i * 4 + 3] = 255u;
        }
    }
    else
    {
        for (std::size_t i = 0; i < n_px; ++i)
        {
            const unsigned a = rgba[i * 4 + 3];
            (*bgra)[i * 4 + 0] = static_cast<std::uint8_t>((rgba[i*4+2] * a + 127u) / 255u);
            (*bgra)[i * 4 + 1] = static_cast<std::uint8_t>((rgba[i*4+1] * a + 127u) / 255u);
            (*bgra)[i * 4 + 2] = static_cast<std::uint8_t>((rgba[i*4+0] * a + 127u) / 255u);
            (*bgra)[i * 4 + 3] = static_cast<std::uint8_t>(a);
        }
    }
    auto p = std::make_shared<Payload>(
        Payload{session_id, participant_id, width, height, std::move(bgra)});
    shell()->post_to_ui_(
        [shell = shell(), p]()
        {
            shell->handle_rtc_screen_frame_ui_(p->session_id, p->participant_id,
                                               p->width, p->height, p->bgra);
        });
}

void EventHandlerBase::on_call_audio_frame(std::uint64_t session_id,
                                            const std::string& /*participant_id*/,
                                            const std::int16_t* samples,
                                            std::size_t sample_count,
                                            std::uint32_t sample_rate,
                                            std::uint32_t num_channels)
{
    // AudioPlayback::push_frame() is documented thread-safe (audio_playback.h).
    // Call it directly on this worker thread — no post_to_ui_() overhead.
    // The mutex in push_call_audio_bgnd_() ensures the output object is not
    // destroyed while we are inside push_frame().
    shell()->push_call_audio_bgnd_(samples, sample_count, sample_rate, num_channels);
    (void)session_id;
}

#endif // TESSERACT_CALLS_ENABLED

} // namespace tesseract
