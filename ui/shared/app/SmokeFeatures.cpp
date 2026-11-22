#include "app/SmokeFeatures.h"

#include <map>

namespace tesseract::smoke
{

namespace
{

const std::map<std::string, EventType>& event_type_names()
{
    static const std::map<std::string, EventType> names = {
        {"Text", EventType::Text},
        {"Image", EventType::Image},
        {"File", EventType::File},
        {"Sticker", EventType::Sticker},
        {"Audio", EventType::Audio},
        {"Voice", EventType::Voice},
        {"Video", EventType::Video},
        {"Redacted", EventType::Redacted},
        {"Notice", EventType::Notice},
        {"Emote", EventType::Emote},
        {"Unhandled", EventType::Unhandled},
        {"DaySeparator", EventType::DaySeparator},
        {"ReadMarker", EventType::ReadMarker},
        {"TimelineStart", EventType::TimelineStart},
        {"Location", EventType::Location},
        {"Utd", EventType::Utd},
        {"PinnedEvent", EventType::PinnedEvent},
        {"CallNotification", EventType::CallNotification},
        {"Membership", EventType::Membership},
        {"RoomName", EventType::RoomName},
        {"RoomTombstone", EventType::RoomTombstone},
        {"Poll", EventType::Poll},
        {"Gallery", EventType::Gallery},
        {"DevUnhandled", EventType::DevUnhandled},
        {"StateEvent", EventType::StateEvent},
        {"RoomTopic", EventType::RoomTopic},
    };
    return names;
}

std::string event_type_name(EventType t)
{
    for (const auto& [name, value] : event_type_names())
    {
        if (value == t)
        {
            return name;
        }
    }
    return "?";
}

std::string want_got(const std::string& key, const std::string& want, const std::string& got)
{
    return key + ": want " + want + ", got " + got;
}

std::string str_check(const std::string& key, const nlohmann::json& want, const std::string& got)
{
    if (!want.is_string())
    {
        return key + ": expected a string in the manifest";
    }
    if (want.get<std::string>() == got)
    {
        return {};
    }
    return want_got(key, want.get<std::string>(), got);
}

std::string bool_check(const std::string& key, const nlohmann::json& want, bool got)
{
    if (!want.is_boolean())
    {
        return key + ": expected a bool in the manifest";
    }
    if (want.get<bool>() == got)
    {
        return {};
    }
    return want_got(key, want.get<bool>() ? "true" : "false", got ? "true" : "false");
}

std::string uint_check(const std::string& key, const nlohmann::json& want, uint64_t got)
{
    if (!want.is_number_unsigned() && !(want.is_number_integer() && want.get<long long>() >= 0))
    {
        return key + ": expected a non-negative integer in the manifest";
    }
    const auto w = want.get<uint64_t>();
    if (w == got)
    {
        return {};
    }
    return want_got(key, std::to_string(w), std::to_string(got));
}

std::string reactions_check(const nlohmann::json& want, const std::vector<Reaction>& got)
{
    if (!want.is_array())
    {
        return "reactions: expected an array in the manifest";
    }
    for (const auto& r : want)
    {
        if (!r.is_object() || !r.contains("key") || !r["key"].is_string())
        {
            return "reactions: each entry needs a string 'key'";
        }
        const auto key = r["key"].get<std::string>();
        const Reaction* found = nullptr;
        for (const auto& g : got)
        {
            if (g.key == key)
            {
                found = &g;
            }
        }
        if (!found)
        {
            return "reactions: want key '" + key + "', not present";
        }
        if (r.contains("count"))
        {
            const auto m = uint_check("reactions['" + key + "'].count", r["count"], found->count);
            if (!m.empty())
            {
                return m;
            }
        }
    }
    return {};
}

} // namespace

std::vector<FeatureCheck> parse_feature_manifest(const std::string& json_text, std::string& error)
{
    const auto j = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        error = "manifest is not a JSON object";
        return {};
    }
    const auto it = j.find("checks");
    if (it == j.end() || !it->is_array())
    {
        error = "manifest has no 'checks' array";
        return {};
    }
    if (it->empty())
    {
        error = "manifest has an empty 'checks' array";
        return {};
    }
    std::vector<FeatureCheck> out;
    for (const auto& c : *it)
    {
        if (!c.is_object() || !c.contains("name") || !c["name"].is_string() ||
            !c.contains("kind") || !c["kind"].is_string())
        {
            error = "every check needs a string 'name' and 'kind'";
            return {};
        }
        FeatureCheck fc;
        fc.name = c["name"].get<std::string>();
        fc.kind = c["kind"].get<std::string>();
        if (fc.kind != "event" && fc.kind != "room" && fc.kind != "action" && fc.kind != "order")
        {
            error = "unknown check kind '" + fc.kind + "' in check '" + fc.name + "'";
            return {};
        }
        fc.args = c;
        out.push_back(std::move(fc));
    }
    return out;
}

std::string manifest_features_room_id(const std::string& json_text)
{
    const auto j = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        return {};
    }
    const auto it = j.find("features_room_id");
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string absent_mismatch(bool recorded)
{
    return recorded ? "an event expected to be absent reached the timeline" : "";
}

std::string event_mismatch(const Event& ev, const nlohmann::json& expect)
{
    if (!expect.is_object())
    {
        return "expect is not an object";
    }
    for (const auto& [key, want] : expect.items())
    {
        std::string m;
        if (key == "type")
        {
            if (!want.is_string())
            {
                m = "type: expected a string in the manifest";
            }
            else if (!event_type_names().count(want.get<std::string>()))
            {
                m = "type: unknown EventType '" + want.get<std::string>() + "'";
            }
            else
            {
                m = str_check(key, want, event_type_name(ev.type));
            }
        }
        else if (key == "sender")
        {
            m = str_check(key, want, ev.sender);
        }
        else if (key == "body")
        {
            m = str_check(key, want, ev.body);
        }
        else if (key == "is_edited")
        {
            m = bool_check(key, want, ev.is_edited);
        }
        else if (key == "in_reply_to_id")
        {
            m = str_check(key, want, ev.in_reply_to_id);
        }
        else if (key == "forwarded")
        {
            m = bool_check(key, want, ev.forwarded);
        }
        else if (key == "bot_command")
        {
            m = str_check(key, want, ev.bot_command);
        }
        else if (key == "thread_reply_count")
        {
            m = uint_check(key, want, ev.thread_reply_count);
        }
        else if (key == "thread_latest_body")
        {
            m = str_check(key, want, ev.thread_latest_body);
        }
        else if (key == "reactions")
        {
            m = reactions_check(want, ev.reactions);
        }
        else
        {
            m = "unknown key '" + key + "'";
        }
        if (!m.empty())
        {
            return m;
        }
    }
    return {};
}

std::string room_mismatch(const RoomInfo& room, const nlohmann::json& expect)
{
    if (!expect.is_object())
    {
        return "expect is not an object";
    }
    for (const auto& [key, want] : expect.items())
    {
        std::string m;
        if (key == "last_message_kind")
        {
            m = str_check(key, want, room.last_message_kind);
        }
        else if (key == "last_message_body")
        {
            m = str_check(key, want, room.last_message_body);
        }
        else if (key == "unread_count")
        {
            m = uint_check(key, want, room.unread_count);
        }
        else if (key == "notification_count")
        {
            m = uint_check(key, want, room.notification_count);
        }
        else if (key == "highlight_count")
        {
            m = uint_check(key, want, room.highlight_count);
        }
        else if (key == "is_favorite")
        {
            m = bool_check(key, want, room.is_favorite);
        }
        else if (key == "is_direct")
        {
            m = bool_check(key, want, room.is_direct);
        }
        else if (key == "replaced")
        {
            m = bool_check(key, want, !room.successor_room_id.empty());
        }
        else
        {
            m = "unknown key '" + key + "'";
        }
        if (!m.empty())
        {
            return m;
        }
    }
    return {};
}

} // namespace tesseract::smoke
