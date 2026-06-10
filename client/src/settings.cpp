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
    catch (const nlohmann::json::exception&)
    {
        return;
    }

    try
    {

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
    prefetch_unread_rooms        = j.value("prefetch_unread_rooms",          true);
    send_presence                = j.value("send_presence",                true);

    room_section_invites_collapsed   = j.value("room_section_invites_collapsed",   false);
    room_section_favorites_collapsed = j.value("room_section_favorites_collapsed", false);
    room_section_dms_collapsed       = j.value("room_section_dms_collapsed",       false);
    room_section_rooms_collapsed     = j.value("room_section_rooms_collapsed",     false);
    room_section_spaces_collapsed    = j.value("room_section_spaces_collapsed",    false);
    room_section_inactive_collapsed  = j.value("room_section_inactive_collapsed",  true);

    if (j.contains("main_window") && j["main_window"].is_object())
    {
        const auto& mw = j["main_window"];
        main_window_geometry.x     = mw.value("x", 0);
        main_window_geometry.y     = mw.value("y", 0);
        main_window_geometry.w     = mw.value("w", 0);
        main_window_geometry.h     = mw.value("h", 0);
        main_window_geometry.dpi   = mw.value("dpi", 0);
        main_window_geometry.valid = (main_window_geometry.w > 0 && main_window_geometry.h > 0);
    }

    popout_windows.clear();
    if (j.contains("popout_windows") && j["popout_windows"].is_array())
    {
        for (const auto& pw : j["popout_windows"])
        {
            if (!pw.is_object()) continue;
            PopoutEntry e;
            e.room_id      = pw.value("room_id", std::string{});
            e.user_id      = pw.value("user_id", std::string{});
            e.geometry.x   = pw.value("x", 0);
            e.geometry.y   = pw.value("y", 0);
            e.geometry.w   = pw.value("w", 0);
            e.geometry.h   = pw.value("h", 0);
            e.geometry.dpi = pw.value("dpi", 0);
            e.geometry.valid = (!e.room_id.empty() && e.geometry.w > 0 && e.geometry.h > 0);
            if (!e.room_id.empty())
                popout_windows.push_back(std::move(e));
        }
    }

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

    } // try (field reading)
    catch (const nlohmann::json::exception&)
    {
        // A structurally valid file with wrong-typed fields (e.g.
        // "notifications_enabled": "yes") throws json::type_error during
        // field access. Treat it like a parse error and leave defaults in
        // place rather than propagating out of load_from_disk.
        return;
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
        {"prefetch_unread_rooms",            prefetch_unread_rooms},
        {"send_presence",                    send_presence},
        {"room_section_invites_collapsed",   room_section_invites_collapsed},
        {"room_section_favorites_collapsed", room_section_favorites_collapsed},
        {"room_section_dms_collapsed",       room_section_dms_collapsed},
        {"room_section_rooms_collapsed",     room_section_rooms_collapsed},
        {"room_section_spaces_collapsed",    room_section_spaces_collapsed},
        {"room_section_inactive_collapsed",  room_section_inactive_collapsed},
    };
    j["language"]    = language;
    j["gif_api_key"] = gif_api_key;

    if (main_window_geometry.valid)
    {
        j["main_window"] = {
            {"x",   main_window_geometry.x},
            {"y",   main_window_geometry.y},
            {"w",   main_window_geometry.w},
            {"h",   main_window_geometry.h},
            {"dpi", main_window_geometry.dpi},
        };
    }

    {
        nlohmann::json pws = nlohmann::json::array();
        for (const auto& e : popout_windows)
        {
            if (e.room_id.empty()) continue;
            nlohmann::json pw;
            pw["room_id"] = e.room_id;
            pw["user_id"] = e.user_id;
            pw["x"]       = e.geometry.x;
            pw["y"]       = e.geometry.y;
            pw["w"]       = e.geometry.w;
            pw["h"]       = e.geometry.h;
            pw["dpi"]     = e.geometry.dpi;
            pws.push_back(std::move(pw));
        }
        j["popout_windows"] = std::move(pws);
    }

    auto path = config_dir / "app_settings.json";
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        return;
    f << j.dump(4) << '\n';
}

} // namespace tesseract
