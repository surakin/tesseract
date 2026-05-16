#include "tesseract/settings.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace tesseract {

// Minimal extractor for a single string value by key from a flat JSON object.
// Handles {"theme":"system"} — values here never contain '"' or '\'.
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

void Settings::load_from_disk(const std::filesystem::path& config_dir) {
    auto path = config_dir / "app_settings.json";
    std::ifstream f(path);
    if (!f.is_open()) return;  // missing file → keep defaults

    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    auto theme = extract_string(json, "theme");
    if (theme == "light")       theme_pref = ThemePreference::Light;
    else if (theme == "dark")   theme_pref = ThemePreference::Dark;
    else                        theme_pref = ThemePreference::System;
}

void Settings::save_to_disk(const std::filesystem::path& config_dir) const {
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    // Ignore ec — if the directory doesn't exist the ofstream open-check catches it.

    const char* theme_str = "system";
    if (theme_pref == ThemePreference::Light)     theme_str = "light";
    else if (theme_pref == ThemePreference::Dark) theme_str = "dark";

    auto path = config_dir / "app_settings.json";
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    f << "{\"theme\":\"" << theme_str << "\"}";
}

} // namespace tesseract
