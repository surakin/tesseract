#include "tesseract/settings.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace tesseract
{

// Minimal extractor for a single string value by key from a flat JSON object.
// Handles {"theme":"system"} — values here never contain '"' or '\'.
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

// Extractor for a bare JSON boolean (true / false) by key.
// Returns the default value when the key is absent or the token is unrecognized.
static bool extract_bool(const std::string& json, const std::string& key,
                         bool default_value = true)
{
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos)
    {
        return default_value;
    }
    pos += needle.size();
    // Skip whitespace and the colon separator.
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    {
        ++pos;
    }
    if (pos >= json.size())
    {
        return default_value;
    }
    if (json.compare(pos, 4, "true") == 0)
    {
        return true;
    }
    if (json.compare(pos, 5, "false") == 0)
    {
        return false;
    }
    return default_value;
}

void Settings::load_from_disk(const std::filesystem::path& config_dir)
{
    auto path = config_dir / "app_settings.json";
    std::ifstream f(path);
    if (!f.is_open())
    {
        return; // missing file → keep defaults
    }

    std::string json((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    auto theme = extract_string(json, "theme");
    if (theme == "light")
    {
        theme_pref = ThemePreference::Light;
    }
    else if (theme == "dark")
    {
        theme_pref = ThemePreference::Dark;
    }
    else
    {
        theme_pref = ThemePreference::System;
    }

    notifications_enabled = extract_bool(json, "notifications_enabled", true);
    notification_image_previews =
        extract_bool(json, "notification_image_previews", true);
    prefetch_full_media = extract_bool(json, "prefetch_full_media", false);
}

void Settings::save_to_disk(const std::filesystem::path& config_dir) const
{
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    // Ignore ec — if the directory doesn't exist the ofstream open-check catches it.

    const char* theme_str = "system";
    if (theme_pref == ThemePreference::Light)
    {
        theme_str = "light";
    }
    else if (theme_pref == ThemePreference::Dark)
    {
        theme_str = "dark";
    }

    auto path = config_dir / "app_settings.json";
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
    {
        return;
    }
    f << "{\"theme\":\"" << theme_str << "\""
      << ",\"notifications_enabled\":"
      << (notifications_enabled ? "true" : "false")
      << ",\"notification_image_previews\":"
      << (notification_image_previews ? "true" : "false")
      << ",\"prefetch_full_media\":"
      << (prefetch_full_media ? "true" : "false") << "}";
}

} // namespace tesseract
