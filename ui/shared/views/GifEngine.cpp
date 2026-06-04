#include "views/GifEngine.h"

namespace tesseract::views
{

namespace
{
constexpr std::string_view kPrefix = "/gif ";

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
} // namespace

std::optional<std::string> GifEngine::match(std::string_view text) const
{
    if (text.size() < kPrefix.size() || text.substr(0, kPrefix.size()) != kPrefix)
    {
        return std::nullopt;
    }
    std::string_view rest = text.substr(kPrefix.size());

    // Trim surrounding whitespace; a query that is all whitespace is "no query
    // yet" (the strip stays hidden until the user types something).
    std::size_t b = 0;
    std::size_t e = rest.size();
    while (b < e && is_space(rest[b]))
    {
        ++b;
    }
    while (e > b && is_space(rest[e - 1]))
    {
        --e;
    }
    if (b == e)
    {
        return std::nullopt;
    }
    return std::string(rest.substr(b, e - b));
}

} // namespace tesseract::views
