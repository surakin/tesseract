#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tesseract
{

/// `s` with every byte that isn't part of well-formed UTF-8 (overlong forms,
/// surrogates, values above U+10FFFF, truncated sequences) replaced by U+FFFD.
/// cxx throws std::invalid_argument when a std::string with invalid UTF-8 is
/// passed to Rust as &str or String, which on the UI thread ends the process;
/// text that comes from the file system (a Linux file name is arbitrary bytes)
/// goes through this first. Valid input is returned unchanged.
inline std::string utf8_lossy(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    const auto at = [&](std::size_t i)
    {
        return static_cast<unsigned char>(s[i]);
    };
    const auto cont = [&](std::size_t i)
    {
        return i < s.size() && (at(i) & 0xC0) == 0x80;
    };
    for (std::size_t i = 0; i < s.size();)
    {
        const unsigned char c = at(i);
        std::size_t len = 0;
        if (c < 0x80)
        {
            len = 1;
        }
        else if (c >= 0xC2 && c <= 0xDF && cont(i + 1))
        {
            len = 2;
        }
        else if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2))
        {
            const unsigned char c1 = at(i + 1);
            const bool overlong = c == 0xE0 && c1 < 0xA0;
            const bool surrogate = c == 0xED && c1 >= 0xA0;
            len = (overlong || surrogate) ? 0 : 3;
        }
        else if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3))
        {
            const unsigned char c1 = at(i + 1);
            const bool overlong = c == 0xF0 && c1 < 0x90;
            const bool too_big = c == 0xF4 && c1 >= 0x90;
            len = (overlong || too_big) ? 0 : 4;
        }
        if (len == 0)
        {
            out += "\xEF\xBF\xBD";
            ++i;
        }
        else
        {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    return out;
}

} // namespace tesseract
