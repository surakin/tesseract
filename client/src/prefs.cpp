#include "tesseract/prefs.h"

#include <cstdio>
#include <string>

namespace tesseract {
namespace Prefs {

// Minimal extractor for a single string value by key from a flat JSON object.
// Handles {"last_room":"!id:host"} — room IDs never contain '"' or '\'.
static std::string extract_string(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
        ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

PrefsData parse(const std::string& json) {
    PrefsData p;
    p.last_room = extract_string(json, "last_room");
    return p;
}

// Defensive JSON string escape. A room ID is normally free of `"`/`\`, but
// a malformed value must not be able to emit invalid JSON (which would lose
// the whole prefs object on the next parse).
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[7];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string serialize(const PrefsData& p) {
    return "{\"last_room\":\"" + json_escape(p.last_room) + "\"}";
}

} // namespace Prefs
} // namespace tesseract
