#include "tesseract/paths.h"

#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace tesseract
{

namespace fs = std::filesystem;

namespace
{

std::string& profile_storage()
{
    static std::string name;
    return name;
}

// "Tesseract" / "tesseract" plus the profile suffix.
fs::path app_folder(const char* base)
{
    return fs::path(std::string(base) + profile_suffix());
}

} // namespace

bool is_valid_profile_name(std::string_view name)
{
    if (name.empty() || name.size() > 32)
    {
        return false;
    }
    for (char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

void set_profile(std::string_view name)
{
    if (name.empty() || is_valid_profile_name(name))
    {
        profile_storage() = std::string(name);
    }
}

const std::string& profile()
{
    return profile_storage();
}

std::string profile_suffix()
{
    const auto& p = profile_storage();
    return p.empty() ? std::string{} : "-" + p;
}

// Resolves the per-user config directory from platform conventions. The
// XDG/APPDATA/HOME environment variables are trusted here: a process that can
// set them for this app can equally exec a tampered binary, so validating
// them against the real home buys little while breaking legitimate sandboxed
// / relocated setups (containers, Flatpak, custom XDG roots). Tokens written
// under this directory are themselves protected by 0600 file permissions in
// session_store.cpp.
fs::path config_dir()
{
#if defined(_WIN32)
    // %APPDATA% is Roaming — the right place for per-user app data.
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
    {
        return fs::path(appdata) / app_folder("Tesseract");
    }
    // Fallback via SHGetFolderPath if the env var is missing.
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
    {
        return fs::path(buf) / app_folder("Tesseract");
    }
    return fs::temp_directory_path() / app_folder("Tesseract");
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / "Library" / "Application Support" / app_folder("Tesseract");
    }
    return fs::temp_directory_path() / app_folder("Tesseract");
#else // Linux / *BSD: XDG basedir
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / app_folder("tesseract");
    }
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / ".config" / app_folder("tesseract");
    }
    return fs::temp_directory_path() / app_folder("tesseract");
#endif
}

fs::path data_dir()
{
#if defined(_WIN32) || defined(__APPLE__)
    // No XDG-style data/config split on these platforms; account data and app
    // config share the per-user app directory.
    return config_dir();
#else // Linux / *BSD: XDG basedir — data lives under XDG_DATA_HOME.
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / app_folder("tesseract");
    }
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / ".local" / "share" / app_folder("tesseract");
    }
    return fs::temp_directory_path() / app_folder("tesseract");
#endif
}

fs::path cache_dir()
{
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
    {
        return fs::path(local) / app_folder("Tesseract");
    }
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(
            SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
    {
        return fs::path(buf) / app_folder("Tesseract");
    }
    return fs::temp_directory_path() / app_folder("Tesseract");
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / "Library" / "Caches" / app_folder("Tesseract");
    }
    return fs::temp_directory_path() / app_folder("Tesseract");
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / app_folder("tesseract");
    }
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / ".cache" / app_folder("tesseract");
    }
    return fs::temp_directory_path() / app_folder("tesseract");
#endif
}

} // namespace tesseract
