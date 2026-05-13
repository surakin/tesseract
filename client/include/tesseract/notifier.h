#pragma once
#include <string>

namespace tesseract {

struct Notification {
    std::string room_id;
    std::string room_name;
    std::string sender;
    std::string body;
    bool        is_mention = false;
};

class INotifier {
public:
    virtual ~INotifier() = default;
    virtual void notify(const Notification& n) = 0;
};

} // namespace tesseract
