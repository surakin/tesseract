#pragma once
#include <tesseract/event_handler.h>
#include <atomic>
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

    // Re-point this bridge at a different window. Multi-window: each account is
    // owned by exactly one window at a time; when an account is popped out into
    // (or handed back from) its own window, its sole bridge follows the owner so
    // every SDK callback reaches the live window. shell_ is read on tokio worker
    // threads when callbacks fire and written here on the UI thread, hence atomic.
    void set_shell(ShellBase* s) { shell_.store(s, std::memory_order_release); }

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
    void on_messages_prepended(const std::string& room_id,
                               EventList events) override;
    void on_messages_appended(const std::string& room_id,
                              EventList events) override;
    void on_messages_updated_batch(const std::string& room_id,
                                   std::vector<std::size_t> indices,
                                   EventList events) override;
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
    void on_thread_messages_prepended(const std::string& room_id,
                                      const std::string& thread_root,
                                      EventList events) override;
    void on_thread_messages_appended(const std::string& room_id,
                                     const std::string& thread_root,
                                     EventList events) override;
    void on_threads_updated(const std::string& room_id) override;
    void on_media_ready(std::uint64_t request_id,
                        const std::vector<std::uint8_t>& bytes) override;
    void on_url_preview_ready(std::uint64_t request_id,
                              const std::string& preview_json) override;
    void on_gif_results(std::uint64_t request_id,
                        const std::vector<GifResult>& results) override;
    void on_gif_search_failed(std::uint64_t request_id,
                              const std::string& message) override;
    void on_forward_done(std::uint64_t request_id) override;
    void on_forward_failed(std::uint64_t request_id,
                           const std::string& message) override;
    void on_space_child_summary_ready(std::uint64_t request_id,
                                      const std::string& summary_json) override;
    void on_server_info_ready(std::uint64_t request_id,
                              const std::string& info_json) override;
    void on_media_preview_config_ready(std::uint64_t request_id,
                                       const std::string& config_json) override;
    void on_room_preview_override_ready(std::uint64_t request_id,
                                        const std::string& override_json) override;
    void on_search_results(std::uint64_t request_id,
                           const std::vector<SearchHit>& results) override;
    void on_search_failed(std::uint64_t request_id,
                          const std::string& message) override;
    void on_paginate_result(std::uint64_t request_id, bool ok,
                            bool reached_start, bool reached_end,
                            const std::string& message) override;
    void on_room_action_complete(std::uint64_t request_id, bool ok,
                                 const std::string& joined_room_id,
                                 const std::string& message) override;
    void on_upload_complete(std::uint64_t request_id, bool ok,
                            const std::string& message) override;
    void on_profile_field_result(std::uint64_t request_id,
                                 const std::string& key, bool ok,
                                 const std::string& message) override;
    void on_extended_profile_ready(std::uint64_t request_id,
                                   const std::string& profile_json) override;
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
#ifndef NDEBUG
    void on_inflight_changed_debug(std::uint32_t count, std::string urls) override;
#endif
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

#ifdef TESSERACT_CALLS_ENABLED
    void on_call_invitation(const std::string& room_id,
                             const std::string& slot_id,
                             const std::string& caller_user_id,
                             const std::string& call_intent,
                             std::uint64_t      lifetime_ms,
                             const std::string& notification_event_id) override;
    void on_call_participant_joined(std::uint64_t session_id,
                                    const RtcParticipantInfo& info) override;
    void on_call_participant_left(std::uint64_t session_id,
                                  const std::string& participant_id) override;
    void on_call_participant_updated(std::uint64_t session_id,
                                     const RtcParticipantInfo& info) override;
    void on_call_ended(std::uint64_t session_id,
                       const std::string& reason) override;
    void on_call_video_frame(std::uint64_t session_id,
                              const std::string& participant_id,
                              std::uint32_t width, std::uint32_t height,
                              const std::uint8_t* rgba,
                              std::size_t rgba_size) override;
    void on_call_screen_frame(std::uint64_t session_id,
                               const std::string& participant_id,
                               std::uint32_t width, std::uint32_t height,
                               const std::uint8_t* rgba,
                               std::size_t rgba_size) override;
    void on_call_audio_frame(std::uint64_t session_id,
                              const std::string& participant_id,
                              const std::int16_t* samples,
                              std::size_t sample_count,
                              std::uint32_t sample_rate,
                              std::uint32_t num_channels) override;
#endif // TESSERACT_CALLS_ENABLED

protected:
    // Current owner window. Atomic because SDK callbacks read it on tokio worker
    // threads while set_shell() writes it on the UI thread (multi-window re-point).
    // Read via shell() in callback bodies.
    std::atomic<ShellBase*> shell_;
    std::string user_id_;

    ShellBase* shell() const { return shell_.load(std::memory_order_acquire); }
};

} // namespace tesseract
