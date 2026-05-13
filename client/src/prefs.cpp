#include "tesseract/prefs.h"
#include "tesseract/paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace tesseract {

namespace fs = std::filesystem;

static fs::path last_room_path() {
    return config_dir() / "last_room.txt";
}

std::string Prefs::load_last_room() {
    std::error_code ec;
    fs::path p = last_room_path();
    if (!fs::exists(p, ec) || ec) return {};

    std::ifstream in(p, std::ios::binary);
    if (!in) return {};

    std::ostringstream buf;
    buf << in.rdbuf();
    std::string s = buf.str();
    // Strip trailing newline / whitespace.
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

void Prefs::save_last_room(const std::string& room_id) {
    if (room_id.empty()) return;

    std::error_code ec;
    fs::path p = last_room_path();
    fs::create_directories(p.parent_path(), ec);
    if (ec) return;

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(room_id.data(), static_cast<std::streamsize>(room_id.size()));
    out.put('\n');
}

} // namespace tesseract
