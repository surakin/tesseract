#include "tesseract/provisioning.h"

#include "tesseract/paths.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>

namespace tesseract
{

namespace
{

bool is_junk(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::optional<std::string> json_string(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return std::nullopt;
    return it->get<std::string>();
}

ProvisionedValues read_user_file()
{
    std::ifstream in(config_dir() / "provisioning.json");
    if (!in)
        return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_provisioning_values(text);
}

const ProvisionedValues& resolved()
{
    static const ProvisionedValues value = [] {
        auto platform = detail::read_platform_provisioning_raw();
        ProvisionedValues normalized;
        if (platform.homeserver)
            normalized.homeserver = normalize_homeserver(*platform.homeserver);
        if (platform.username)
            normalized.username = normalize_username(*platform.username);
        return merge_provisioned(read_user_file(), std::move(normalized));
    }();
    return value;
}

} // namespace

std::optional<std::string> normalize_homeserver(std::string_view raw)
{
    while (!raw.empty() && is_junk(raw.front()))
        raw.remove_prefix(1);
    while (!raw.empty() && (is_junk(raw.back()) || raw.back() == '/'))
        raw.remove_suffix(1);
    if (raw.empty())
        return std::nullopt;
    return std::string(raw);
}

std::optional<std::string> normalize_username(std::string_view raw)
{
    while (!raw.empty() && (is_junk(raw.front()) || raw.front() == '@'))
        raw.remove_prefix(1);
    while (!raw.empty() && is_junk(raw.back()))
        raw.remove_suffix(1);
    if (raw.empty())
        return std::nullopt;
    return std::string(raw);
}

std::string homeserver_server_name(std::string_view homeserver)
{
    if (auto scheme = homeserver.find("://"); scheme != std::string_view::npos)
        homeserver.remove_prefix(scheme + 3);
    if (auto slash = homeserver.find('/'); slash != std::string_view::npos)
        homeserver = homeserver.substr(0, slash);
    return std::string(homeserver);
}

std::optional<std::string> make_user_id(std::string_view username, std::string_view homeserver)
{
    auto user = normalize_username(username);
    if (!user)
        return std::nullopt;
    if (user->find(':') != std::string::npos)
        return "@" + *user;
    auto server = homeserver_server_name(homeserver);
    if (server.empty())
        return std::nullopt;
    return "@" + *user + ":" + server;
}

ProvisionedValues parse_provisioning_values(std::string_view json_text)
{
    auto j = nlohmann::json::parse(json_text.begin(), json_text.end(), nullptr, false);
    if (!j.is_object())
        return {};
    ProvisionedValues out;
    if (auto hs = json_string(j, "default_homeserver"))
        out.homeserver = normalize_homeserver(*hs);
    if (auto user = json_string(j, "default_username"))
        out.username = normalize_username(*user);
    return out;
}

std::optional<std::string> parse_provisioning_json(std::string_view json_text)
{
    return parse_provisioning_values(json_text).homeserver;
}

ProvisionedValues merge_provisioned(ProvisionedValues preferred, ProvisionedValues fallback)
{
    return {preferred.homeserver ? std::move(preferred.homeserver) : std::move(fallback.homeserver),
            preferred.username ? std::move(preferred.username) : std::move(fallback.username)};
}

std::optional<std::string> provisioned_default_homeserver()
{
    return resolved().homeserver;
}

std::optional<std::string> provisioned_default_username()
{
    return resolved().username;
}

std::optional<std::string> provisioned_user_id()
{
    const auto& v = resolved();
    if (!v.username)
        return std::nullopt;
    return make_user_id(*v.username, v.homeserver.value_or(""));
}

bool login_provisioned()
{
    const auto& v = resolved();
    return v.homeserver.has_value() || v.username.has_value();
}

} // namespace tesseract
