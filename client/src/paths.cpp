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
        return fs::path(appdata) / "Tesseract";
    }
    // Fallback via SHGetFolderPath if the env var is missing.
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
    {
        return fs::path(buf) / L"Tesseract";
    }
    return fs::temp_directory_path() / "Tesseract";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / "Library" / "Application Support" / "Tesseract";
    }
    return fs::temp_directory_path() / "Tesseract";
#else // Linux / *BSD: XDG basedir
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / "tesseract";
    }
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / ".config" / "tesseract";
    }
    return fs::temp_directory_path() / "tesseract";
#endif
}

fs::path cache_dir()
{
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
    {
        return fs::path(local) / "Tesseract";
    }
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(
            SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
    {
        return fs::path(buf) / L"Tesseract";
    }
    return fs::temp_directory_path() / "Tesseract";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / "Library" / "Caches" / "Tesseract";
    }
    return fs::temp_directory_path() / "Tesseract";
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / "tesseract";
    }
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / ".cache" / "tesseract";
    }
    return fs::temp_directory_path() / "tesseract";
#endif
}

} // namespace tesseract
