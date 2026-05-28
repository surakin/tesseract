#include "tesseract/prefs.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace tesseract
{
namespace Prefs
{

PrefsData parse(const std::string& json_str)
{
    PrefsData p;
    try
    {
        auto j    = nlohmann::json::parse(json_str);
        p.last_room  = j.value("last_room", std::string{});
        p.open_rooms = j.value("open_rooms", std::vector<std::string>{});
    }
    catch (...)
    {
    }
    // Backward compat: old prefs have last_room but no open_rooms array.
    if (p.open_rooms.empty() && !p.last_room.empty())
        p.open_rooms.push_back(p.last_room);
    return p;
}

std::string serialize(const PrefsData& p)
{
    nlohmann::json j;
    j["last_room"]  = p.last_room;
    j["open_rooms"] = p.open_rooms;
    return j.dump();
}

} // namespace Prefs
} // namespace tesseract
