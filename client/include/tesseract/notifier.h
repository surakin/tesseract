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
};

class INotifier
{
public:
    virtual ~INotifier() = default;
    virtual void notify(const Notification& n) = 0;
};

} // namespace tesseract
