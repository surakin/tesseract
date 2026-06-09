#include "tesseract/session_store.h"
#include "tesseract/secret_store.h"
#include "tesseract/paths.h"
#include "json_util.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace tesseract
{

namespace fs = std::filesystem;


// ---------------------------------------------------------------------------
// Tiny JSON scanners (Matrix IDs never contain '"' or '\\', and the file
// formats we emit are flat objects, so a regex-free scan is enough). Mirrors
// the style of `Prefs::parse` so the two stay consistent.
// ---------------------------------------------------------------------------

static std::string extract_string(std::string_view json, std::string_view key)
{
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');

    auto pos = json.find(needle);
    if (pos == std::string_view::npos)
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
    if (end == std::string_view::npos)
    {
        return {};
    }
    return std::string(json.substr(pos, end - pos));
}

// ---------------------------------------------------------------------------
// Atomic file write helper (tmp + rename, with a fall-back path for older
// filesystems that can't atomically replace an existing destination).
// ---------------------------------------------------------------------------

// Write `content` to `path` via a private temp file that is fsync'd before
// being atomically renamed into place. The file holds live OAuth tokens, so:
//   * it is created mode 0600 (never world-readable, even mid-write);
//   * the bytes are flushed to disk (fsync) before the rename, so a crash or
//     power loss can't leave a renamed-but-empty session file;
//   * the rename never deletes the destination first — on a filesystem that
//     rejects replace-rename the old file is moved aside and restored on
//     failure, so there is no window with no session file at all.
static bool write_all_fd(int fd, std::string_view content)
{
    const char* p = content.data();
    size_t left = content.size();
    while (left > 0)
    {
#if defined(_WIN32)
        int n = _write(
            fd, p,
            static_cast<unsigned>(left > 0x7fffffff ? 0x7fffffff : left));
#else
        ssize_t n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
#endif
        if (n <= 0)
        {
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

static bool atomic_write(const fs::path& p, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec)
    {
        return false;
    }

    fs::path tmp = p;
    tmp += ".tmp";

    {
#if defined(_WIN32)
        int fd = -1;
        _wsopen_s(&fd, tmp.wstring().c_str(),
                  _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYNO,
                  _S_IREAD | _S_IWRITE);
#else
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
#endif
        if (fd < 0)
        {
            return false;
        }
        bool ok = write_all_fd(fd, content);
#if defined(_WIN32)
        if (ok)
        {
            ok = (_commit(fd) == 0);
        }
        _close(fd);
#else
        if (ok)
        {
            ok = (::fsync(fd) == 0);
        }
        ::close(fd);
#endif
        if (!ok)
        {
            std::error_code rm;
            fs::remove(tmp, rm);
            return false;
        }
    }

    fs::rename(tmp, p, ec);
    if (ec)
    {
        // Filesystem rejected replace-rename. Move the existing file aside
        // first so a crash here still leaves a recoverable session, then
        // restore it if the final rename fails.
        fs::path bak = p;
        bak += ".bak";
        std::error_code be;
        fs::remove(bak, be);
        const bool had_old = fs::exists(p, be);
        if (had_old)
        {
            fs::rename(p, bak, be);
        }
        fs::rename(tmp, p, ec);
        if (ec)
        {
            if (had_old)
            {
                std::error_code re;
                fs::rename(bak, p, re);
            }
            std::error_code ec2;
            fs::remove(tmp, ec2);
            return false;
        }
        std::error_code re;
        fs::remove(bak, re);
    }
    return true;
}

// Written to session.json after a successful migration to SecretStore.
// load_account() treats this as "data is in SecretStore; return nullopt on miss."
static constexpr std::string_view kMigratedSentinel = "{\"v\":2}";

// ---------------------------------------------------------------------------
// Legacy single-account API
// ---------------------------------------------------------------------------

std::string SessionStore::path()
{
    return (config_dir() / "session.json").string();
}

std::optional<std::string> SessionStore::load()
{
    std::ifstream in(fs::path(path()), std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    // A read error (truncated read, I/O error) must not be mistaken for a
    // valid-but-short session blob: returning a partial token JSON would
    // surface as a confusing login failure instead of a clean re-login.
    if (in.bad())
    {
        return std::nullopt;
    }
    std::string s = buf.str();
    if (s.empty())
    {
        return std::nullopt;
    }
    return s;
}

bool SessionStore::save(const std::string& json)
{
    return atomic_write(fs::path(path()), json);
}

void SessionStore::clear()
{
    std::error_code ec;
    fs::remove(path(), ec);
}

// ---------------------------------------------------------------------------
// Multi-account API
// ---------------------------------------------------------------------------

std::string SessionStore::sanitize_user_id(const std::string& user_id)
{
    std::string out;
    out.reserve(user_id.size());
    for (char c : user_id)
    {
        switch (c)
        {
        // '.' is replaced too: a user_id containing ".." must not be able
        // to produce an account path that escapes the accounts/ directory.
        case '@':
        case ':':
        case '/':
        case '\\':
        case '.':
        case '?':
        case '*':
        case '"':
        case '<':
        case '>':
        case '|':
            out.push_back('_');
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    while (!out.empty() && out.front() == '_')
    {
        out.erase(out.begin());
    }
    return out;
}

fs::path SessionStore::account_dir(const std::string& user_id)
{
    return data_dir() / "accounts" / sanitize_user_id(user_id);
}

fs::path SessionStore::sdk_store_dir(const std::string& user_id)
{
    return account_dir(user_id) / "matrix-store";
}

// Parse the on-disk index body with nlohmann. Unlike the previous hand-rolled
// scanner (which returned a silently-empty index on ANY malformation —
// indistinguishable from "no accounts"), this distinguishes three outcomes:
//   * empty body            ⇒ empty index, not corrupt   (treated by caller as
//                                                          file-absent)
//   * valid JSON object     ⇒ parsed index, not corrupt
//   * parse error / wrong   ⇒ empty index, corrupt = true
//     shape
// The on-disk format is unchanged: a flat object with `active_user_id`
// (string) + `user_ids` (array of strings).
static SessionStore::AccountIndex parse_index_body(const std::string& body)
{
    SessionStore::AccountIndex idx;
    if (body.empty())
    {
        return idx; // not corrupt — caller treats an empty/absent file as "no
                    // accounts".
    }

    try
    {
        const auto j = nlohmann::json::parse(body);
        if (!j.is_object())
        {
            idx.corrupt = true;
            return idx;
        }
        if (auto it = j.find("active_user_id");
            it != j.end() && it->is_string())
        {
            idx.active_user_id = it->get<std::string>();
        }
        if (auto it = j.find("user_ids"); it != j.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                if (e.is_string())
                {
                    idx.user_ids.push_back(e.get<std::string>());
                }
            }
        }
    }
    catch (const nlohmann::json::exception&)
    {
        // Truncated / malformed JSON. Signal corruption so the caller does NOT
        // mistake a recoverable file for "no accounts" and drop every account.
        idx = SessionStore::AccountIndex{};
        idx.corrupt = true;
    }
    return idx;
}

SessionStore::AccountIndex SessionStore::load_index()
{
    AccountIndex idx;
    fs::path p = data_dir() / "accounts.json";
    std::ifstream in(p, std::ios::binary);
    if (!in)
    {
        return idx; // file absent ⇒ legitimately no accounts (corrupt == false)
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    if (in.bad())
    {
        // I/O error reading an existing file: treat like corruption so we don't
        // silently drop accounts and clobber the file on the next save.
        idx.corrupt = true;
        return idx;
    }

    idx = parse_index_body(buf.str());
    if (idx.corrupt)
    {
        std::fprintf(stderr,
                     "[tesseract] accounts.json at %s is corrupt or unparseable; "
                     "refusing to treat as 'no accounts'. It will be quarantined "
                     "to accounts.json.corrupt on the next save rather than "
                     "overwritten.\n",
                     p.string().c_str());
    }
    return idx;
}

static std::string serialize_index(const SessionStore::AccountIndex& idx)
{
    std::string out;
    out.reserve(64 + idx.user_ids.size() * 48);
    out.append("{\"active_user_id\":\"");
    out.append(json_escape(idx.active_user_id));
    out.append("\",\"user_ids\":[");
    bool first = true;
    for (const auto& uid : idx.user_ids)
    {
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        out.push_back('"');
        out.append(json_escape(uid));
        out.push_back('"');
    }
    out.append("]}");
    return out;
}

bool SessionStore::save_index(const AccountIndex& idx)
{
    const fs::path p = data_dir() / "accounts.json";

    // Invariant: a corrupt (but recoverable) accounts.json must never be
    // silently overwritten — that would make the data loss permanent. If the
    // file currently on disk fails to parse, quarantine it to
    // accounts.json.corrupt before writing the new index, so the original bytes
    // remain recoverable. This is self-contained: it re-reads what's actually
    // on disk rather than trusting a stale in-memory flag, so it protects the
    // invariant regardless of which caller is saving.
    std::error_code ec;
    if (fs::exists(p, ec))
    {
        std::string existing;
        {
            std::ifstream in(p, std::ios::binary);
            if (in)
            {
                std::ostringstream buf;
                buf << in.rdbuf();
                if (!in.bad())
                {
                    existing = buf.str();
                }
            }
        }
        if (parse_index_body(existing).corrupt)
        {
            const fs::path quarantine = data_dir() / "accounts.json.corrupt";
            std::error_code qe;
            // Best-effort: don't overwrite an earlier quarantine, and never let
            // a quarantine failure block the legitimate new write — but if it
            // does fail, abort rather than clobber the recoverable original.
            if (!fs::exists(quarantine, qe))
            {
                fs::rename(p, quarantine, qe);
                if (qe)
                {
                    std::fprintf(
                        stderr,
                        "[tesseract] failed to quarantine corrupt accounts.json "
                        "(%s); refusing to overwrite recoverable file.\n",
                        qe.message().c_str());
                    return false;
                }
            }
        }
    }

    return atomic_write(p, serialize_index(idx));
}

std::optional<std::string>
SessionStore::load_account(const std::string& user_id)
{
    // 1. Authoritative store: SecretStore (Keychain / CredManager / libsecret).
    //    Returns nullopt when the backend is unavailable (stub) or the key
    //    does not exist.
    if (auto sec = SecretStore::load(user_id))
        return sec;

    // 2. Legacy plaintext fallback.
    fs::path p = account_dir(user_id) / "session.json";
    std::string s;
    {
        // Scoped so the read handle is released before atomic_write below.
        // On Windows the replace-rename in atomic_write fails while another
        // handle in this process still has the target file open, which would
        // silently keep the plaintext in place and break the migration path.
        std::ifstream in(p, std::ios::binary);
        if (!in)
            return std::nullopt;
        std::ostringstream buf;
        buf << in.rdbuf();
        if (in.bad())
            return std::nullopt;
        s = buf.str();
    }

    // Sentinel: migration has already run but SecretStore returned nothing
    // (key deleted externally or backend temporarily unavailable). Treat as
    // no session — the user must log in again.
    if (s.empty() || s == kMigratedSentinel)
        return std::nullopt;

    // 3. Opportunistic migration: on first load after upgrade, move the
    //    plaintext session to SecretStore. Best-effort: if SecretStore is
    //    unavailable (stub) the plaintext continues to be used as-is.
    if (SecretStore::save(user_id, s))
        atomic_write(p, kMigratedSentinel);

    return s;
}

bool SessionStore::save_account(const std::string& user_id,
                                const std::string& json)
{
    if (sanitize_user_id(user_id).empty())
        return false;

    if (SecretStore::save(user_id, json))
    {
        // Sentinel write is best-effort: if it fails, SecretStore remains
        // authoritative on the next load (step 1 of load_account).
        atomic_write(account_dir(user_id) / "session.json", kMigratedSentinel);
        return true;
    }

    // SecretStore unavailable (stub / backend error) — write full JSON to
    // disk (plaintext fallback).
    return atomic_write(account_dir(user_id) / "session.json", json);
}

void SessionStore::clear_account(const std::string& user_id)
{
    SecretStore::remove(user_id);
    std::error_code ec;
    fs::remove_all(account_dir(user_id), ec);
}

// ---------------------------------------------------------------------------
// Legacy → multi-account migration
// ---------------------------------------------------------------------------

/// Where the Rust SDK used to put the matrix-sdk SQLite store before
/// `set_data_dir` existed. Mirrors what `default_data_dir()` in
/// `sdk/src/client.rs` computes today. Used only by `migrate_legacy_layout`.
static fs::path legacy_sdk_store_dir()
{
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
    {
        return fs::path(appdata) / "tesseract" / "matrix-store";
    }
    return fs::temp_directory_path() / "tesseract" / "matrix-store";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / "Library" / "Application Support" /
               "tesseract" / "matrix-store";
    }
    return fs::temp_directory_path() / "tesseract" / "matrix-store";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / "tesseract" / "matrix-store";
    }
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return fs::path(home) / ".local" / "share" / "tesseract" /
               "matrix-store";
    }
    return fs::temp_directory_path() / "tesseract" / "matrix-store";
#endif
}

/// `std::filesystem::rename` with a copy+remove fallback for cross-device
/// moves (EXDEV) or filesystems that can't rename a non-empty directory in
/// place. Used by the migration step; falls back to recursive copy when the
/// rename fails for any reason.
static bool move_path(const fs::path& src, const fs::path& dst)
{
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec)
    {
        return true;
    }

    fs::copy(src, dst,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (ec)
    {
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

/// Relocate an already-multi-account layout (`accounts/` tree + `accounts.json`)
/// from `config_dir()` to `data_dir()`. Older builds stored account data under
/// the config directory; this lifts it into the XDG data directory. A no-op when
/// `data_dir() == config_dir()` (Windows/macOS) — callers guard on that.
static bool migrate_config_accounts_to_data()
{
    std::error_code ec;
    const fs::path src_dir = config_dir() / "accounts";
    const fs::path src_index = config_dir() / "accounts.json";
    const fs::path dst_dir = data_dir() / "accounts";
    const fs::path dst_index = data_dir() / "accounts.json";

    fs::create_directories(data_dir(), ec);
    if (ec)
    {
        return false;
    }

    // Move the per-account tree first; accounts.json is the completion sentinel
    // (the data-dir existence check above), so it must land LAST — a crash
    // mid-move leaves the config-side index in place and the move retries
    // cleanly next launch.
    if (fs::exists(src_dir, ec) && !move_path(src_dir, dst_dir))
    {
        return false;
    }

    if (!move_path(src_index, dst_index))
    {
        // Roll the accounts tree back so the next launch retries cleanly.
        if (fs::exists(dst_dir, ec))
        {
            move_path(dst_dir, src_dir);
        }
        return false;
    }
    return true;
}

bool SessionStore::migrate_legacy_layout()
{
    std::error_code ec;

    // (0) Already migrated to the data dir? Bail.
    if (fs::exists(data_dir() / "accounts.json", ec))
    {
        return true;
    }

    // (1) Multi-account layout still under config_dir (older build)? Relocate
    //     the whole accounts/ tree + index into data_dir.
    if (data_dir() != config_dir() &&
        fs::exists(config_dir() / "accounts.json", ec))
    {
        return migrate_config_accounts_to_data();
    }

    // (2) Fresh install? Nothing to do.
    fs::path legacy_session = config_dir() / "session.json";
    fs::path legacy_store = legacy_sdk_store_dir();
    const bool have_session = fs::exists(legacy_session, ec);
    const bool have_store = fs::exists(legacy_store, ec);
    if (!have_session)
    {
        // An orphan store without a session blob is unusable — the SDK can't
        // restore without the OAuth tokens, so delete it to free disk.
        if (have_store)
        {
            fs::remove_all(legacy_store, ec);
        }
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
    const std::string uid = extract_string(blob, "user_id");
    if (uid.empty() || sanitize_user_id(uid).empty())
    {
        // (3a) Corrupt blob: throw away both legacy files and let the user
        // log in fresh on the next launch.
        fs::remove(legacy_session, ec);
        if (have_store)
        {
            fs::remove_all(legacy_store, ec);
        }
        return true;
    }

    // (4) Ensure the destination dir exists.
    fs::path dst = account_dir(uid);
    fs::create_directories(dst, ec);
    if (ec)
    {
        return false;
    }

    // (5) Move session.json into the account dir.
    fs::path dst_session = dst / "session.json";
    if (!move_path(legacy_session, dst_session))
    {
        return false;
    }

    // (6) Move matrix-store/ into the account dir. If this fails, restore
    // the session file so the next launch retries cleanly — never leave the
    // SDK pointed at an empty new store while the legacy data is still on
    // disk.
    if (have_store)
    {
        fs::path dst_store = dst / "matrix-store";
        if (!move_path(legacy_store, dst_store))
        {
            std::error_code rb;
            fs::rename(dst_session, legacy_session, rb);
            if (rb)
            {
                std::fprintf(
                    stderr,
                    "[tesseract] migration rollback failed: session stranded "
                    "at %s (%s)\n",
                    dst_session.string().c_str(), rb.message().c_str());
            }
            // Best-effort cleanup of the now-empty account dir.
            std::error_code rmc;
            fs::remove(dst, rmc);
            return false;
        }
    }

    // (6a) Refresh SecretStore so that load_account() (which checks the
    // credential store first) sees the migrated content and not a stale
    // pre-migration snapshot. Best-effort: if SecretStore is unavailable,
    // load_account() falls back to the plaintext file moved in step (5).
    SecretStore::save(uid, blob);

    // (7) Write accounts.json. If this fails after both moves succeeded the
    // app would loop forever on a half-migrated layout — undo the moves.
    AccountIndex idx;
    idx.active_user_id = uid;
    idx.user_ids = {uid};
    if (!save_index(idx))
    {
        std::error_code rb;
        fs::rename(dst_session, legacy_session, rb);
        if (rb)
        {
            std::fprintf(
                stderr,
                "[tesseract] migration rollback failed: session stranded "
                "at %s (%s)\n",
                dst_session.string().c_str(), rb.message().c_str());
        }
        std::error_code rb2;
        if (have_store)
        {
            fs::rename(dst / "matrix-store", legacy_store, rb2);
        }
        std::error_code rmc;
        fs::remove(dst, rmc);
        return false;
    }

    return true;
}

} // namespace tesseract
