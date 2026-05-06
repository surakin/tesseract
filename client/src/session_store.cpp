#include "tesseract/session_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace tesseract {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------

namespace {

fs::path config_root() {
#if defined(_WIN32)
    // %APPDATA% is the Roaming folder; this is what you want for per-user app data.
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        return fs::path(appdata) / "Tesseract";
    }
    // Fallback via SHGetFolderPath if the env var is missing.
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        return fs::path(buf) / L"Tesseract";
    }
    return fs::temp_directory_path() / "Tesseract";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / "Library" / "Application Support" / "Tesseract";
    }
    return fs::temp_directory_path() / "Tesseract";
#else  // Linux / *BSD: XDG basedir
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return fs::path(xdg) / "tesseract";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".config" / "tesseract";
    }
    return fs::temp_directory_path() / "tesseract";
#endif
}

} // namespace

// ---------------------------------------------------------------------------

std::string SessionStore::path() {
    return (config_root() / "session.json").string();
}

std::optional<std::string> SessionStore::load() {
    std::error_code ec;
    fs::path p = path();
    if (!fs::exists(p, ec) || ec) return std::nullopt;

    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;

    std::ostringstream buf;
    buf << in.rdbuf();
    std::string s = buf.str();
    if (s.empty()) return std::nullopt;
    return s;
}

bool SessionStore::save(const std::string& json) {
    std::error_code ec;
    fs::path p   = path();
    fs::path dir = p.parent_path();

    fs::create_directories(dir, ec);
    if (ec) return false;

    fs::path tmp = p;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) return false;
    } // close before rename

    fs::rename(tmp, p, ec);
    if (ec) {
        // Some filesystems can't rename across the same directory atomically
        // when the target exists (e.g. older Windows). Fall back to remove + rename.
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

void SessionStore::clear() {
    std::error_code ec;
    fs::remove(path(), ec);
}

} // namespace tesseract
