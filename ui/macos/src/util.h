#pragma once
#include <string>

namespace tesseract::macos
{

inline std::string trim(std::string s)
{
    auto a = s.find_first_not_of(" \t\n\r");
    auto b = s.find_last_not_of(" \t\n\r");
    if (a == std::string::npos)
    {
        return {};
    }
    return s.substr(a, b - a + 1);
}

} // namespace tesseract::macos
