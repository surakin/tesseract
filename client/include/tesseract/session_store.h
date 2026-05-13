#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tesseract {

/// Persistent on-disk store for OAuth session blobs.
///
/// On-disk layout (multi-account, per-user-id directory):
///   Windows : %APPDATA%/Tesseract/
///   macOS   : $HOME/Library/Application Support/Tesseract/
///   Linux   : $XDG_CONFIG_HOME/tesseract/   (defaults to ~/.config/tesseract)
///
///   <config>/accounts.json                                 ← index
///   <config>/accounts/<sanitized-uid>/session.json         ← PersistedSession
///   <config>/accounts/<sanitized-uid>/matrix-store/        ← SDK SQLite cache
///
/// The legacy single-account layout (a `session.json` directly under
/// `<config>/` plus a `matrix-store/` next to it) is migrated into the new
/// layout on first launch by `migrate_legacy_layout()`. After migration, the
/// legacy `path()` / `load()` / `save()` / `clear()` static helpers are
/// effectively dead code — callers should move to the per-account methods.
///
/// The session blob carries OAuth tokens + cross-signing metadata. v1 stores
/// them in plaintext on every platform; the migration path is to move
/// access/refresh tokens into the platform secret store (DPAPI / libsecret /
/// Keychain) and keep only homeserver + user_id in this file. Don't add
/// fields without thinking about that migration.
class SessionStore {
public:
    // ---------- Legacy single-account API (kept until shells migrate) ----------

    /// Absolute path to the legacy single-account session file.
    static std::string path();
    static std::optional<std::string> load();
    static bool        save(const std::string& json);
    static void        clear();

    // ---------- Multi-account API ----------

    /// Index of every account whose session is persisted on disk, plus which
    /// one is the foreground account. `active_user_id` is empty when no
    /// accounts are present.
    struct AccountIndex {
        std::string              active_user_id;
        std::vector<std::string> user_ids;
    };

    /// Replace `:` / `/` / `\` / `@` (anything that would be awkward in a
    /// path segment) with `_`. Returns an empty string when the input is
    /// empty or sanitises to nothing.
    static std::string sanitize_user_id(const std::string& user_id);

    /// `<config>/accounts/<sanitize(user_id)>/`. Does not create the
    /// directory; callers do that explicitly when they're about to write.
    static std::filesystem::path account_dir(const std::string& user_id);

    /// `<config>/accounts/<sanitize(user_id)>/matrix-store`. Pass this to
    /// `Client::set_data_dir` before `begin_oauth` / `restore_session`.
    static std::filesystem::path sdk_store_dir(const std::string& user_id);

    /// Read `<config>/accounts.json`. Missing / unparseable file ⇒ empty
    /// `AccountIndex` (active_user_id == "").
    static AccountIndex load_index();

    /// Atomically write `accounts.json`. Returns false on filesystem error.
    static bool save_index(const AccountIndex& idx);

    /// Read `<config>/accounts/<sanitize(user_id)>/session.json`. Returns
    /// nullopt when the file is missing or empty.
    static std::optional<std::string> load_account(const std::string& user_id);

    /// Atomically write a PersistedSession JSON into the account directory.
    /// Creates the directory tree on demand. Returns false on filesystem
    /// error.
    static bool save_account(const std::string& user_id, const std::string& json);

    /// Remove `<config>/accounts/<sanitize(user_id)>/` (session, SDK store,
    /// everything). Idempotent.
    static void clear_account(const std::string& user_id);

    /// One-shot, idempotent migration from the legacy single-account layout
    /// to the multi-account directory layout. Safe to call on every startup
    /// before any `Client` is constructed. Returns:
    ///   true  — migration was unnecessary, or completed cleanly.
    ///   false — migration was attempted and a filesystem error rolled it
    ///           back to the legacy layout (try again next launch).
    /// See the test cases in `test_session_store.cpp` for the full state
    /// machine (clean install / legacy session+store / legacy session only /
    /// already-migrated / corrupt session / store-move failure).
    static bool migrate_legacy_layout();
};

} // namespace tesseract
