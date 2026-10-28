#pragma once
#include <cctype>
#include <string>

namespace tesseract::linux_portal
{

// XDG portal notification IDs must match [a-zA-Z0-9_-]+. Matrix room IDs
// contain '!', ':' and '.' which are invalid — map them to '_'.
// Returns "_" when the result would otherwise be empty.
inline std::string sanitize_notification_id(const std::string& room_id)
{
    std::string out;
    out.reserve(room_id.size());
    for (unsigned char c : room_id)
    {
        out += (std::isalnum(c) || c == '_' || c == '-')
                   ? static_cast<char>(c)
                   : '_';
    }
    return out.empty() ? std::string("_") : out;
}

} // namespace tesseract::linux_portal
