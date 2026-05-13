#include "tesseract/event_handler_bridge.h"
#include "ffi_convert.h"

namespace tesseract_ffi {

void EventHandlerBridge::on_timeline_reset(
    rust::Str room_id,
    const rust::Vec<TimelineEvent>& snapshot) const
{
    if (!handler_) return;
    std::vector<std::unique_ptr<tesseract::Event>> events;
    events.reserve(snapshot.size());
    for (const auto& ev : snapshot)
        events.push_back(tesseract::make_event(ev));
    handler_->on_timeline_reset(std::string(room_id), std::move(events));
}

void EventHandlerBridge::on_message_inserted(
    rust::Str room_id,
    std::uint64_t index,
    const TimelineEvent& ev) const
{
    if (!handler_) return;
    handler_->on_message_inserted(
        std::string(room_id),
        static_cast<std::size_t>(index),
        tesseract::make_event(ev));
}

void EventHandlerBridge::on_message_updated(
    rust::Str room_id,
    std::uint64_t index,
    const TimelineEvent& ev) const
{
    if (!handler_) return;
    handler_->on_message_updated(
        std::string(room_id),
        static_cast<std::size_t>(index),
        tesseract::make_event(ev));
}

void EventHandlerBridge::on_message_removed(
    rust::Str room_id,
    std::uint64_t index) const
{
    if (!handler_) return;
    handler_->on_message_removed(
        std::string(room_id),
        static_cast<std::size_t>(index));
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
    rust::Str context, rust::Str message, bool soft_logout) const
{
    if (!handler_) return;
    handler_->on_sync_error(std::string(context), std::string(message), soft_logout);
}

void EventHandlerBridge::on_session_refreshed(rust::Str session_json) const {
    if (!handler_) return;
    handler_->on_session_saved(std::string(session_json));
}

void EventHandlerBridge::on_backup_progress(const BackupProgress& progress) const {
    if (!handler_) return;
    handler_->on_backup_progress(tesseract::from_ffi(progress));
}

void EventHandlerBridge::on_room_list_state(std::uint8_t state) const {
    if (!handler_) return;
    // Clamp unknown codes back to Init so the C++ side never sees an
    // out-of-range enum value if the Rust protocol ever adds a state we
    // don't know yet. The exhaustive Rust mapper makes this a defensive
    // belt-and-braces — there's no path that produces an invalid code today.
    tesseract::RoomListState rls = (state <= static_cast<std::uint8_t>(
                                        tesseract::RoomListState::Terminated))
        ? static_cast<tesseract::RoomListState>(state)
        : tesseract::RoomListState::Init;
    handler_->on_room_list_state(rls);
}

void EventHandlerBridge::on_image_packs_updated() const {
    if (!handler_) return;
    handler_->on_image_packs_updated();
}

void EventHandlerBridge::on_account_prefs_updated(rust::Str json) const {
    if (!handler_) return;
    handler_->on_account_prefs_updated(std::string(json));
}

void EventHandlerBridge::on_notification(
    rust::Str room_id, rust::Str room_name,
    rust::Str sender, rust::Str body, bool is_mention) const
{
    if (!handler_) return;
    handler_->on_notification(std::string(room_id), std::string(room_name),
                               std::string(sender), std::string(body), is_mention);
}

} // namespace tesseract_ffi
