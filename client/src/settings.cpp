#include "tesseract/settings.h"

#include "fs_util.h"
#include "tesseract/secret_store.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace tesseract
{

ProxyUrlParts split_proxy_credentials(const std::string& url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string::npos)
        return {url, {}};
    const auto auth_start = scheme + 3;
    const auto auth_end = url.find_first_of("/?#", auth_start);
    const auto authority = url.substr(
        auth_start, auth_end == std::string::npos ? std::string::npos : auth_end - auth_start);
    const auto at = authority.rfind('@');
    if (at == std::string::npos)
        return {url, {}};
    return {url.substr(0, auth_start) + url.substr(auth_start + at + 1), authority.substr(0, at)};
}

std::string join_proxy_credentials(const std::string& url, const std::string& userinfo)
{
    const auto scheme = url.find("://");
    if (userinfo.empty() || scheme == std::string::npos)
        return url;
    return url.substr(0, scheme + 3) + userinfo + "@" + url.substr(scheme + 3);
}

namespace
{

// Parses `p` as a JSON object. False when it can't be opened or isn't one.
bool read_json_object(const std::filesystem::path& p, nlohmann::json& out)
{
    std::ifstream f(p);
    if (!f.is_open())
        return false;
    try
    {
        f >> out;
    }
    catch (const nlohmann::json::exception&)
    {
        return false;
    }
    return out.is_object();
}

} // namespace

void Settings::load_from_disk(const std::filesystem::path& config_dir)
{
    auto path = config_dir / "app_settings.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return; // first run

    nlohmann::json j;
    if (!read_json_object(path, j))
    {
        // Unreadable or not a JSON object. Never leave it where the next save
        // would overwrite it with defaults: move it aside, then fall back to
        // the last good copy kept by save_to_disk(), else to the defaults.
        auto corrupt = path;
        corrupt += ".corrupt";
        std::error_code mv;
        std::filesystem::rename(path, corrupt, mv);
        std::fprintf(stderr, "tesseract: %s is corrupt; moved to %s\n",
                     path.string().c_str(), corrupt.string().c_str());
        auto last_good = path;
        last_good += ".last-good";
        if (!read_json_object(last_good, j))
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

    // Missing key (settings predating this field) defaults to System, not
    // Blue -- Windows already always matched the OS accent color before
    // this field existed, so System is the value that preserves that
    // existing behavior unchanged. Unrecognized garbage also falls back to
    // System for the same reason, rather than to a specific fixed color.
    auto theme_accent_str = j.value("theme_accent", std::string("system"));
    if (theme_accent_str == "blue")
        theme_accent = ThemeAccent::Blue;
    else if (theme_accent_str == "forest")
        theme_accent = ThemeAccent::Forest;
    else if (theme_accent_str == "sunset")
        theme_accent = ThemeAccent::Sunset;
    else if (theme_accent_str == "violet")
        theme_accent = ThemeAccent::Violet;
    else
        theme_accent = ThemeAccent::System;

    auto low_power = j.value("low_power", std::string("auto"));
    if (low_power == "on")
        low_power_pref = LowPowerPreference::On;
    else if (low_power == "off")
        low_power_pref = LowPowerPreference::Off;
    else
        low_power_pref = LowPowerPreference::Auto;

    notifications_enabled        = j.value("notifications_enabled",        true);
    notification_image_previews  = j.value("notification_image_previews",  true);
    notification_hide_content    = j.value("notification_hide_content",    false);
    prefetch_full_media          = j.value("prefetch_full_media",          false);
    recompress_images_on_send    = j.value("recompress_images_on_send",    true);
    open_media_externally        = j.value("open_media_externally",        false);
    send_maps_urls_as_location  = j.value("send_maps_urls_as_location",   false);
    send_bundled_url_previews    = j.value("send_bundled_url_previews",    true);
    fetch_url_previews_directly  = j.value("fetch_url_previews_directly",  false);
    exclude_insecure_devices     = j.value("exclude_insecure_devices",     false);
    block_sends_on_unverified_devices = j.value("block_sends_on_unverified_devices", true);
    {
        auto pm = j.value("proxy_mode", std::string("system"));
        proxy_mode = pm == "none"   ? ProxyMode::None
                   : pm == "manual" ? ProxyMode::Manual
                                    : ProxyMode::System;
        proxy_url = j.value("proxy_url", std::string());
        proxy_creds_in_keyring = false;
        proxy_creds_unreadable = false;
        proxy_creds_saved.clear();
        // The credential was moved to the secret store when it was saved; put
        // it back so the rest of the app sees one URL.
        if (j.value("proxy_credentials", std::string()) == "keyring" &&
            split_proxy_credentials(proxy_url).userinfo.empty())
        {
            bool failed = false;
            if (auto userinfo = SecretStore::load_proxy_credentials(&failed))
            {
                proxy_url = join_proxy_credentials(proxy_url, *userinfo);
                proxy_creds_saved = std::move(*userinfo);
            }
            else if (failed)
            {
                // Don't treat an unreadable keyring as "no credential": a later
                // save must neither delete the secret nor drop the marker.
                proxy_creds_unreadable = true;
            }
            proxy_creds_in_keyring = true;
        }
    }
    group_inactive_rooms         = j.value("group_inactive_rooms",         false);
    group_unread_rooms            = j.value("group_unread_rooms",            false);
    inactive_room_threshold_days = j.value("inactive_room_threshold_days", 30);
    autoscroll_unread_rooms      = j.value("autoscroll_unread_rooms",       true);
    prefetch_unread_rooms        = j.value("prefetch_unread_rooms",          true);
    launch_at_login               = j.value("launch_at_login",              false);

    close_action                  = close_action_from_string(
        j.value("close_action", std::string(close_action_to_string(CloseAction::HideToTray))));
    start_minimized               = j.value("start_minimized",             false);

    send_presence                = j.value("send_presence",                true);
    index_messages_for_search    = j.value("index_messages_for_search",   false);
    check_for_updates            = j.value("check_for_updates",            true);
    show_room_join_leave_events  = j.value("show_room_join_leave_events", false);
    show_sender_status_in_timeline = j.value("show_sender_status_in_timeline", false);
    show_sender_timezone_in_timeline = j.value("show_sender_timezone_in_timeline", false);
    auto_detect_timezone = j.value("auto_detect_timezone", false);
    animate_avatars              = j.value("animate_avatars",              true);
    msc2545_legacy_compat        = j.value("msc2545_legacy_compat",        true);
    developer_mode               = j.value("developer_mode",              false);
    crash_reporting_enabled      = j.value("crash_reporting_enabled",     false);

    audio_input_device_id  = j.value("audio_input_device_id",  std::string{});
    audio_output_device_id = j.value("audio_output_device_id", std::string{});
    camera_device_id       = j.value("camera_device_id",       std::string{});

    room_section_invites_collapsed   = j.value("room_section_invites_collapsed",   false);
    room_section_favorites_collapsed = j.value("room_section_favorites_collapsed", false);
    room_section_dms_collapsed       = j.value("room_section_dms_collapsed",       false);
    room_section_rooms_collapsed     = j.value("room_section_rooms_collapsed",     false);
    room_section_spaces_collapsed    = j.value("room_section_spaces_collapsed",    false);
    room_section_inactive_collapsed        = j.value("room_section_inactive_collapsed",        true);
    room_section_unread_collapsed = j.value("room_section_unread_collapsed", false);
    room_section_space_unjoined_collapsed  = j.value("room_section_space_unjoined_collapsed",  false);
    room_section_call_rooms_collapsed      = j.value("room_section_call_rooms_collapsed",      false);
    room_section_knocks_collapsed          = j.value("room_section_knocks_collapsed",          false);

    sidebar_width     = j.value("sidebar_width",     0);
    sidebar_collapsed = j.value("sidebar_collapsed", false);

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

    encryption_reminder_snoozed_until.clear();
    if (j.contains("encryption_reminder_snoozed_until") &&
        j["encryption_reminder_snoozed_until"].is_object())
    {
        for (const auto& [uid, until] : j["encryption_reminder_snoozed_until"].items())
            if (until.is_number_integer())
                encryption_reminder_snoozed_until[uid] = until.get<std::int64_t>();
    }

    recovery_key_unsaved.clear();
    if (j.contains("recovery_key_unsaved") && j["recovery_key_unsaved"].is_array())
    {
        for (const auto& uid : j["recovery_key_unsaved"])
            if (uid.is_string())
                recovery_key_unsaved.insert(uid.get<std::string>());
    }

    save_key_reminder_dismissals.clear();
    if (j.contains("save_key_reminder_dismissals") &&
        j["save_key_reminder_dismissals"].is_object())
    {
        for (const auto& [uid, n] : j["save_key_reminder_dismissals"].items())
            if (n.is_number_integer())
                save_key_reminder_dismissals[uid] = n.get<int>();
    }

    silent_recovery_declined.clear();
    if (j.contains("silent_recovery_declined") && j["silent_recovery_declined"].is_array())
    {
        for (const auto& uid : j["silent_recovery_declined"])
            if (uid.is_string())
                silent_recovery_declined.insert(uid.get<std::string>());
    }

    save_key_reminder_snoozed_until.clear();
    if (j.contains("save_key_reminder_snoozed_until") &&
        j["save_key_reminder_snoozed_until"].is_object())
    {
        for (const auto& [uid, until] : j["save_key_reminder_snoozed_until"].items())
            if (until.is_number_integer())
                save_key_reminder_snoozed_until[uid] = until.get<std::int64_t>();
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

    {
        auto lvl = j.value("sdk_log_level", std::string{});
        if (!lvl.empty())
            sdk_log_level = lvl;
    }

    {
        auto mode = j.value("call_overlay_mode", std::string("docked"));
        if (mode == "docked_expanded") call_overlay_mode = CallOverlayMode::DockedExpanded;
        else if (mode == "floating")   call_overlay_mode = CallOverlayMode::Floating;
        else if (mode == "popout")     call_overlay_mode = CallOverlayMode::Popout;
        else                           call_overlay_mode = CallOverlayMode::Docked;
    }
    call_overlay_float_x = j.value("call_overlay_float_x", 40.0f);
    call_overlay_float_y = j.value("call_overlay_float_y", 40.0f);

    {
        auto layout = j.value("message_layout", std::string("classic"));
        message_layout = (layout == "bubbles") ? MessageLayout::Bubbles
                         : (layout == "irc")   ? MessageLayout::Irc
                                               : MessageLayout::Classic;
        bubbles_show_own_avatar = j.value("bubbles_show_own_avatar", true);
        timeline_column_pct = 100;
        if (auto it = j.find("timeline_column_pct"); it != j.end() && it->is_number_integer())
            timeline_column_pct = std::clamp(it->get<int>(), kTimelineColumnMinPct,
                                             kTimelineColumnMaxPct);
        timeline_column_anchor =
            j.value("timeline_column_anchor", std::string("left")) == "center"
                ? TimelineColumnAnchor::Center
                : TimelineColumnAnchor::Left;
        room_list_density =
            j.value("room_list_density", std::string("normal")) == "compact"
                ? RoomListDensity::Compact
                : RoomListDensity::Normal;
    }

    {
        const auto& kKeys = FontPrefs::kCategoryKeys;
        fonts = FontPrefs{};
        const auto fj = j.find("fonts");
        if (fj != j.end() && fj->is_object())
        {
            fonts.base_pt = fj->value("base_pt", 0);
            if (fonts.base_pt != 0)
                fonts.base_pt = std::clamp(fonts.base_pt, FontPrefs::kBaseMinPt, FontPrefs::kBaseMaxPt);
            fonts.ui_family = fj->value("ui_family", std::string{});
            fonts.mono_family = fj->value("mono_family", std::string{});
            const auto cj = fj->find("categories");
            if (cj != fj->end() && cj->is_object())
            {
                for (std::size_t i = 0; i < FontPrefs::kCategoryCount; ++i)
                {
                    const auto it = cj->find(kKeys[i]);
                    if (it == cj->end() || !it->is_object())
                        continue;
                    fonts.categories[i].delta_pt =
                        std::clamp(it->value("delta", 0), FontPrefs::kDeltaMinPt,
                                   FontPrefs::kDeltaMaxPt);
                    fonts.categories[i].family =
                        it->value("family", std::string{});
                }
            }
        }
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

    const char* theme_accent_str =
        theme_accent == ThemeAccent::Blue   ? "blue"   :
        theme_accent == ThemeAccent::Forest ? "forest" :
        theme_accent == ThemeAccent::Sunset ? "sunset" :
        theme_accent == ThemeAccent::Violet ? "violet" : "system";

    const char* low_power_str =
        low_power_pref == LowPowerPreference::On  ? "on"  :
        low_power_pref == LowPowerPreference::Off ? "off" : "auto";

    const char* close_action_str = close_action_to_string(close_action);

    // The proxy credential goes to the secret store; the file keeps the URL
    // without it. If the store is unavailable the file keeps it, as before.
    std::string proxy_url_to_write = proxy_url;
    bool creds_in_keyring = false;
    {
        const auto parts = split_proxy_credentials(proxy_url);
        if (!parts.userinfo.empty())
        {
            if ((parts.userinfo == proxy_creds_saved && proxy_creds_in_keyring) ||
                SecretStore::save_proxy_credentials(parts.userinfo))
            {
                proxy_creds_saved = parts.userinfo;
                proxy_creds_in_keyring = true;
                proxy_creds_unreadable = false;
                proxy_url_to_write = parts.url;
                creds_in_keyring = true;
            }
        }
        else if (proxy_creds_unreadable)
        {
            creds_in_keyring = true; // keep the marker; the secret is still there
        }
        else if (proxy_creds_in_keyring)
        {
            // The user took the credential out of the URL.
            SecretStore::remove_proxy_credentials();
            proxy_creds_in_keyring = false;
            proxy_creds_saved.clear();
        }
    }

    nlohmann::json j = {
        {"theme",                            theme_str},
        {"theme_accent",                     theme_accent_str},
        {"low_power",                        low_power_str},
        {"notifications_enabled",            notifications_enabled},
        {"notification_image_previews",      notification_image_previews},
        {"notification_hide_content",        notification_hide_content},
        {"prefetch_full_media",              prefetch_full_media},
        {"recompress_images_on_send",        recompress_images_on_send},
        {"open_media_externally",            open_media_externally},
        {"send_maps_urls_as_location",       send_maps_urls_as_location},
        {"send_bundled_url_previews",        send_bundled_url_previews},
        {"fetch_url_previews_directly",      fetch_url_previews_directly},
        {"exclude_insecure_devices",         exclude_insecure_devices},
        {"block_sends_on_unverified_devices", block_sends_on_unverified_devices},
        {"proxy_mode",                       proxy_mode == ProxyMode::None   ? "none"
                                           : proxy_mode == ProxyMode::Manual ? "manual"
                                                                             : "system"},
        {"proxy_url",                        proxy_url_to_write},
        {"proxy_credentials",                creds_in_keyring ? "keyring" : ""},
        {"group_inactive_rooms",             group_inactive_rooms},
        {"group_unread_rooms",            group_unread_rooms},
        {"inactive_room_threshold_days",     inactive_room_threshold_days},
        {"autoscroll_unread_rooms",          autoscroll_unread_rooms},
        {"prefetch_unread_rooms",            prefetch_unread_rooms},
        {"launch_at_login",                  launch_at_login},
        {"close_action",                     close_action_str},
        {"start_minimized",                  start_minimized},
        {"send_presence",                    send_presence},
        {"index_messages_for_search",        index_messages_for_search},
        {"check_for_updates",                check_for_updates},
        {"show_room_join_leave_events",      show_room_join_leave_events},
        {"show_sender_status_in_timeline",   show_sender_status_in_timeline},
        {"show_sender_timezone_in_timeline", show_sender_timezone_in_timeline},
        {"auto_detect_timezone",             auto_detect_timezone},
        {"animate_avatars",                  animate_avatars},
        {"msc2545_legacy_compat",            msc2545_legacy_compat},
        {"developer_mode",                   developer_mode},
        {"crash_reporting_enabled",          crash_reporting_enabled},
        {"room_section_invites_collapsed",   room_section_invites_collapsed},
        {"room_section_favorites_collapsed", room_section_favorites_collapsed},
        {"room_section_dms_collapsed",       room_section_dms_collapsed},
        {"room_section_rooms_collapsed",     room_section_rooms_collapsed},
        {"room_section_spaces_collapsed",    room_section_spaces_collapsed},
        {"room_section_inactive_collapsed",        room_section_inactive_collapsed},
        {"room_section_unread_collapsed", room_section_unread_collapsed},
        {"room_section_space_unjoined_collapsed",  room_section_space_unjoined_collapsed},
        {"room_section_call_rooms_collapsed",      room_section_call_rooms_collapsed},
        {"room_section_knocks_collapsed",          room_section_knocks_collapsed},
        {"sidebar_width",     sidebar_width},
        {"sidebar_collapsed", sidebar_collapsed},
        {"audio_input_device_id",  audio_input_device_id},
        {"audio_output_device_id", audio_output_device_id},
        {"camera_device_id",       camera_device_id},
    };
    j["language"]    = language;
    j["gif_api_key"] = gif_api_key;
    j["message_layout"] =
        message_layout == MessageLayout::Bubbles ? "bubbles"
        : message_layout == MessageLayout::Irc   ? "irc"
                                                 : "classic";
    j["bubbles_show_own_avatar"] = bubbles_show_own_avatar;
    j["timeline_column_pct"] = timeline_column_pct;
    j["timeline_column_anchor"] =
        timeline_column_anchor == TimelineColumnAnchor::Center ? "center" : "left";
    j["room_list_density"] =
        room_list_density == RoomListDensity::Compact ? "compact" : "normal";

    {
        const auto& kKeys = FontPrefs::kCategoryKeys;
        nlohmann::json cats = nlohmann::json::object();
        for (std::size_t i = 0; i < FontPrefs::kCategoryCount; ++i)
            cats[kKeys[i]] = {{"delta", fonts.categories[i].delta_pt},
                              {"family", fonts.categories[i].family}};
        j["fonts"] = {{"base_pt", fonts.base_pt},
                      {"ui_family", fonts.ui_family},
                      {"mono_family", fonts.mono_family},
                      {"categories", std::move(cats)}};
    }

    {
        const char* mode_str =
            call_overlay_mode == CallOverlayMode::DockedExpanded ? "docked_expanded" :
            call_overlay_mode == CallOverlayMode::Floating       ? "floating"        :
            call_overlay_mode == CallOverlayMode::Popout         ? "popout"          : "docked";
        j["call_overlay_mode"]    = mode_str;
        j["call_overlay_float_x"] = call_overlay_float_x;
        j["call_overlay_float_y"] = call_overlay_float_y;
    }

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

    if (!encryption_reminder_snoozed_until.empty())
    {
        nlohmann::json snoozes = nlohmann::json::object();
        for (const auto& [uid, until] : encryption_reminder_snoozed_until)
            snoozes[uid] = until;
        j["encryption_reminder_snoozed_until"] = std::move(snoozes);
    }
    if (!recovery_key_unsaved.empty())
        j["recovery_key_unsaved"] = recovery_key_unsaved;
    if (!save_key_reminder_dismissals.empty())
    {
        nlohmann::json counts = nlohmann::json::object();
        for (const auto& [uid, n] : save_key_reminder_dismissals)
            counts[uid] = n;
        j["save_key_reminder_dismissals"] = std::move(counts);
    }
    if (!silent_recovery_declined.empty())
        j["silent_recovery_declined"] = silent_recovery_declined;
    if (!save_key_reminder_snoozed_until.empty())
    {
        nlohmann::json snoozes = nlohmann::json::object();
        for (const auto& [uid, until] : save_key_reminder_snoozed_until)
            snoozes[uid] = until;
        j["save_key_reminder_snoozed_until"] = std::move(snoozes);
    }

    auto path = config_dir / "app_settings.json";
    // Keep the previous version as a last-good copy before replacing it, so a
    // file that later turns out unreadable can be recovered (see load).
    {
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
        {
            auto last_good = path;
            last_good += ".last-good";
            std::filesystem::copy_file(
                path, last_good, std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    // Tmp + fsync + rename: a crash can't leave a truncated settings file.
    if (!atomic_write_file(path, j.dump(4) + "\n"))
        std::fprintf(stderr, "tesseract: could not save %s\n", path.string().c_str());
}

const char* close_action_to_string(Settings::CloseAction action)
{
    switch (action)
    {
    case Settings::CloseAction::Quit:
        return "quit";
    case Settings::CloseAction::Minimize:
        return "minimize";
    case Settings::CloseAction::HideToTray:
        break;
    }
    return "tray";
}

Settings::CloseAction close_action_from_string(std::string_view value)
{
    if (value == "quit")
        return Settings::CloseAction::Quit;
    if (value == "minimize")
        return Settings::CloseAction::Minimize;
    return Settings::CloseAction::HideToTray;
}

} // namespace tesseract
