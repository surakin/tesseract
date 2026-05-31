#pragma once
/// This header is included by the cxx-generated Rust bridge (see sdk/src/lib.rs).
/// It maps the Rust extern "C++" type EventHandlerBridge to the concrete C++ type
/// that wraps tesseract::IEventHandler.

#include "rust/cxx.h"
#include "event_handler.h"
#include "types.h"
#include <cstdint>
#include <memory>
#include <mutex>

// Forward declarations for cxx-generated types.
namespace tesseract_ffi
{
struct TimelineEvent;
struct RoomInfo;
struct InviteInfo;
struct BackupProgress;
struct VerificationEmoji;
} // namespace tesseract_ffi

namespace tesseract_ffi
{

/// Thread-safe slot holding the non-owning IEventHandler pointer the bridge
/// dispatches to. The bridge reads the handler through this slot under a mutex,
/// so detach() (called during sync teardown) is observed atomically by any
/// callback already executing on a worker thread.
///
/// Why this exists: the Rust sync loop's stop_sync() aborts its tasks, but
/// tokio abort() only *requests* cancellation — a task already running a
/// synchronous C++ callback through this bridge runs to its next await point
/// before it actually stops, so a late callback can still fire after stop_sync
/// returns (see sdk/src/client/sync.rs::stop_sync). The shell tears the
/// IEventHandler down shortly after stop_sync(); without this slot, that late
/// callback would dereference a dangling handler. Detaching the slot inside
/// Client::stop_sync() (before the handler dies) makes the null check in every
/// callback below live, so the stray callback drops cleanly instead.
class HandlerSlot
{
public:
    explicit HandlerSlot(tesseract::IEventHandler* handler) : handler_(handler)
    {
    }

    /// Sever the link to the IEventHandler. After this returns every load()
    /// observes nullptr. Idempotent; safe to call from any thread.
    void detach() noexcept
    {
        std::lock_guard<std::mutex> lk(mutex_);
        handler_ = nullptr;
    }

    /// Snapshot the handler under the lock. Returns nullptr once detached.
    tesseract::IEventHandler* load() const noexcept
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return handler_;
    }

private:
    mutable std::mutex mutex_;
    tesseract::IEventHandler* handler_; // non-owning
};

/// Concrete bridge object whose pointer is handed to the Rust sync loop.
/// Rust holds a UniquePtr<EventHandlerBridge> and calls the methods below.
class EventHandlerBridge
{
public:
    explicit EventHandlerBridge(std::shared_ptr<HandlerSlot> slot)
        : slot_(std::move(slot))
    {
    }

    /// Atomically reset the room's timeline to `snapshot` (oldest-first).
    void on_timeline_reset(rust::Str room_id,
                           const rust::Vec<TimelineEvent>& snapshot) const;

    /// Insert `event` at visible-index `index` in `room_id`'s timeline.
    void on_message_inserted(rust::Str room_id, std::uint64_t index,
                             const TimelineEvent& event) const;

    /// Replace the event currently at visible-index `index` with `event`.
    void on_message_updated(rust::Str room_id, std::uint64_t index,
                            const TimelineEvent& event) const;

    /// Remove the event at visible-index `index`.
    void on_message_removed(rust::Str room_id, std::uint64_t index) const;

    void on_rooms_updated(const rust::Vec<RoomInfo>& rooms) const;
    void on_invites_updated(const rust::Vec<InviteInfo>& invites) const;
    void on_error(rust::Str context, rust::Str message, bool soft_logout) const;
    void on_session_refreshed(rust::Str session_json) const;
    void on_backup_progress(const BackupProgress& progress) const;
    void on_enable_recovery_progress(std::uint8_t step,
                                     rust::Str recovery_key,
                                     std::uint32_t backed_up,
                                     std::uint32_t total) const;
    void on_room_list_state(std::uint8_t state) const;
    void on_image_packs_updated() const;
    void on_threads_updated(rust::Str room_id) const;
    void on_account_prefs_updated(rust::Str json) const;
    void on_media_preview_config_updated(rust::Str json) const;
    void on_notification(rust::Str room_id, rust::Str room_name,
                         rust::Str sender, rust::Str body, bool is_mention,
                         rust::Slice<const uint8_t> avatar_bytes,
                         rust::Slice<const uint8_t> image_bytes) const;

    void on_verification_request(rust::Str flow_id, rust::Str user_id,
                                 rust::Str device_id, bool incoming) const;
    void on_sas_ready(rust::Str flow_id,
                      const rust::Vec<VerificationEmoji>& emojis) const;
    void on_verification_done(rust::Str flow_id) const;
    void on_verification_cancelled(rust::Str flow_id, rust::Str reason) const;
    void on_verification_state_changed(bool verified) const;

    void on_typing_changed(rust::Str room_id,
                           const rust::Vec<rust::String>& user_ids) const;

    void on_presence_changed(rust::Str user_id, std::uint8_t state) const;

    /// Thread-timeline callbacks — mirror of the four room-timeline callbacks
    /// above with an additional `thread_root` parameter.
    void on_thread_reset(rust::Str room_id, rust::Str thread_root,
                         const rust::Vec<TimelineEvent>& snapshot) const;
    void on_thread_inserted(rust::Str room_id, rust::Str thread_root,
                            std::uint64_t index, const TimelineEvent& ev) const;
    void on_thread_updated(rust::Str room_id, rust::Str thread_root,
                           std::uint64_t index, const TimelineEvent& ev) const;
    void on_thread_removed(rust::Str room_id, rust::Str thread_root,
                           std::uint64_t index) const;

private:
    // Shared, mutex-guarded handler slot. `const`: the shared_ptr is set once
    // and never reassigned, so the worker-thread reads of it are data-race-free
    // by construction. The handler *value* is loaded under the slot's mutex via
    // slot_->load(), so detach() (run during teardown) is observed atomically
    // and the callbacks below drop cleanly afterwards. Client::Impl holds the
    // other shared_ptr to this same slot, so a single detach() severs every
    // live bridge at once.
    const std::shared_ptr<HandlerSlot> slot_;
};

/// Synchronously persist a refreshed OAuth session blob to the platform
/// secret store (see SessionStore::save_account). Invoked by matrix-sdk's
/// save_session_callback through the cxx bridge on a worker thread; `user_id`
/// is the full MXID the session belongs to. Unlike the EventHandlerBridge
/// callbacks this is a free function: it has no dependence on a live
/// IEventHandler, so it stays valid across sync start/stop and during the
/// runtime teardown the synchronous callback exists to survive.
void persist_session(rust::Str user_id, rust::Str session_json);

} // namespace tesseract_ffi
