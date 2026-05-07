#include "tesseract/event_handler_bridge.h"
#include "ffi_convert.h"

namespace tesseract_ffi {

void EventHandlerBridge::on_message_event(const TimelineEvent& ev) const {
    if (!handler_) return;
    handler_->on_message(tesseract::from_ffi(ev));
}

void EventHandlerBridge::on_rooms_updated(
    const rust::Vec<RoomInfo>& rooms) const
{
    if (!handler_) return;

    std::vector<tesseract::RoomInfo> cpp_rooms;
    cpp_rooms.reserve(rooms.size());
    for (const auto& r : rooms)
        cpp_rooms.push_back(tesseract::from_ffi(r));
    handler_->on_rooms_updated(cpp_rooms);
}

void EventHandlerBridge::on_error(
    rust::Str context, rust::Str message) const
{
    if (!handler_) return;
    handler_->on_sync_error(std::string(context), std::string(message));
}

void EventHandlerBridge::on_session_refreshed(rust::Str session_json) const {
    if (!handler_) return;
    handler_->on_session_saved(std::string(session_json));
}

void EventHandlerBridge::on_timeline_reset(rust::Str room_id) const {
    if (!handler_) return;
    handler_->on_timeline_reset(std::string(room_id));
}

} // namespace tesseract_ffi
