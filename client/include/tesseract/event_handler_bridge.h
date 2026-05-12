#pragma once
/// This header is included by the cxx-generated Rust bridge (see sdk/src/lib.rs).
/// It maps the Rust extern "C++" type EventHandlerBridge to the concrete C++ type
/// that wraps tesseract::IEventHandler.

#include "rust/cxx.h"
#include "event_handler.h"
#include "types.h"
#include <cstdint>
#include <memory>

// Forward declarations for cxx-generated types.
namespace tesseract_ffi {
    struct TimelineEvent;
    struct RoomInfo;
    struct BackupProgress;
}

namespace tesseract_ffi {

/// Concrete bridge object whose pointer is handed to the Rust sync loop.
/// Rust holds a UniquePtr<EventHandlerBridge> and calls the methods below.
class EventHandlerBridge {
public:
    explicit EventHandlerBridge(tesseract::IEventHandler* handler)
        : handler_(handler) {}

    /// Atomically reset the room's timeline to `snapshot` (oldest-first).
    void on_timeline_reset(rust::Str room_id,
                            const rust::Vec<TimelineEvent>& snapshot) const;

    /// Insert `event` at visible-index `index` in `room_id`'s timeline.
    void on_message_inserted(rust::Str room_id,
                              std::uint64_t index,
                              const TimelineEvent& event) const;

    /// Replace the event currently at visible-index `index` with `event`.
    void on_message_updated(rust::Str room_id,
                             std::uint64_t index,
                             const TimelineEvent& event) const;

    /// Remove the event at visible-index `index`.
    void on_message_removed(rust::Str room_id,
                             std::uint64_t index) const;

    void on_rooms_updated(const rust::Vec<RoomInfo>& rooms) const;
    void on_error(rust::Str context, rust::Str message, bool soft_logout) const;
    void on_session_refreshed(rust::Str session_json) const;
    void on_backup_progress(const BackupProgress& progress) const;

private:
    tesseract::IEventHandler* handler_; // non-owning
};

} // namespace tesseract_ffi
