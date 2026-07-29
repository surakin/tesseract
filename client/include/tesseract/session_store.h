#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tesseract
{

/// Persistent on-disk store for OAuth session blobs.
///
/// Account data lives under `data_dir()` (see paths.h), NOT `config_dir()` —
/// only `app_settings.json` belongs in the config directory.
///   Windows : %APPDATA%/Tesseract/        (data_dir() == config_dir())
///   macOS   : $HOME/Library/Application Support/Tesseract/ (data == config)
///   Linux   : $XDG_DATA_HOME/tesseract/    (defaults to ~/.local/share/tesseract)
///
///   <data>/accounts.json                                 ← index
///   <data>/accounts/<sanitized-uid>/session.json         ← PersistedSession
///   <data>/accounts/<sanitized-uid>/matrix-store/        ← SDK SQLite cache
///
/// Two migrations run on first launch via `migrate_legacy_layout()`:
///   (a) the pre-multi-account single-account layout (a `session.json` directly
///       under `config_dir()` plus the SDK's old `matrix-store/`), and
///   (b) a multi-account `accounts/` tree left under `config_dir()` by builds
///       that predate the data/config split (Linux only).
/// After migration the legacy `path()` / `load()` / `save()` / `clear()` static
/// helpers are effectively dead code — callers should use the per-account
/// methods.
///
/// The session blob carries OAuth tokens + cross-signing metadata. v1 stores
/// them in plaintext on every platform; the migration path is to move
/// access/refresh tokens into the platform secret store (DPAPI / libsecret /
/// Keychain) and keep only homeserver + user_id in this file. Don't add
/// fields without thinking about that migration.
class SessionStore
{
public:
    // ---------- Legacy single-account API (kept until shells migrate) ----------

    /// Absolute path to the legacy single-account session file.
    static std::string path();
    static std::optional<std::string> load();
    static bool save(const std::string& json);
    static void clear();

    // ---------- Multi-account API ----------

    /// Index of every account whose session is persisted on disk, plus which
    /// one is the foreground account. `active_user_id` is empty when no
    /// accounts are present.
    struct AccountIndex
    {
        std::string active_user_id;
        std::vector<std::string> user_ids;

        /// True when `accounts.json` existed on disk but could not be parsed
        /// (truncated / malformed JSON). This is distinct from a legitimately
        /// absent file, which yields an empty index with `corrupt == false`.
        ///
        /// A corrupt index must NOT be mistaken for "no accounts": that would
        /// silently drop every known account and force a re-login of each. When
        /// this flag is set, `save_index` refuses to clobber the (recoverable)
        /// file with a fresh empty set on the same run — it quarantines the
        /// original to `accounts.json.corrupt` first.
        bool corrupt = false;
    };

    /// Replace `:` / `/` / `\` / `@` (anything that would be awkward in a
    /// path segment) with `_`. Returns an empty string when the input is
    /// empty or sanitises to nothing.
    static std::string sanitize_user_id(const std::string& user_id);

    /// `<data>/accounts/<sanitize(user_id)>/`. Does not create the
    /// directory; callers do that explicitly when they're about to write.
    static std::filesystem::path account_dir(const std::string& user_id);

    /// `<data>/accounts/<sanitize(user_id)>/matrix-store`. Pass this to
    /// `Client::set_data_dir` before `begin_oauth` / `restore_session`.
    static std::filesystem::path sdk_store_dir(const std::string& user_id);

    /// Read `<data>/accounts.json`.
    ///   * Absent / empty file ⇒ empty `AccountIndex`, `corrupt == false`
    ///     (legitimately no accounts).
    ///   * Valid JSON object   ⇒ parsed `AccountIndex`, `corrupt == false`.
    ///   * Truncated / malformed / wrong-shape file, or an I/O read error ⇒
    ///     empty `AccountIndex` with `corrupt == true`.
    /// Callers MUST check `corrupt`: an empty index with `corrupt == true` means
    /// "the file exists but is unreadable" — NOT "no accounts" — and must not be
    /// treated as a reason to drop every account or to persist an empty set.
    static AccountIndex load_index();

    /// Atomically write `accounts.json`. Returns false on filesystem error.
    /// If the file currently on disk is corrupt/unparseable, it is first
    /// quarantined to `accounts.json.corrupt` so the recoverable original is
    /// never silently clobbered by the new (possibly empty) index.
    static bool save_index(const AccountIndex& idx);

    /// Read `<data>/accounts/<sanitize(user_id)>/session.json`. Returns
    /// nullopt when the file is missing or empty.
    static std::optional<std::string> load_account(const std::string& user_id);

    /// Atomically write a PersistedSession JSON into the account directory.
    /// Creates the directory tree on demand. Returns false on filesystem
    /// error.
    static bool save_account(const std::string& user_id,
                             const std::string& json);

    /// A session JSON blob plus the key (if any) encrypting its matrix-sdk
    /// SQLite store. `store_key` is empty for a session that predates store
    /// encryption, or was otherwise never given one — Tesseract does not
    /// migrate existing sessions, so an empty key here is a permanent, valid
    /// state, not a TODO.
    struct LoadedAccount
    {
        std::string session_json;
        std::vector<uint8_t> store_key;
    };

    /// Like `load_account`, but also returns the store-encryption key
    /// persisted alongside the session by `save_account_with_key`. A record
    /// written by the plain `save_account` (or one written before this
    /// existed) is recognised as such and returned with an empty
    /// `store_key`, unencrypted, unchanged from today.
    static std::optional<LoadedAccount> load_account_with_key(
        const std::string& user_id);

    /// Like `save_account`, but also persists `store_key` alongside the
    /// session (pass an empty vector for a session that has none). Wraps
    /// both into one JSON blob stored through the same `save_account` path,
    /// so it's subject to the exact same atomicity/SecretStore-vs-plaintext
    /// fallback behavior.
    static bool save_account_with_key(const std::string& user_id,
                                      const std::string& session_json,
                                      const std::vector<uint8_t>& store_key);

    /// Persist an updated session JSON (e.g. after a token refresh) while
    /// preserving whatever store-encryption key is already on file for this
    /// account. Looks up the account's current record and keeps its
    /// store_key unchanged; falls back to a bare `save_account` when the
    /// account has no key (legacy/unencrypted) or no prior record exists
    /// yet. Use this — never a bare `save_account` — for any persistence
    /// path that doesn't itself know the account's store_key, so a refresh
    /// can never silently strip the key from an already-encrypted account.
    static bool save_session_update(const std::string& user_id,
                                    const std::string& session_json);

    /// Remove `<data>/accounts/<sanitize(user_id)>/` (session, SDK store,
    /// everything). Idempotent.
    static void clear_account(const std::string& user_id);

    /// One-shot, idempotent migration to the current `data_dir()` layout.
    /// Handles both the pre-multi-account single-account layout and a
    /// multi-account `accounts/` tree left under `config_dir()` by older builds.
    /// Safe to call on every startup before any `Client` is constructed.
    /// Returns:
    ///   true  — migration was unnecessary, or completed cleanly.
    ///   false — migration was attempted and a filesystem error rolled it
    ///           back to the previous layout (try again next launch).
    /// See the test cases in `test_session_store.cpp` for the full state
    /// machine (clean install / legacy session+store / legacy session only /
    /// already-migrated / corrupt session / store-move failure /
    /// config→data relocation).
    static bool migrate_legacy_layout();
};

} // namespace tesseract
