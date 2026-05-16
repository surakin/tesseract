#include "tesseract/event_handler_bridge.h"
#include "ffi_convert.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace {
// On a 32-bit target std::size_t is narrower than the Rust u64 index; a
// silent truncation would mutate the wrong timeline row. Reject the
// (impossible-in-practice but undefined) out-of-range case explicitly.
inline bool index_fits(std::uint64_t i) {
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        return i <= static_cast<std::uint64_t>(SIZE_MAX);
    } else {
        return true;
    }
}
} // namespace

namespace tesseract_ffi {

namespace {
// Every method below is invoked by cxx-generated Rust trampolines. An
// exception unwinding out of a `void` C++ callback into Rust is undefined
// behavior (the Rust unwinding ABI does not support it). `guard` gives a
// defined failure mode instead: the callback is dropped and the exception
// is logged, never propagated across the FFI boundary.
template <class F>
void guard(const char* where, F&& f) noexcept {
    try {
        f();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[tesseract] %s threw: %s (dropped)\n", where,
                     e.what());
    } catch (...) {
        std::fprintf(stderr, "[tesseract] %s threw non-exception (dropped)\n",
                     where);
    }
}
} // namespace

void EventHandlerBridge::on_timeline_reset(
    rust::Str room_id,
    const rust::Vec<TimelineEvent>& snapshot) const
{
    guard("on_timeline_reset", [&] {
        if (!handler_) return;
        std::vector<std::unique_ptr<tesseract::Event>> events;
        events.reserve(snapshot.size());
        for (const auto& ev : snapshot)
            events.push_back(tesseract::make_event(ev));
        handler_->on_timeline_reset(std::string(room_id), std::move(events));
    });
}

void EventHandlerBridge::on_message_inserted(
    rust::Str room_id,
    std::uint64_t index,
    const TimelineEvent& ev) const
{
    guard("on_message_inserted", [&] {
        if (!handler_ || !index_fits(index)) return;
        handler_->on_message_inserted(
            std::string(room_id),
            static_cast<std::size_t>(index),
            tesseract::make_event(ev));
    });
}

void EventHandlerBridge::on_message_updated(
    rust::Str room_id,
    std::uint64_t index,
    const TimelineEvent& ev) const
{
    guard("on_message_updated", [&] {
        if (!handler_ || !index_fits(index)) return;
        handler_->on_message_updated(
            std::string(room_id),
            static_cast<std::size_t>(index),
            tesseract::make_event(ev));
    });
}

void EventHandlerBridge::on_message_removed(
    rust::Str room_id,
    std::uint64_t index) const
{
    guard("on_message_removed", [&] {
        if (!handler_ || !index_fits(index)) return;
        handler_->on_message_removed(
            std::string(room_id),
            static_cast<std::size_t>(index));
    });
}

void EventHandlerBridge::on_rooms_updated(
    const rust::Vec<RoomInfo>& rooms) const
{
    guard("on_rooms_updated", [&] {
        if (!handler_) return;
        std::vector<tesseract::RoomInfo> cpp_rooms;
        cpp_rooms.reserve(rooms.size());
        for (const auto& r : rooms)
            cpp_rooms.push_back(tesseract::from_ffi(r));
        handler_->on_rooms_updated(cpp_rooms);
    });
}

void EventHandlerBridge::on_error(
    rust::Str context, rust::Str message, bool soft_logout) const
{
    guard("on_error", [&] {
        if (!handler_) return;
        handler_->on_sync_error(std::string(context), std::string(message),
                                soft_logout);
    });
}

void EventHandlerBridge::on_session_refreshed(rust::Str session_json) const {
    guard("on_session_refreshed", [&] {
        if (!handler_) return;
        handler_->on_session_saved(std::string(session_json));
    });
}

void EventHandlerBridge::on_backup_progress(const BackupProgress& progress) const {
    guard("on_backup_progress", [&] {
        if (!handler_) return;
        handler_->on_backup_progress(tesseract::from_ffi(progress));
    });
}

void EventHandlerBridge::on_room_list_state(std::uint8_t state) const {
    guard("on_room_list_state", [&] {
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
    });
}

void EventHandlerBridge::on_image_packs_updated() const {
    guard("on_image_packs_updated", [&] {
        if (!handler_) return;
        handler_->on_image_packs_updated();
    });
}

void EventHandlerBridge::on_account_prefs_updated(rust::Str json) const {
    guard("on_account_prefs_updated", [&] {
        if (!handler_) return;
        handler_->on_account_prefs_updated(std::string(json));
    });
}

void EventHandlerBridge::on_notification(
    rust::Str room_id, rust::Str room_name,
    rust::Str sender, rust::Str body, bool is_mention,
    rust::Slice<const uint8_t> avatar_bytes) const
{
    guard("on_notification", [&] {
        if (!handler_) return;
        std::vector<uint8_t> av(avatar_bytes.data(),
                                 avatar_bytes.data() + avatar_bytes.size());
        handler_->on_notification(std::string(room_id), std::string(room_name),
                                   std::string(sender), std::string(body),
                                   is_mention, av);
    });
}

void EventHandlerBridge::on_verification_request(
    rust::Str flow_id, rust::Str user_id, rust::Str device_id, bool incoming) const
{
    guard("on_verification_request", [&] {
        if (!handler_) return;
        handler_->on_verification_request(
            std::string(flow_id), std::string(user_id), std::string(device_id),
            incoming);
    });
}

void EventHandlerBridge::on_sas_ready(
    rust::Str flow_id, const rust::Vec<VerificationEmoji>& emojis) const
{
    guard("on_sas_ready", [&] {
        if (!handler_) return;
        std::vector<tesseract::VerificationEmoji> cpp_emojis;
        cpp_emojis.reserve(emojis.size());
        for (const auto& e : emojis)
            cpp_emojis.push_back({std::string(e.symbol), std::string(e.description)});
        handler_->on_sas_ready(std::string(flow_id), std::move(cpp_emojis));
    });
}

void EventHandlerBridge::on_verification_done(rust::Str flow_id) const {
    guard("on_verification_done", [&] {
        if (!handler_) return;
        handler_->on_verification_done(std::string(flow_id));
    });
}

void EventHandlerBridge::on_verification_cancelled(
    rust::Str flow_id, rust::Str reason) const
{
    guard("on_verification_cancelled", [&] {
        if (!handler_) return;
        handler_->on_verification_cancelled(std::string(flow_id),
                                            std::string(reason));
    });
}

void EventHandlerBridge::on_verification_state_changed(bool verified) const {
    guard("on_verification_state_changed", [&] {
        if (!handler_) return;
        handler_->on_verification_state_changed(verified);
    });
}

void EventHandlerBridge::on_typing_changed(
    rust::Str room_id,
    const rust::Vec<rust::String>& user_ids) const
{
    guard("on_typing_changed", [&] {
        if (!handler_) return;
        std::vector<std::string> ids;
        ids.reserve(user_ids.size());
        for (const auto& uid : user_ids) ids.push_back(std::string(uid));
        handler_->on_typing_changed(std::string(room_id), std::move(ids));
    });
}

} // namespace tesseract_ffi
