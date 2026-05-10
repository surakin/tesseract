#pragma once
/// This header is included by the cxx-generated Rust bridge (see sdk/src/lib.rs).
/// It maps the Rust extern "C++" type EventHandlerBridge to the concrete C++ type
/// that wraps tesseract::IEventHandler.

#include "rust/cxx.h"
#include "event_handler.h"
#include "types.h"
#include <memory>

// Forward declarations for cxx-generated types.
namespace tesseract_ffi {
    struct TimelineEvent;
    struct RoomInfo;
}

namespace tesseract_ffi {

/// Concrete bridge object whose pointer is handed to the Rust sync loop.
/// Rust holds a UniquePtr<EventHandlerBridge> and calls the methods below.
class EventHandlerBridge {
public:
    explicit EventHandlerBridge(tesseract::IEventHandler* handler)
        : handler_(handler) {}

    void on_message_event(const TimelineEvent& event) const;
    void on_rooms_updated(const rust::Vec<RoomInfo>& rooms) const;
    void on_error(rust::Str context, rust::Str message, bool soft_logout) const;
    void on_session_refreshed(rust::Str session_json) const;
    /// Signals the UI to clear the message view for room_id before the
    /// initial cached timeline items arrive via on_message_event.
    void on_timeline_reset(rust::Str room_id) const;

private:
    tesseract::IEventHandler* handler_; // non-owning
    mutable std::unique_ptr<tesseract::Event> last_event_;
};

} // namespace tesseract_ffi