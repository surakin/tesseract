#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace matrix {

struct RoomInfo {
    std::string id;
    std::string name;
    std::string topic;
    uint64_t    unread_count = 0;
    bool        is_direct    = false;
};

struct Message {
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string body;
    uint64_t    timestamp = 0; ///< Unix ms
    std::string msg_type;      ///< "m.text", "m.image", …
};

} // namespace matrix
