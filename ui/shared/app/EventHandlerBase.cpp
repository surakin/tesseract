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
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
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
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
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
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
        {
            shell->handle_message_updated_ui_(std::move(p->rid), p->idx,
                                              std::move(p->ev));
        });
}

void EventHandlerBase::on_message_removed(const std::string& room_id,
                                          std::size_t index)
{
    shell_->post_to_ui_(
        [shell = shell_, rid = room_id, idx = index]() mutable
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
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
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
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
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
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
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
    shell_->post_to_ui_(
        [shell = shell_, rid = room_id, root = thread_root,
         idx = index]() mutable
        {
            shell->handle_thread_removed_ui_(std::move(rid), std::move(root),
                                             idx);
        });
}

void EventHandlerBase::on_threads_updated(const std::string& room_id)
{
    shell_->post_to_ui_(
        [shell = shell_, rid = room_id]() mutable
        {
            shell->handle_threads_updated_ui_(std::move(rid));
        });
}

void EventHandlerBase::on_media_ready(std::uint64_t request_id,
                                      const std::vector<std::uint8_t>& bytes)
{
    auto b = std::make_shared<std::vector<std::uint8_t>>(bytes);
    shell_->post_to_ui_(
        [shell = shell_, request_id, b]() mutable
        {
            shell->handle_media_ready_ui_(request_id, std::move(*b));
        });
}

void EventHandlerBase::on_url_preview_ready(std::uint64_t request_id,
                                            const std::string& preview_json)
{
    shell_->post_to_ui_(
        [shell = shell_, request_id, json = preview_json]() mutable
        {
            shell->handle_url_preview_ready_ui_(request_id, std::move(json));
        });
}

void EventHandlerBase::on_gif_results(std::uint64_t request_id,
                                      const std::vector<GifResult>& results)
{
    auto r = std::make_shared<std::vector<GifResult>>(results);
    shell_->post_to_ui_(
        [shell = shell_, request_id, r]() mutable
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
    shell_->post_to_ui_(
        [shell = shell_, request_id, msg = message]() mutable
        {
            shell->dispatch_gif_failed_to_secondary_windows_(request_id, msg);
            shell->handle_gif_search_failed_ui_(request_id, std::move(msg));
        });
}

void EventHandlerBase::on_paginate_result(std::uint64_t request_id, bool ok,
                                          bool reached_start, bool reached_end,
                                          const std::string& message)
{
    shell_->post_to_ui_(
        [shell = shell_, request_id, ok, reached_start, reached_end,
         msg = message]() mutable
        {
            shell->handle_paginate_result_ui_(request_id, ok, reached_start,
                                             reached_end, std::move(msg));
        });
}

void EventHandlerBase::on_rooms_updated(const std::vector<RoomInfo>& rooms)
{
    auto rs = rooms; // one copy; moved into the lambda below
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, rs = std::move(rs)]() mutable
        {
            shell->push_rooms_(std::move(uid), std::move(rs));
        });
}

void EventHandlerBase::on_invites_updated(const std::vector<InviteInfo>& invites)
{
    auto inv = invites;  // one copy; moved into the lambda below
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, inv = std::move(inv)]() mutable
        {
            shell->push_invites_(std::move(uid), std::move(inv));
        });
}

void EventHandlerBase::on_sync_error(const std::string& context,
                                     const std::string& description,
                                     bool soft_logout)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, ctx = context, desc = description,
         sl = soft_logout]() mutable
        {
            if (ctx == "sync_offline" || ctx == "sync_error")
                shell->handle_offline_ui_();
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
    shell_->post_to_ui_(
        [shell = shell_, p = progress]()
        {
            shell->handle_backup_progress_ui_(p);
        });
}

void EventHandlerBase::on_enable_recovery_progress(uint8_t step,
                                                    const std::string& recovery_key,
                                                    uint32_t backed_up,
                                                    uint32_t total)
{
    auto* s = shell_;
    std::string key = recovery_key;
    s->post_to_ui_([s, step, key, backed_up, total]()
    {
        s->handle_enable_recovery_progress_ui_(step, key, backed_up, total);
    });
}

void EventHandlerBase::on_crypto_reset_result(bool ok, const std::string& message)
{
    auto* s = shell_;
    std::string msg = message;
    s->post_to_ui_([s, ok, msg]()
    {
        s->handle_crypto_reset_result_ui_(ok, msg);
    });
}

void EventHandlerBase::on_room_list_state(RoomListState state)
{
    shell_->post_to_ui_(
        [shell = shell_, s = state]()
        {
            shell->push_room_list_state_(s);
            shell->on_room_list_state_ui_();
            shell->on_inflight_ui_();
            if (s == RoomListState::Running)
            {
                shell->begin_server_info_fetch_();
                if (shell->offline_)
                    shell->handle_online_ui_();
            }
        });
}

void EventHandlerBase::on_inflight_changed(uint32_t count)
{
    shell_->post_to_ui_(
        [shell = shell_, n = count]()
        {
            // Re-read the authoritative atomic on the UI thread rather than
            // trusting the snapshot `n`: notifications from different worker
            // threads can be delivered out of order (or dropped if the Rust
            // handler mutex is contended), which would otherwise leave the
            // activity dot stuck above the true count. The live read makes the
            // displayed value converge regardless of delivery order.
            shell->last_inflight_ = shell->client_
                                        ? shell->client_->in_flight_count()
                                        : n;
            shell->on_inflight_ui_();
        });
}

void EventHandlerBase::on_image_packs_updated()
{
    shell_->post_to_ui_(
        [shell = shell_]()
        {
            shell->handle_image_packs_updated_ui_();
        });
}

void EventHandlerBase::on_account_prefs_updated(const std::string& json)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, j = json]() mutable
        {
            shell->handle_account_prefs_updated_ui_(std::move(uid),
                                                    std::move(j));
        });
}

void EventHandlerBase::on_media_preview_config_updated(const std::string& json)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, j = json]() mutable
        {
            shell->handle_media_preview_config_updated_ui_(std::move(uid),
                                                           std::move(j));
        });
}

void EventHandlerBase::on_notification(const std::string& room_id,
                                       const std::string& room_name,
                                       const std::string& sender,
                                       const std::string& body, bool is_mention,
                                       const std::vector<uint8_t>& avatar_bytes,
                                       const std::vector<uint8_t>& image_bytes)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, rid = room_id, rn = room_name,
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
    shell_->post_to_ui_(
        [shell = shell_, fid = flow_id, uid = user_id, did = device_id,
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
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
        {
            shell->handle_sas_ready_ui_(std::move(p->fid), std::move(p->em));
        });
}

void EventHandlerBase::on_verification_done(const std::string& flow_id)
{
    shell_->post_to_ui_(
        [shell = shell_, fid = flow_id]() mutable
        {
            shell->handle_verification_done_ui_(std::move(fid));
        });
}

void EventHandlerBase::on_verification_cancelled(const std::string& flow_id,
                                                 const std::string& reason)
{
    shell_->post_to_ui_(
        [shell = shell_, fid = flow_id, r = reason]() mutable
        {
            shell->handle_verification_cancelled_ui_(std::move(fid),
                                                     std::move(r));
        });
}

void EventHandlerBase::on_verification_state_changed(bool is_verified)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, v = is_verified]()
        {
            // Always persist the state so switch_active_account can restore it.
            for (auto& a : shell->accounts_)
            {
                if (a->user_id == uid)
                {
                    a->unverified = !v;
                    break;
                }
            }
            // Only touch the live UI when this is the active account.
            if (shell->active_account_index_ >= 0 &&
                shell->active_account_index_ <
                    static_cast<int>(shell->accounts_.size()) &&
                shell->accounts_[shell->active_account_index_]->user_id == uid)
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
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable
        {
            shell->handle_typing_changed_ui_(std::move(p->rid),
                                             std::move(p->ns));
        });
}

void EventHandlerBase::on_upload_complete(std::uint64_t request_id, bool ok,
                                           const std::string& message)
{
    shell_->post_to_ui_(
        [shell = shell_, request_id, ok, msg = message]() mutable
        {
            shell->handle_upload_complete_ui_(request_id, ok, std::move(msg));
        });
}

void EventHandlerBase::on_room_action_complete(std::uint64_t request_id,
                                               bool ok,
                                               const std::string& joined_room_id,
                                               const std::string& message)
{
    shell_->post_to_ui_(
        [shell = shell_, request_id, ok, rid = joined_room_id,
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
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id, s = state]()
        {
            shell->handle_presence_changed_ui_(uid, s);
        });
}

} // namespace tesseract
