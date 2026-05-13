#include "tesseract/session_store.h"
#include "tesseract/paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tesseract {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Tiny JSON scanners (Matrix IDs never contain '"' or '\\', and the file
// formats we emit are flat objects, so a regex-free scan is enough). Mirrors
// the style of `Prefs::parse` so the two stay consistent.
// ---------------------------------------------------------------------------

static std::string extract_string(std::string_view json, std::string_view key) {
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');

    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
        ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(pos, end - pos));
}

static std::vector<std::string> extract_string_array(std::string_view json,
                                                     std::string_view key) {
    std::vector<std::string> out;

    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');

    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return out;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
        ++pos;
    if (pos >= json.size() || json[pos] != '[') return out;
    ++pos;

    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                     json[pos] == ',' || json[pos] == '\n' || json[pos] == '\r'))
            ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '"') break;  // malformed
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) break;
        out.emplace_back(json.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Atomic file write helper (tmp + rename, with a fall-back path for older
// filesystems that can't atomically replace an existing destination).
// ---------------------------------------------------------------------------

static bool atomic_write(const fs::path& p, std::string_view content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) return false;

    fs::path tmp = p;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) return false;
    }

    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
        if (ec) {
            std::error_code ec2;
            fs::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Legacy single-account API
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
    return atomic_write(fs::path(path()), json);
}

void SessionStore::clear() {
    std::error_code ec;
    fs::remove(path(), ec);
}

// ---------------------------------------------------------------------------
// Multi-account API
// ---------------------------------------------------------------------------

std::string SessionStore::sanitize_user_id(const std::string& user_id) {
    std::string out;
    out.reserve(user_id.size());
    for (char c : user_id) {
        switch (c) {
            case '@': case ':': case '/': case '\\':
            case '?': case '*': case '"': case '<': case '>': case '|':
                out.push_back('_');
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    return out;
}

fs::path SessionStore::account_dir(const std::string& user_id) {
    return config_dir() / "accounts" / sanitize_user_id(user_id);
}

fs::path SessionStore::sdk_store_dir(const std::string& user_id) {
    return account_dir(user_id) / "matrix-store";
}

SessionStore::AccountIndex SessionStore::load_index() {
    AccountIndex idx;
    std::error_code ec;
    fs::path p = config_dir() / "accounts.json";
    if (!fs::exists(p, ec) || ec) return idx;

    std::ifstream in(p, std::ios::binary);
    if (!in) return idx;
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string body = buf.str();
    if (body.empty()) return idx;

    idx.active_user_id = extract_string(body, "active_user_id");
    idx.user_ids       = extract_string_array(body, "user_ids");
    return idx;
}

static std::string serialize_index(const SessionStore::AccountIndex& idx) {
    std::string out;
    out.reserve(64 + idx.user_ids.size() * 48);
    out.append("{\"active_user_id\":\"");
    out.append(idx.active_user_id);
    out.append("\",\"user_ids\":[");
    bool first = true;
    for (const auto& uid : idx.user_ids) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out.append(uid);
        out.push_back('"');
    }
    out.append("]}");
    return out;
}

bool SessionStore::save_index(const AccountIndex& idx) {
    return atomic_write(config_dir() / "accounts.json", serialize_index(idx));
}

std::optional<std::string> SessionStore::load_account(const std::string& user_id) {
    std::error_code ec;
    fs::path p = account_dir(user_id) / "session.json";
    if (!fs::exists(p, ec) || ec) return std::nullopt;

    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string s = buf.str();
    if (s.empty()) return std::nullopt;
    return s;
}

bool SessionStore::save_account(const std::string& user_id, const std::string& json) {
    if (sanitize_user_id(user_id).empty()) return false;
    return atomic_write(account_dir(user_id) / "session.json", json);
}

void SessionStore::clear_account(const std::string& user_id) {
    std::error_code ec;
    fs::remove_all(account_dir(user_id), ec);
}

// ---------------------------------------------------------------------------
// Legacy → multi-account migration
// ---------------------------------------------------------------------------

/// Where the Rust SDK used to put the matrix-sdk SQLite store before
/// `set_data_dir` existed. Mirrors what `default_data_dir()` in
/// `sdk/src/client.rs` computes today. Used only by `migrate_legacy_layout`.
static fs::path legacy_sdk_store_dir() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        return fs::path(appdata) / "tesseract" / "matrix-store";
    }
    return fs::temp_directory_path() / "tesseract" / "matrix-store";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / "Library" / "Application Support"
             / "tesseract" / "matrix-store";
    }
    return fs::temp_directory_path() / "tesseract" / "matrix-store";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return fs::path(xdg) / "tesseract" / "matrix-store";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".local" / "share" / "tesseract" / "matrix-store";
    }
    return fs::temp_directory_path() / "tesseract" / "matrix-store";
#endif
}

/// `std::filesystem::rename` with a copy+remove fallback for cross-device
/// moves (EXDEV) or filesystems that can't rename a non-empty directory in
/// place. Used by the migration step; falls back to recursive copy when the
/// rename fails for any reason.
static bool move_path(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec) return true;

    fs::copy(src, dst,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
        std::error_code ec2;
        fs::remove_all(dst, ec2);
        return false;
    }
    fs::remove_all(src, ec);
    // If the source can't be removed, the migration still succeeded — the
    // copy reached the destination. Stale source data is harmless next
    // launch because we look at accounts.json first.
    return true;
}

bool SessionStore::migrate_legacy_layout() {
    std::error_code ec;

    // (1) Already migrated? Bail.
    if (fs::exists(config_dir() / "accounts.json", ec)) return true;

    // (2) Fresh install? Nothing to do.
    fs::path legacy_session = config_dir() / "session.json";
    fs::path legacy_store   = legacy_sdk_store_dir();
    const bool have_session = fs::exists(legacy_session, ec);
    const bool have_store   = fs::exists(legacy_store, ec);
    if (!have_session) {
        // An orphan store without a session blob is unusable — the SDK can't
        // restore without the OAuth tokens, so delete it to free disk.
        if (have_store) fs::remove_all(legacy_store, ec);
        return true;
    }

    // (3) Read user_id out of session.json. The ifstream is closed before
    // the move/remove below — on Windows an open handle blocks rename().
    std::string blob;
    {
        std::ifstream in(legacy_session, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        blob = buf.str();
    }
    const std::string uid  = extract_string(blob, "user_id");
    if (uid.empty() || sanitize_user_id(uid).empty()) {
        // (3a) Corrupt blob: throw away both legacy files and let the user
        // log in fresh on the next launch.
        fs::remove(legacy_session, ec);
        if (have_store) fs::remove_all(legacy_store, ec);
        return true;
    }

    // (4) Ensure the destination dir exists.
    fs::path dst = account_dir(uid);
    fs::create_directories(dst, ec);
    if (ec) return false;

    // (5) Move session.json into the account dir.
    fs::path dst_session = dst / "session.json";
    if (!move_path(legacy_session, dst_session)) return false;

    // (6) Move matrix-store/ into the account dir. If this fails, restore
    // the session file so the next launch retries cleanly — never leave the
    // SDK pointed at an empty new store while the legacy data is still on
    // disk.
    if (have_store) {
        fs::path dst_store = dst / "matrix-store";
        if (!move_path(legacy_store, dst_store)) {
            std::error_code rb;
            fs::rename(dst_session, legacy_session, rb);
            // Best-effort cleanup of the now-empty account dir.
            fs::remove(dst, rb);
            return false;
        }
    }

    // (7) Write accounts.json. If this fails after both moves succeeded the
    // app would loop forever on a half-migrated layout — undo the moves.
    AccountIndex idx;
    idx.active_user_id = uid;
    idx.user_ids       = { uid };
    if (!save_index(idx)) {
        std::error_code rb;
        fs::rename(dst_session, legacy_session, rb);
        if (have_store) fs::rename(dst / "matrix-store", legacy_store, rb);
        fs::remove(dst, rb);
        return false;
    }

    return true;
}

} // namespace tesseract
