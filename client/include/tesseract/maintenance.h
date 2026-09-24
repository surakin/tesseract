#pragma once

#include <string>
#include <vector>

namespace tesseract
{

/// Result of signing out one stored account.
struct LogoutOutcome
{
    std::string user_id;
    /// True when the homeserver accepted the logout (the device and its
    /// tokens are revoked server-side). The local copy is wiped either way.
    bool server_ok = false;
    /// Failure detail from restore/logout when !server_ok.
    std::string error;
};

struct LogoutAllReport
{
    /// accounts.json exists but could not be read. Nothing was touched —
    /// an unreadable index must never be treated as "no accounts".
    bool index_corrupt = false;
    std::vector<LogoutOutcome> accounts;
};

/// Headless `--logoutall`: for every account in the active profile's index,
/// restore its session, log it out on the homeserver, then wipe its local
/// state (session, secret-store entry, matrix-sdk store) and finally persist
/// an empty index. Blocking; runs without any UI. The caller must hold the
/// profile's single-instance lock so no running instance has the stores
/// open.
LogoutAllReport logout_all_accounts();

} // namespace tesseract
