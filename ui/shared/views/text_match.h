#pragma once
#include <algorithm>
#include <cctype>
#include <string_view>

namespace tesseract::views
{

/// Case-insensitive (ASCII) substring test. An empty needle always matches.
inline bool contains_ci(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b)
                          {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

} // namespace tesseract::views
