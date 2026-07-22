#pragma once

// Small case-insensitive string-matching helpers shared by LanguagePicker
// and TimezonePicker's match_rank_() implementations. Plain `inline`
// functions (not an anonymous namespace) since Unity builds can combine
// multiple .cpp files using these into one translation unit, where two
// identically-named anonymous-namespace definitions would collide.

#include <cctype>
#include <string_view>

namespace tesseract::views
{

inline bool icontains(std::string_view hay, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (hay.size() < needle.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
    {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size(); ++k)
        {
            if (std::tolower((unsigned char)hay[i + k]) !=
                std::tolower((unsigned char)needle[k]))
            {
                ok = false;
                break;
            }
        }
        if (ok)
            return true;
    }
    return false;
}

inline bool istarts_with(std::string_view hay, std::string_view prefix)
{
    if (hay.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower((unsigned char)hay[i]) != std::tolower((unsigned char)prefix[i]))
            return false;
    return true;
}

inline bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

} // namespace tesseract::views
