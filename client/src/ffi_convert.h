#pragma once
#include "tesseract/client.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"
#include "tesseract/types.h"
#include <memory>
#include <string>

namespace tesseract {

inline Result from_ffi(const tesseract_ffi::OpResult& r) {
    return { r.ok, std::string(r.message) };
}

inline RoomInfo from_ffi(const tesseract_ffi::RoomInfo& r) {
    return {
        .id           = std::string(r.id),
        .name         = std::string(r.name),
        .topic        = std::string(r.topic),
        .unread_count = r.unread_count,
        .is_direct    = r.is_direct,
        .avatar_url   = std::string(r.avatar_url),
    };
}

inline std::unique_ptr<Event> make_event(const tesseract_ffi::TimelineEvent& e) {
    std::string msg_type(e.msg_type);

    if (msg_type == "m.text") {
        auto ev = std::make_unique<TextEvent>();
        ev->event_id = std::string(e.event_id);
        ev->room_id = std::string(e.room_id);
        ev->sender = std::string(e.sender);
        ev->sender_name = std::string(e.sender_name);
        ev->sender_avatar_url = std::string(e.sender_avatar_url);
        ev->body = std::string(e.body);
        ev->timestamp = e.timestamp;
        return ev;
    }

    if (msg_type == "m.image") {
        auto ev = std::make_unique<ImageEvent>();
        ev->event_id = std::string(e.event_id);
        ev->room_id = std::string(e.room_id);
        ev->sender = std::string(e.sender);
        ev->sender_name = std::string(e.sender_name);
        ev->sender_avatar_url = std::string(e.sender_avatar_url);
        ev->body = std::string(e.body);
        ev->timestamp = e.timestamp;
        ev->image_url = std::string(e.source_json);
        ev->width = e.width;
        ev->height = e.height;
        return ev;
    }

    if (msg_type == "m.file") {
        auto ev = std::make_unique<FileEvent>();
        ev->event_id = std::string(e.event_id);
        ev->room_id = std::string(e.room_id);
        ev->sender = std::string(e.sender);
        ev->sender_name = std::string(e.sender_name);
        ev->sender_avatar_url = std::string(e.sender_avatar_url);
        ev->body = std::string(e.body);
        ev->timestamp = e.timestamp;
        ev->file_url = std::string(e.file_json);
        ev->file_name = std::string(e.file_name);
        ev->file_size = e.file_size;
        return ev;
    }

    // Fallback for unhandled message types
    auto ev = std::make_unique<UnhandledEvent>(msg_type);
    ev->event_id = std::string(e.event_id);
    ev->room_id = std::string(e.room_id);
    ev->sender = std::string(e.sender);
    ev->sender_name = std::string(e.sender_name);
    ev->sender_avatar_url = std::string(e.sender_avatar_url);
    ev->body = std::string(e.body);
    ev->timestamp = e.timestamp;
    return ev;
}

} // namespace tesseract