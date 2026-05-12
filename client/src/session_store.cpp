#include "tesseract/session_store.h"
#include "tesseract/paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace tesseract {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------

std::string SessionStore::path() {
    return (config_dir() / "session.json").string();
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
