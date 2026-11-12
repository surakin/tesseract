#include "ServerPicker.h"

#include "tk/i18n.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tesseract::views
{

namespace
{
std::string server_lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return out;
}
} // namespace

ServerPicker::ServerPicker()
{
    if (!host())
    {
        return;
    }
    init_(kFieldH, kRowH, kWidth, kMaxRows, tk::tr("Homeserver"));
}

void ServerPicker::set_servers(std::vector<std::string> servers)
{
    servers_ = std::move(servers);
}

std::size_t ServerPicker::entry_count_() const
{
    return servers_.size();
}

// 0 = exact, 1 = starts-with, 2 = substring; the empty query matches all.
int ServerPicker::match_rank_(std::size_t index, std::string_view query) const
{
    if (query.empty())
    {
        return 2;
    }
    const std::string s = server_lower(servers_[index]);
    const std::string q = server_lower(query);
    if (s == q)
    {
        return 0;
    }
    if (s.starts_with(q))
    {
        return 1;
    }
    return s.find(q) != std::string::npos ? 2 : -1;
}

std::string ServerPicker::entry_key_(std::size_t index) const
{
    return servers_[index];
}

std::string ServerPicker::entry_label_(std::size_t index) const
{
    return servers_[index];
}

std::string ServerPicker::entry_display_(std::size_t index) const
{
    return servers_[index];
}

} // namespace tesseract::views
