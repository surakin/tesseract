#pragma once
#include <optional>
#include <string>

namespace tesseract {

/// Tiny per-user JSON-blob store for the OAuth session metadata produced by
/// Client::export_session(). Used to skip the LoginDialog on subsequent
/// launches when a valid session is already on disk.
///
/// On-disk layout (single file, atomically replaced):
///   Windows : %APPDATA%/Tesseract/session.json
///   macOS   : $HOME/Library/Application Support/Tesseract/session.json
///   Linux   : $XDG_CONFIG_HOME/tesseract/session.json   (defaults to ~/.config)
///
/// The blob is OAuth tokens + cross-signing metadata. v1 stores it in
/// plaintext; the migration path is to move the access/refresh tokens into
/// the platform secret store (DPAPI / libsecret / Keychain) and keep only
/// homeserver + user_id in this file. Don't add fields without thinking
/// about that migration.
class SessionStore {
public:
    /// Absolute path to the session file. Parent directory is not guaranteed
    /// to exist until save() runs.
    static std::string path();

    /// Returns the stored JSON if the file exists and is non-empty.
    static std::optional<std::string> load();

    /// Atomically write `json` (tmp file + rename) to path(). Creates the
    /// parent directory if missing. Returns true on success.
    static bool save(const std::string& json);

    /// Delete the session file if it exists. Idempotent.
    static void clear();
};

} // namespace tesseract
