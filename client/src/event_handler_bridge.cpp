#include "tesseract/event_handler_bridge.hpp"

// cxx-generated header (produced by sdk/build.rs)
#include "tesseract-sdk-ffi/src/lib.rs.h"

namespace tesseract_ffi {

void EventHandlerBridge::on_sync_token(rust::Str /*token*/) const {
    // Nothing to propagate for a plain sync token; extend if persistence needed.
}

void EventHandlerBridge::on_message_event(const TimelineEvent& ev) const {
    if (!handler_) return;

    tesseract::Message msg{
        .event_id  = std::string(ev.event_id),
        .room_id   = std::string(ev.room_id),
        .sender    = std::string(ev.sender),
        .body      = std::string(ev.body),
        .timestamp = ev.timestamp,
        .msg_type  = std::string(ev.msg_type),
    };
    handler_->on_message(msg);
}

void EventHandlerBridge::on_rooms_updated(
    const rust::Vec<RoomInfo>& rooms) const
{
    if (!handler_) return;

    std::vector<tesseract::RoomInfo> cpp_rooms;
    cpp_rooms.reserve(rooms.size());
    for (const auto& r : rooms) {
        cpp_rooms.push_back({
            .id           = std::string(r.id),
            .name         = std::string(r.name),
            .topic        = std::string(r.topic),
            .unread_count = r.unread_count,
            .is_direct    = r.is_direct,
        });
    }
    handler_->on_rooms_updated(cpp_rooms);
}

void EventHandlerBridge::on_error(
    rust::Str context, rust::Str message) const
{
    if (!handler_) return;
    handler_->on_sync_error(std::string(context), std::string(message));
}

void EventHandlerBridge::on_s