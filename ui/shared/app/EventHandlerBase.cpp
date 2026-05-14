#include "app/EventHandlerBase.h"
#include "app/ShellBase.h"
#include <tesseract/session_store.h>

namespace tesseract {

void EventHandlerBase::on_timeline_reset(
    const std::string& room_id,
    std::vector<std::unique_ptr<Event>> snapshot)
{
    // Wrap in shared_ptr so the move-only lambda is copy-constructible
    // (std::function requires a copyable target).
    struct Payload { std::string rid; std::vector<std::unique_ptr<Event>> snap; };
    auto p = std::make_shared<Payload>(Payload{room_id, std::move(snapshot)});
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable {
            shell->handle_timeline_reset_ui_(std::move(p->rid), std::move(p->snap));
        });
}

void EventHandlerBase::on_message_inserted(
    const std::string& room_id, std::size_t index, std::unique_ptr<Event> event)
{
    struct Payload { std::string rid; std::size_t idx; std::unique_ptr<Event> ev; };
    auto p = std::make_shared<Payload>(Payload{room_id, index, std::move(event)});
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable {
            shell->handle_message_inserted_ui_(std::move(p->rid), p->idx, std::move(p->ev));
        });
}

void EventHandlerBase::on_message_updated(
    const std::string& room_id, std::size_t index, std::unique_ptr<Event> event)
{
    struct Payload { std::string rid; std::size_t idx; std::unique_ptr<Event> ev; };
    auto p = std::make_shared<Payload>(Payload{room_id, index, std::move(event)});
    shell_->post_to_ui_(
        [shell = shell_, p]() mutable {
            shell->handle_message_updated_ui_(std::move(p->rid), p->idx, std::move(p->ev));
        });
}

void EventHandlerBase::on_message_removed(const std::string& room_id, std::size_t index)
{
    shell_->post_to_ui_(
        [shell = shell_, rid = room_id, idx = index]() mutable {
            shell->handle_message_removed_ui_(std::move(rid), idx);
        });
}

void EventHandlerBase::on_rooms_updated(const std::vector<RoomInfo>& rooms)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, rs = rooms]() mutable {
            shell->push_rooms_(std::move(uid), std::move(rs));
        });
}

void EventHandlerBase::on_sync_error(
    const std::string& context, const std::string& description, bool soft_logout)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, ctx = context,
         desc = description, sl = soft_logout]() mutable {
            shell->handle_sync_error_ui_(std::move(ctx), std::move(uid),
                                         std::move(desc), sl);
        });
}

void EventHandlerBase::on_session_saved(const std::string& session_json)
{
    if (!user_id_.empty())
        SessionStore::save_account(user_id_, session_json);
}

void EventHandlerBase::on_backup_progress(const BackupProgress& progress)
{
    shell_->post_to_ui_(
        [shell = shell_, p = progress]() {
            shell->handle_backup_progress_ui_(p);
        });
}

void EventHandlerBase::on_room_list_state(RoomListState state)
{
    shell_->post_to_ui_(
        [shell = shell_, s = state]() {
            shell->push_room_list_state_(s);
            shell->on_room_list_state_ui_();
        });
}

void EventHandlerBase::on_image_packs_updated()
{
    shell_->post_to_ui_(
        [shell = shell_]() {
            shell->handle_image_packs_updated_ui_();
        });
}

void EventHandlerBase::on_account_prefs_updated(const std::string& json)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_, j = json]() mutable {
            shell->handle_account_prefs_updated_ui_(std::move(uid), std::move(j));
        });
}

void EventHandlerBase::on_notification(
    const std::string& room_id, const std::string& room_name,
    const std::string& sender, const std::string& body,
    bool is_mention, const std::vector<uint8_t>& avatar_bytes)
{
    shell_->post_to_ui_(
        [shell = shell_, uid = user_id_,
         rid = room_id, rn = room_name,
         s = sender, b = body, im = is_mention, av = avatar_bytes]() mutable {
            shell->handle_notification_ui_(std::move(uid), std::move(rid),
                                           std::move(rn), std::move(s),
                                           std::move(b), im, std::move(av));
        });
}

} // namespace tesseract
