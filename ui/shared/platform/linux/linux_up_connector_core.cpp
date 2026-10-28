#include "platform/linux/linux_up_connector_core.h"

#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>

namespace tesseract::up
{

std::string sanitize_token(const std::string& user_id)
{
    std::string t;
    t.reserve(user_id.size());
    for (char c : user_id)
    {
        t += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return t;
}

namespace
{

// Bracketed IPv6 literal (hex digits, ':' and '.' only, at least one ':'), or
// a reg-name / IPv4 host of [A-Za-z0-9._-] with no empty label and no leading
// '-' or '.'.
bool is_valid_gateway_host(const std::string& host)
{
    if (host.front() == '[')
    {
        if (host.size() < 3 || host.back() != ']')
        {
            return false;
        }
        bool colon = false;
        for (std::size_t i = 1; i + 1 < host.size(); ++i)
        {
            const char c = host[i];
            colon = colon || c == ':';
            if (c != ':' && c != '.' &&
                !std::isxdigit(static_cast<unsigned char>(c)))
            {
                return false;
            }
        }
        return colon;
    }
    if (host.front() == '-' || host.front() == '.' ||
        host.find("..") != std::string::npos)
    {
        return false;
    }
    for (char c : host)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' &&
            c != '-' && c != '_')
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<std::string> normalize_gateway_url(const std::string& endpoint)
{
    for (char c : endpoint)
    {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f || c == '\\')
        {
            return std::nullopt;
        }
    }

    constexpr std::string_view kScheme = "https://";
    if (endpoint.size() < kScheme.size())
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kScheme.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(endpoint[i])) != kScheme[i])
        {
            return std::nullopt;
        }
    }

    const std::size_t auth_begin = kScheme.size();
    const std::size_t auth_end = endpoint.find_first_of("/?#", auth_begin);
    const std::string authority = endpoint.substr(
        auth_begin,
        auth_end == std::string::npos ? std::string::npos : auth_end - auth_begin);
    if (authority.empty() || authority.find('@') != std::string::npos)
    {
        return std::nullopt;
    }

    // Split host from an optional :port (bracketed IPv6 literals keep their
    // inner colons).
    std::string host = authority;
    std::string port;
    if (authority.front() == '[')
    {
        const auto close = authority.find(']');
        if (close == std::string::npos)
        {
            return std::nullopt;
        }
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size())
        {
            if (authority[close + 1] != ':')
            {
                return std::nullopt;
            }
            port = authority.substr(close + 2);
        }
        if (host.size() <= 2)
        {
            return std::nullopt;
        }
    }
    else if (const auto colon = authority.find(':'); colon != std::string::npos)
    {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.empty() || !is_valid_gateway_host(host))
    {
        return std::nullopt;
    }
    if (authority.find(':', host.size()) != std::string::npos)
    {
        // A port was given; it must be 1-5 digits in 1..65535.
        if (port.empty() || port.size() > 5)
        {
            return std::nullopt;
        }
        int value = 0;
        for (char c : port)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
        }
        if (value < 1 || value > 65535)
        {
            return std::nullopt;
        }
    }

    // Matrix HTTP pushers require the URL path to be /_matrix/push/v1/notify.
    // By UP convention the push provider exposes a Matrix gateway at that path.
    return "https://" + authority + "/_matrix/push/v1/notify";
}

std::optional<std::string> extract_push_room_id(std::string_view payload)
{
    const auto doc = nlohmann::json::parse(payload.begin(), payload.end(),
                                           nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object())
    {
        return std::nullopt;
    }
    const auto note = doc.find("notification");
    if (note == doc.end() || !note->is_object())
    {
        return std::nullopt;
    }
    const auto room = note->find("room_id");
    if (room == note->end() || !room->is_string())
    {
        return std::nullopt;
    }
    auto id = room->get<std::string>();
    if (id.empty())
    {
        return std::nullopt;
    }
    return id;
}

bool PusherCore::begin(tesseract::Client* client, const std::string& user_id)
{
    if (client_)
    {
        return false; // already started
    }
    client_ = client;
    token_ = sanitize_token(user_id);
    // Honour the persisted Notifications toggle on startup so a user who
    // disabled push isn't silently re-registered every launch.
    enabled_ = tesseract::Settings::instance().notifications_enabled;
    return true;
}

void PusherCore::on_new_endpoint(const std::string& endpoint)
{
    if (!client_)
    {
        return;
    }
    auto gateway = normalize_gateway_url(endpoint);
    if (!gateway)
    {
        // Log scheme + authority only: never the path or query string.
        const auto end = endpoint.find_first_of("/?#", endpoint.find("://") == std::string::npos
                                                           ? 0
                                                           : endpoint.find("://") + 3);
        std::fprintf(stderr, "[up] rejected push endpoint: %.100s\n",
                     endpoint.substr(0, end).c_str());
        return;
    }
    gateway_url_ = std::move(*gateway);
    if (!enabled_)
    {
        return; // user disabled notifications; keep the endpoint cached
    }
    client_->register_pusher(token_, kPusherAppId, kPusherAppName,
                             kPusherDeviceName, gateway_url_, kPusherLang);
}

void PusherCore::set_enabled(bool enabled)
{
    if (enabled_ == enabled)
    {
        return;
    }
    enabled_ = enabled;
    if (!client_)
    {
        return; // not started yet; honour the flag when begin() runs
    }
    if (enabled)
    {
        if (!gateway_url_.empty())
        {
            client_->register_pusher(token_, kPusherAppId, kPusherAppName,
                                     kPusherDeviceName, gateway_url_,
                                     kPusherLang);
        }
    }
    else
    {
        // remove_pusher is idempotent on the homeserver, so calling it when
        // no pusher exists is harmless.
        client_->remove_pusher(token_, kPusherAppId);
    }
}

void PusherCore::remove_pusher()
{
    if (!client_)
    {
        return;
    }
    client_->remove_pusher(token_, kPusherAppId);
}

void PusherCore::on_message(std::string_view payload)
{
    if (!client_)
    {
        return;
    }
    if (auto room = extract_push_room_id(payload))
    {
        client_->hint_push_room(*room);
    }
}

} // namespace tesseract::up
