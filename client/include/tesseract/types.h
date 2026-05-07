#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tesseract {

struct RoomInfo {
    std::string id;
    std::string name;
    std::string topic;
    uint64_t    unread_count = 0;
    bool        is_direct    = false;
    std::string avatar_url;   ///< mxc:// URI; empty when the room has no avatar.
};

struct Message {
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string sender_name;        ///< resolved display name; empty → fall back to sender
    std::string sender_avatar_url;  ///< mxc:// URI; empty when user has no avatar
    std::string body;
    uint64_t    timestamp = 0; ///< Unix ms
    std::string msg_type;      ///< "m.text", "m.image", …
};

} // namespace tesseract
