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

// Case-insensitive substring match (byte-level ASCII approximation).
inline bool name_matches(const std::string& name, const std::string& query)
{
    if (query.empty())
        return true;
    if (name.size() < query.size())
        return false;
    auto to_lower = [](char c)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + query.size() <= name.size(); ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < query.size(); ++j)
        {
            if (to_lower(name[i + j]) != to_lower(query[j]))
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

} // namespace tesseract::text
