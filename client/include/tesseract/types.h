#pragma once
#include <cstdint>
#include <string>

namespace tesseract {

enum class EventType { Text, Image, File, Unhandled };

struct Event {
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string sender_name;
    std::string sender_avatar_url;
    std::string body;
    uint64_t    timestamp = 0;
    EventType   type = EventType::Unhandled;
    virtual ~Event() = default;
};

struct TextEvent : public Event {
    TextEvent() { type = EventType::Text; }
};

struct ImageEvent : public Event {
    std::string image_url;  // mxc:// URI
    uint64_t    width = 0;
    uint64_t    height = 0;

    ImageEvent() { type = EventType::Image; }
};

struct FileEvent : public Event {
    std::string file_url;   // mxc:// URI
    std::string file_name;
    uint64_t    file_size = 0;

    FileEvent() { type = EventType::File; }
};

/// Fallback for message types we don't handle yet (stickers, reactions, etc.)
struct UnhandledEvent : public Event {
    std::string msg_type;

    UnhandledEvent() { type = EventType::Unhandled; }
    explicit UnhandledEvent(const std::string& t) : msg_type(t) { type = EventType::Unhandled; }
};

struct RoomInfo {
    std::string id;
    std::string name;
    std::string topic;
    uint64_t    unread_count = 0;
    bool        is_direct    = false;
    std::string avatar_url;
};

} // namespace tesseract