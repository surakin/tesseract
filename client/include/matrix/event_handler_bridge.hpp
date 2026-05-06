#pragma once
/// This header is included by the cxx-generated Rust bridge (see sdk/src/lib.rs).
/// It maps the Rust extern "C++" type EventHandlerBridge to the concrete C++ type
/// that wraps matrix::IEventHandler.

#include "event_handler.hpp"
#include "types.hpp"

// Forward declarations for cxx-generated types.
namespace matrix_ffi {
    struct TimelineEvent;
    struct RoomInfo;
}

namespace matrix_ffi {

/// Concrete bridge object whose pointer is handed to the Rust sync loop.
/// Rust holds a UniquePtr<EventHandlerBridge> and calls the methods below.
class EventHandlerBridge {
public:
    explicit EventHandlerBridge(matrix::IEventHandler* handler)
        : handler_(handler) {}

    void on_sync_token(rust::Str token) const;
    void on_message_event(const TimelineEvent& event) const;
    void on_rooms_updated(const rust::Vec<RoomInfo>& rooms) const;
    void on_error(rust::Str context, rust::Str message) const;

private:
    matrix::IEventHandler* handler_; // non-owning; lifetime managed by MatrixClient
};

} // namespace matrix_ffi
