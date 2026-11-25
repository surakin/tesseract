#pragma once

// Consent for running a world's scripts: built-in scripts run, a bundle hash
// the user trusted before runs, a bundle published by a verified user runs,
// anything else asks.

#include <tesseract/types.h>

#include <filesystem>
#include <mutex>
#include <set>
#include <string>

namespace tesseract
{

/// Local (per device) list of trusted world-bundle hashes, one JSON file.
/// A missing or corrupt file is an empty list, never "trust everything".
class WorldTrustStore
{
public:
    explicit WorldTrustStore(std::filesystem::path file);

    /// `hash` must be exactly 64 lowercase hex characters; anything else is
    /// never trusted (no normalisation).
    bool trusted(const std::string& hash) const;

    /// Trust `hash` and persist atomically. Non-canonical hashes are ignored.
    /// A corrupt file is overwritten.
    /// Returns true when the hash is persisted (also when already present);
    /// false when it could not be saved or the list is full.
    bool add(const std::string& hash);

    /// Default location: `data_dir()/trusted_world_bundles.json`.
    static std::filesystem::path default_path();

private:
    std::set<std::string> read_file_() const;
    std::filesystem::path file_;
    mutable std::mutex mu_;
};

enum class WorldConsent
{
    Run,
    Ask
};

/// Order: built-in scripts, trusted hash, verified sender, else Ask.
WorldConsent decide_consent(bool scripts_are_builtin, const std::string& hash, bool sender_verified,
                            const WorldTrustStore& store);

/// Only the strongest state counts: Verified. Unknown, NotVerified and
/// VerificationViolation (was verified, identity since reset) are not verified.
bool user_trust_is_verified(UserTrust trust);

} // namespace tesseract
