#pragma once
#include <string>
#include <string_view>

namespace tesseract
{

inline void html_escape_to(std::string_view s, std::string& out)
{
    for (char c : s)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
            break;
        }
    }
}

} // namespace tesseract
