#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tesseract
{

struct Notification
{
    std::string room_id;
    std::string room_name;
    std::string sender;
    std::string body;
    bool is_mention = false;
    std::vector<uint8_t>
        avatar_bytes; // raw PNG/JPEG of the room avatar; empty = use app icon
    // Raw encoded bytes (PNG/JPEG/GIF/WebP) of the message's image or
    // sticker, shown as the notification's picture. Empty for non-image
    // messages, or when the privacy gate suppressed it (locked screen /
    // disabled in settings).
    std::vector<uint8_t> image_bytes;
    // Event ID of the message that triggered this notification, or empty if
    // unknown. When non-empty, a quick reply is sent as a proper threaded
    // reply (m.in_reply_to); when empty, it falls back to a plain message.
    std::string event_id;
};

class INotifier
{
public:
    virtual ~INotifier() = default;
    virtual void notify(const Notification& n) = 0;
};

} // namespace tesseract
