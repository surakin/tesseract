#pragma once
#include <tesseract/event_handler.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tesseract
{

class ShellBase; // forward declaration — full definition in ShellBase.h

// Centralised IEventHandler adapter shared by all four platform shells.
// Stores a ShellBase* and marshals every callback to the UI thread via
// shell->post_to_ui_(), then calls the corresponding handle_*_ui_() virtual
// on the shell. Platform EventHandler/EventBridge classes can inherit from
// this (adding only what is platform-unique, e.g. Qt6's QObject base).
class EventHandlerBase : public IEventHandler
{
public:
    explicit EventHandlerBase(ShellBase* shell) : shell_(shell)
    {
    }

    void set_user_id(std::string id)
    {
        user_id_ = std::move(id);
    }
    const std::string& user_id() const
    {
        return user_id_;
    }

    void
    on_timeline_reset(const std::string& room_id,
                      EventList snapshot) override;
    void on_message_inserted(const std::string& room_id, std::size_t index,
                             std::unique_ptr<Event> event) override;
    void on_message_updated(const std::string& room_id, std::size_t index,
                            std::unique_ptr<Event> event) override;
    void on_message_removed(const std::string& room_id,
                            std::size_t index) override;
    void
    on_thread_reset(const std::string& room_id,
                    const std::string& thread_root,
                    EventList snapshot) override;
    void on_thread_inserted(const std::string& room_id,
                            const std::string& thread_root,
                            std::size_t index,
                            std::unique_ptr<Event> event) override;
    void on_thread_updated(const std::string& room_id,
                           const std::string& thread_root,
                           std::size_t index,
                           std::unique_ptr<Event> event) override;
    void on_thread_removed(const std::string& room_id,
                           const std::string& thread_root,
                           std::size_t index) override;
    void on_threads_updated(const std::string& room_id) override;
    void on_media_ready(std::uint64_t request_id,
                        const std::vector<std::uint8_t>& bytes) override;
    void on_url_preview_ready(std::uint64_t request_id,
                              const std::string& preview_json) override;
    void on_gif_results(std::uint64_t request_id,
                        const std::vector<GifResult>& results) override;
    void on_gif_search_failed(std::uint64_t request_id,
                              const std::string& message) override;
    void on_rooms_updated(const std::vector<RoomInfo>& rooms) override;
    void on_invites_updated(const std::vector<InviteInfo>& invites) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_session_saved(const std::string& session_json) override;
    void on_backup_progress(const BackupProgress& progress) override;
    void on_enable_recovery_progress(uint8_t step,
                                     const std::string& recovery_key,
                                     uint32_t backed_up,
                                     uint32_t total) final;
    void on_crypto_reset_result(bool ok, const std::string& message) final;
    void on_room_list_state(RoomListState state) override;
    void on_inflight_changed(uint32_t count) override;
    void on_image_packs_updated() override;
    void on_account_prefs_updated(const std::string& json) override;
    void on_media_preview_config_updated(const std::string& json) override;
    void on_notification(const std::string& room_id,
                         const std::string& room_name,
                         const std::string& sender, const std::string& body,
                         bool is_mention,
                         const std::vector<uint8_t>& avatar_bytes,
                         const std::vector<uint8_t>& image_bytes) override;
    void on_verification_request(const std::string& flow_id,
                                 const std::string& user_id,
                                 const std::string& device_id,
                                 bool incoming) override;
    void on_sas_ready(const std::string& flow_id,
                      std::vector<VerificationEmoji> emojis) override;
    void on_verification_done(const std::string& flow_id) override;
    void on_verification_cancelled(const std::string& flow_id,
                                   const std::string& reason) override;
    void on_verification_state_changed(bool is_verified) override;
    void on_typing_changed(const std::string& room_id,
                           const std::vector<std::string>& names) override;
    void on_presence_changed(const std::string& user_id,
                             PresenceState state) override;

protected:
    ShellBase* shell_;
    std::string user_id_;
};

} // namespace tesseract
