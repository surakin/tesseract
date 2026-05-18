#pragma once
#include <string>
#include <string_view>

namespace tesseract::text
{

// Trim leading/trailing ASCII whitespace (space, tab, CR, LF).
inline std::string trim(std::string_view s)
{
    constexpr const char* ws = " \t\n\r";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos)
    {
        return {};
    }
    const auto e = s.find_last_not_of(ws);
    return std::string(s.substr(b, e - b + 1));
}

} // namespace tesseract::text
