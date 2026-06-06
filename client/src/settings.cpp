#include "tesseract/settings.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace tesseract
{

void Settings::load_from_disk(const std::filesystem::path& config_dir)
{
    auto path = config_dir / "app_settings.json";
    std::ifstream f(path);
    if (!f.is_open())
        return;

    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::parse_error&)
    {
        return;
    }

    auto theme = j.value("theme", std::string("system"));
    if (theme == "light")
        theme_pref = ThemePreference::Light;
    else if (theme == "dark")
        theme_pref = ThemePreference::Dark;
    else
        theme_pref = ThemePreference::System;

    notifications_enabled        = j.value("notifications_enabled",        true);
    notification_image_previews  = j.value("notification_image_previews",  true);
    notification_hide_content    = j.value("notification_hide_content",    false);
    prefetch_full_media          = j.value("prefetch_full_media",          false);
    group_inactive_rooms         = j.value("group_inactive_rooms",         false);
    inactive_room_threshold_days = j.value("inactive_room_threshold_days", 30);
    autoscroll_unread_rooms      = j.value("autoscroll_unread_rooms",       true);
    send_presence                = j.value("send_presence",                true);

    room_section_invites_collapsed   = j.value("room_section_invites_collapsed",   false);
    room_section_favorites_collapsed = j.value("room_section_favorites_collapsed", false);
    room_section_dms_collapsed       = j.value("room_section_dms_collapsed",       false);
    room_section_rooms_collapsed     = j.value("room_section_rooms_collapsed",     false);
    room_section_spaces_collapsed    = j.value("room_section_spaces_collapsed",    false);
    room_section_inactive_collapsed  = j.value("room_section_inactive_collapsed",  true);

    auto lang = j.value("language", std::string{});
    if (!lang.empty())
    {
        language = lang;
    }

    // Only override the (possibly baked-in) default when an explicit non-empty
    // key is present on disk, mirroring the language handling above.
    auto gif_key = j.value("gif_api_key", std::string{});
    if (!gif_key.empty())
    {
        gif_api_key = gif_key;
    }
}

void Settings::save_to_disk(const std::filesystem::path& config_dir) const
{
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);

    const char* theme_str =
        theme_pref == ThemePreference::Light ? "light" :
        theme_pref == ThemePreference::Dark  ? "dark"  : "system";

    nlohmann::json j = {
        {"theme",                            theme_str},
        {"notifications_enabled",            notifications_enabled},
        {"notification_image_previews",      notification_image_previews},
        {"notification_hide_content",        notification_hide_content},
        {"prefetch_full_media",              prefetch_full_media},
        {"group_inactive_rooms",             group_inactive_rooms},
        {"inactive_room_threshold_days",     inactive_room_threshold_days},
        {"autoscroll_unread_rooms",          autoscroll_unread_rooms},
        {"send_presence",                    send_presence},
        {"room_section_invites_collapsed",   room_section_invites_collapsed},
        {"room_section_favorites_collapsed", room_section_favorites_collapsed},
        {"room_section_dms_collapsed",       room_section_dms_collapsed},
        {"room_section_rooms_collapsed",     room_section_rooms_collapsed},
        {"room_section_spaces_collapsed",    room_section_spaces_collapsed},
        {"room_section_inactive_collapsed",  room_section_inactive_collapsed},
    };

    auto path = config_dir / "app_settings.json";
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        return;
    j["language"] = language;
    j["gif_api_key"] = gif_api_key;
    f << j.dump(4) << '\n';
}

} // namespace tesseract
