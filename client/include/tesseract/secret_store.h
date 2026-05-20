#pragma once

#include <optional>
#include <string>

namespace tesseract
{

// Per-platform secure credential storage for OAuth session blobs.
//
// Backends:
//   Windows : Windows Credential Manager (CredWriteW / CredReadW / CredDeleteW)
//   macOS   : Keychain Services (SecItemAdd / SecItemCopyMatching / SecItemDelete)
//   Linux   : libsecret (secret_password_*_sync) — falls back to the no-op
//             stub when libsecret is not found at configure time.
//
// All methods are synchronous and safe to call on the UI thread: the
// credential APIs used here are fast (in-process or IPC but not network).
class SecretStore
{
public:
    /// Load the JSON blob stored for `user_id`. Returns nullopt when the key
    /// does not exist, on backend error, or when the backend is unavailable.
    static std::optional<std::string> load(const std::string& user_id);

    /// Persist `json` for `user_id`. Returns false on failure or when the
    /// backend is unavailable (caller should fall back to plaintext).
    static bool save(const std::string& user_id, const std::string& json);

    /// Remove the credential for `user_id`. No-op if the key does not exist
    /// or the backend is unavailable.
    static void remove(const std::string& user_id);
};

} // namespace tesseract
