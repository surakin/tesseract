#include "tesseract/prefs.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace tesseract
{
namespace Prefs
{

PrefsData room_layout(const std::string& last_room, const std::vector<std::string>& open_room_ids)
{
    PrefsData p;
    p.last_room = last_room;
    p.open_rooms = open_room_ids;
    // Always round-trip: with no open tabs, the active room is the layout.
    if (p.open_rooms.empty() && !last_room.empty())
    {
        p.open_rooms.push_back(last_room);
    }
    return p;
}

PrefsData without_layout(PrefsData p)
{
    p.last_room.clear();
    p.open_rooms.clear();
    p.recent_rooms.clear();
    return p;
}

PrefsData parse(const std::string& json_str)
{
    PrefsData p;
    try
    {
        auto j = nlohmann::json::parse(json_str);
        p.last_room = j.value("last_room", std::string{});
        p.open_rooms = j.value("open_rooms", std::vector<std::string>{});
        p.bridge_not_bridged_overrides = j.value("bridge_overrides", std::vector<std::string>{});
        p.emoji_skin_tone = j.value("emoji_skin_tone", std::string{});
        p.recent_rooms = j.value("recent_rooms", std::vector<std::string>{});
        p.directory_servers = j.value("directory_servers", std::vector<std::string>{});
        p.pinned_spaces = j.value("pinned_spaces", std::vector<std::string>{});
        if (p.directory_servers.size() > kMaxDirectoryServers)
        {
            p.directory_servers.resize(kMaxDirectoryServers);
        }
    }
    catch (...)
    {
    }
    // Backward compat: old prefs have last_room but no open_rooms array.
    if (p.open_rooms.empty() && !p.last_room.empty())
    {
        p.open_rooms.push_back(p.last_room);
    }
    return p;
}

std::string serialize(const PrefsData& p, const std::string& base_json)
{
    nlohmann::json j = nlohmann::json::object();
    if (!base_json.empty())
    {
        auto base = nlohmann::json::parse(base_json, nullptr, /*allow_exceptions=*/false);
        if (base.is_object())
        {
            j = std::move(base);
        }
    }
    j["last_room"] = p.last_room;
    j["open_rooms"] = p.open_rooms;
    if (p.bridge_not_bridged_overrides.empty())
    {
        j.erase("bridge_overrides");
    }
    else
    {
        j["bridge_overrides"] = p.bridge_not_bridged_overrides;
    }
    if (p.emoji_skin_tone.empty())
    {
        j.erase("emoji_skin_tone");
    }
    else
    {
        j["emoji_skin_tone"] = p.emoji_skin_tone;
    }
    if (p.recent_rooms.empty())
    {
        j.erase("recent_rooms");
    }
    else
    {
        j["recent_rooms"] = p.recent_rooms;
    }
    if (p.directory_servers.empty())
    {
        j.erase("directory_servers");
    }
    else
    {
        j["directory_servers"] = p.directory_servers;
    }
    if (p.pinned_spaces.empty())
    {
        j.erase("pinned_spaces");
    }
    else
    {
        j["pinned_spaces"] = p.pinned_spaces;
    }
    return j.dump();
}

namespace
{
bool same_server(const std::string& a, const std::string& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char x, char y)
                      {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

void push_unique(std::vector<std::string>& list, const std::string& server)
{
    if (server.empty())
    {
        return;
    }
    for (const auto& s : list)
    {
        if (same_server(s, server))
        {
            return;
        }
    }
    list.push_back(server);
}
} // namespace

std::vector<std::string> add_directory_server(std::vector<std::string> saved,
                                              const std::string& server)
{
    if (server.empty())
    {
        return saved;
    }
    saved.erase(std::remove_if(saved.begin(), saved.end(),
                               [&](const std::string& s)
                               {
                                   return same_server(s, server);
                               }),
                saved.end());
    saved.insert(saved.begin(), server);
    if (saved.size() > kMaxDirectoryServers)
    {
        saved.resize(kMaxDirectoryServers);
    }
    return saved;
}

std::vector<std::string> directory_server_choices(const std::vector<std::string>& saved,
                                                  const std::string& homeserver)
{
    std::vector<std::string> out;
    for (const auto& s : saved)
    {
        push_unique(out, s);
    }
    push_unique(out, homeserver);
    push_unique(out, "activematrixrooms.com");
    push_unique(out, "matrixrooms.info");
    if (out.size() > kMaxDirectoryServers)
    {
        out.resize(kMaxDirectoryServers);
    }
    return out;
}

} // namespace Prefs
} // namespace tesseract
