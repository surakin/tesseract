#pragma once

// Small, dependency-free text helpers shared across views/shell code.

#include <cctype>
#include <cstddef>
#include <string_view>

namespace tk
{

// ASCII case-insensitive substring test: true iff `needle` occurs in `haystack`
// ignoring case (empty needle matches). This is the canonical form of the
// `name_matches` / `icontains` / `contains_ci` helpers that were copy-pasted
// across the room list, quick switcher, mention/shortcode/sticker engines, and
// the shell roster filter — new call sites should use this one.
inline bool ci_contains(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (haystack.size() < needle.size())
    {
        return false;
    }
    auto lower = [](char c)
    { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j)
        {
            if (lower(haystack[i + j]) != lower(needle[j]))
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}

} // namespace tk
