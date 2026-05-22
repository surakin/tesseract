#include "views/MentionEngine.h"

#include <algorithm>
#include <cctype>

namespace tesseract::views
{
namespace
{

bool is_mention_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '-' || c == '.';
}

bool icontains(std::string_view hay, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (hay.size() < needle.size())
    {
        return false;
    }
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
        {
            return true;
        }
    }
    return false;
}

// Rank: 0 = exact, 1 = starts-with, 2 = substring, 3 = no match.
int match_rank(std::string_view hay, std::string_view needle)
{
    if (needle.empty())
    {
        return 2;
    }
    auto ieq = [](char a, char b)
    {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    };
    if (hay.size() == needle.size() &&
        std::equal(hay.begin(), hay.end(), needle.begin(), ieq))
    {
        return 0;
    }
    if (hay.size() >= needle.size() &&
        std::equal(needle.begin(), needle.end(), hay.begin(), ieq))
    {
        return 1;
    }
    return icontains(hay, needle) ? 2 : 3;
}

// Localpart of a Matrix user id: "@name:server" -> "name".
std::string_view localpart(std::string_view user_id)
{
    std::string_view s = user_id;
    if (!s.empty() && s.front() == '@')
    {
        s.remove_prefix(1);
    }
    auto colon = s.find(':');
    if (colon != std::string_view::npos)
    {
        s = s.substr(0, colon);
    }
    return s;
}

} // namespace

std::optional<MentionMatch>
MentionEngine::find_prefix(std::string_view text, int cursor) const
{
    cursor = std::min(cursor, (int)text.size());
    if (cursor < 0)
    {
        return std::nullopt;
    }
    int pos = cursor;
    while (pos > 0 && is_mention_char(text[pos - 1]))
    {
        --pos;
    }
    if (pos == 0 || text[pos - 1] != '@')
    {
        return std::nullopt;
    }
    int at = pos - 1;
    // The '@' must start the text or follow whitespace (avoid e-mail addrs).
    if (at > 0)
    {
        char b = text[at - 1];
        if (b != ' ' && b != '\t' && b != '\n')
        {
            return std::nullopt;
        }
    }
    std::string prefix(text.substr(pos, cursor - pos));
    return MentionMatch{at, cursor, std::move(prefix)};
}

std::vector<MentionCandidate>
MentionEngine::lookup(std::string_view prefix,
                      const std::vector<tesseract::RoomMember>& members,
                      int max_results, bool include_room) const
{
    using Ranked = std::pair<int, MentionCandidate>;
    std::vector<Ranked> ranked;
    ranked.reserve(members.size());

    for (const auto& m : members)
    {
        int rn = std::min(match_rank(m.display_name, prefix),
                          match_rank(localpart(m.user_id), prefix));
        if (rn >= 3)
        {
            continue;
        }
        MentionCandidate c;
        c.user_id = m.user_id;
        c.display_name =
            m.display_name.empty() ? m.user_id : m.display_name;
        c.avatar_url = m.avatar_url;
        ranked.emplace_back(rn, std::move(c));
    }

    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& a, const Ranked& b)
                     { return a.first < b.first; });

    std::vector<MentionCandidate> out;
    if (include_room && (prefix.empty() || icontains("room", prefix)))
    {
        MentionCandidate room;
        room.display_name = "@room";
        room.is_room = true;
        out.push_back(std::move(room));
    }
    for (auto& [r, c] : ranked)
    {
        if ((int)out.size() >= max_results)
        {
            break;
        }
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace tesseract::views
