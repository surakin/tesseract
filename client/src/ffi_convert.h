#pragma once
#include "tesseract/client.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"
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

inline Message from_ffi(const tesseract_ffi::TimelineEvent& e) {
    return {
        .event_id          = std::string(e.event_id),
        .room_id           = std::string(e.room_id),
        .sender            = std::string(e.sender),
        .sender_name       = std::string(e.sender_name),
        .sender_avatar_url = std::string(e.sender_avatar_url),
        .body              = std::string(e.body),
        .timestamp         = e.timestamp,
        .msg_type          = std::string(e.msg_type),
    };
}

} // namespace tesseract
