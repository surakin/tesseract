#include "app/SearchBackend.h"

#include "views/text_util.h"

#include <algorithm>
#include <cctype>

namespace tesseract
{

namespace
{
bool ci_less(const std::string& a, const std::string& b)
{
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(),
        [](char x, char y)
        {
            return std::tolower(static_cast<unsigned char>(x)) <
                   std::tolower(static_cast<unsigned char>(y));
        });
}
} // namespace

SearchBackend::Handle SearchBackend::register_shell(ShellRegistration reg)
{
    const Handle h = next_handle_++;
    shells_.emplace(h, std::move(reg));
    return h;
}

void SearchBackend::unregister_shell(Handle h)
{
    shells_.erase(h);
}

std::vector<SearchBackend::Result>
SearchBackend::query(const std::string& term, std::size_t max_results) const
{
    std::vector<Result> out;
    for (const auto& [handle, reg] : shells_)
    {
        if (reg.rooms)
        {
            for (const auto& r : reg.rooms())
            {
                const std::string& title =
                    r.name.empty()
                        ? (r.canonical_alias.empty() ? r.id : r.canonical_alias)
                        : r.name;
                if (!text::name_matches(title, term) &&
                    !text::name_matches(r.topic, term) &&
                    !text::name_matches(r.canonical_alias, term))
                    continue;
                Result res;
                res.kind = ResultKind::Room;
                res.id = r.id;
                res.title = title;
                res.subtitle = r.topic.empty() ? r.canonical_alias : r.topic;
                res.has_unread = r.unread_count > 0 && !r.muted;
                out.push_back(std::move(res));
            }
        }
        if (reg.known_users)
        {
            for (const auto& m : reg.known_users())
            {
                const std::string& title =
                    m.display_name.empty() ? m.user_id : m.display_name;
                if (!text::name_matches(title, term) &&
                    !text::name_matches(m.user_id, term))
                    continue;
                Result res;
                res.kind = ResultKind::Contact;
                res.id = m.user_id;
                res.title = title;
                res.subtitle = m.user_id;
                out.push_back(std::move(res));
            }
        }
    }

    // Rooms ranked ahead of contacts; alphabetical within each group. Cross-
    // shell recency ordering (last_activity_ts) would need a merge across
    // providers, which isn't worth the complexity for a capped result list.
    std::stable_sort(out.begin(), out.end(),
                      [](const Result& a, const Result& b)
                      {
                          if (a.kind != b.kind)
                              return a.kind == ResultKind::Room;
                          return ci_less(a.title, b.title);
                      });
    if (out.size() > max_results)
        out.resize(max_results);
    return out;
}

void SearchBackend::activate(const std::string& id, ResultKind kind) const
{
    const bool is_room = kind == ResultKind::Room;
    for (const auto& [handle, reg] : shells_)
    {
        if (is_room)
        {
            if (!reg.rooms || !reg.activate_room)
                continue;
            const auto rooms = reg.rooms();
            const bool found =
                std::any_of(rooms.begin(), rooms.end(),
                            [&](const tesseract::RoomInfo& r)
                            { return r.id == id; });
            if (found)
            {
                reg.activate_room(id);
                return;
            }
        }
        else
        {
            if (!reg.known_users || !reg.activate_contact)
                continue;
            const auto users = reg.known_users();
            const bool found =
                std::any_of(users.begin(), users.end(),
                            [&](const tesseract::RoomMember& m)
                            { return m.user_id == id; });
            if (found)
            {
                reg.activate_contact(id);
                return;
            }
        }
    }
}

} // namespace tesseract
