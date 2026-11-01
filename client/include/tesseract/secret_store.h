#pragma once

#include "tesseract/paths.h"

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
    static std::optional<std::string> load(const std::string& user_id)
    {
        return load_entry_(key_for(user_id), nullptr);
    }

    /// Persist `json` for `user_id`. Returns false on failure or when the
    /// backend is unavailable (caller should fall back to plaintext).
    static bool save(const std::string& user_id, const std::string& json)
    {
        return save_entry_(key_for(user_id), json, "Tesseract session");
    }

    /// Remove the credential for `user_id`. No-op if the key does not exist
    /// or the backend is unavailable.
    static void remove(const std::string& user_id)
    {
        remove_entry_(key_for(user_id));
    }

    /// The account's recovery key, when Tesseract set up recovery by itself
    /// (see ShellBase's silent encryption setup): held until the user saves
    /// it, and afterwards only if they chose to keep it on this device. A
    /// separate entry from the session; same semantics as load() / save() /
    /// remove().
    struct RecoveryKeyLookup
    {
        enum class Status
        {
            Found,
            Missing,    // no entry
            Unreadable, // the secure storage couldn't be read (locked keyring,
                        // dismissed unlock prompt, no backend, …)
        };
        Status status = Status::Missing;
        std::string key;
    };
    static RecoveryKeyLookup load_recovery_key(const std::string& user_id)
    {
        bool failed = false;
        if (auto key = load_entry_(recovery_key_for(user_id), &failed))
        {
            return {RecoveryKeyLookup::Status::Found, std::move(*key)};
        }
        return {failed ? RecoveryKeyLookup::Status::Unreadable : RecoveryKeyLookup::Status::Missing,
                {}};
    }
    static bool save_recovery_key(const std::string& user_id, const std::string& key)
    {
        return save_entry_(recovery_key_for(user_id), key, "Tesseract recovery key");
    }
    static void remove_recovery_key(const std::string& user_id)
    {
        remove_entry_(recovery_key_for(user_id));
    }

    /// The manual proxy's `user:password`, kept out of app_settings.json. One
    /// entry per profile; same semantics as load() / save() / remove().
    /// `failed` is set when the secure storage couldn't be read, as opposed to
    /// the entry not existing.
    static std::optional<std::string> load_proxy_credentials(bool* failed)
    {
        return load_entry_(proxy_credentials_key(), failed);
    }
    static bool save_proxy_credentials(const std::string& userinfo)
    {
        return save_entry_(proxy_credentials_key(), userinfo, "Tesseract proxy");
    }
    static void remove_proxy_credentials()
    {
        remove_entry_(proxy_credentials_key());
    }

    /// The backend key actually used for `user_id`: the bare MXID in the
    /// default profile (unchanged from before profiles existed, so no
    /// migration), `profile:<name>/<user_id>` under `--profile=<name>` so the
    /// same account signed in to two profiles keeps two independent
    /// credentials. MXIDs start with '@', so the two forms never collide.
    static std::string key_for(const std::string& user_id)
    {
        const std::string& p = profile();
        return p.empty() ? user_id : "profile:" + p + "/" + user_id;
    }

    /// The backend key of the recovery-key entry: key_for() behind a prefix
    /// neither an MXID ('@') nor a profile key ("profile:") can start with.
    static std::string recovery_key_for(const std::string& user_id)
    {
        return "recovery-key:" + key_for(user_id);
    }

    /// The backend key of the proxy-credentials entry (per profile, behind a
    /// prefix no MXID or other key starts with).
    static std::string proxy_credentials_key()
    {
        const std::string& p = profile();
        return p.empty() ? "proxy-credentials:" : "proxy-credentials:profile:" + p;
    }

private:
    // Per-backend primitives, keyed by the final backend key. `label` is the
    // human-readable name the OS keyring shows, where it shows one.
    // `failed` (optional): set when the backend couldn't be read, as opposed
    // to the entry not existing.
    static std::optional<std::string> load_entry_(const std::string& storage_key, bool* failed);
    static bool save_entry_(const std::string& storage_key, const std::string& value,
                            const char* label);
    static void remove_entry_(const std::string& storage_key);
};

} // namespace tesseract
