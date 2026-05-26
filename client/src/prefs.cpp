#include "tesseract/prefs.h"
#include "json_util.h"

#include <string>

namespace tesseract
{
namespace Prefs
{

// Minimal extractor for a single string value by key from a flat JSON object.
// Handles {"last_room":"!id:host"} — room IDs never contain '"' or '\'.
static std::string extract_string(const std::string& json,
                                  const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos)
    {
        return {};
    }
    pos += needle.size();
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"')
    {
        return {};
    }
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string::npos)
    {
        return {};
    }
    return json.substr(pos, end - pos);
}

PrefsData parse(const std::string& json)
{
    PrefsData p;
    p.last_room = extract_string(json, "last_room");
    return p;
}


std::string serialize(const PrefsData& p)
{
    return "{\"last_room\":\"" + json_escape(p.last_room) + "\"}";
}

} // namespace Prefs
} // namespace tesseract
