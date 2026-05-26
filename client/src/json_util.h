#pragma once
#include <cstdio>
#include <string>
#include <string_view>

namespace tesseract
{

// JSON-escape a string value. Writers that build flat JSON objects by
// concatenation use this to ensure a malformed value from the server cannot
// emit invalid JSON and silently lose the containing object on next parse.
inline std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[7];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

} // namespace tesseract
