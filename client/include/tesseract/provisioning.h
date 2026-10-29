#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace tesseract
{

// Deployment-provisioned login defaults, written by an installer or an admin
// rather than by the user. Two values: the homeserver ("DefaultHomeserver" /
// "default_homeserver") and the username ("DefaultUsername" / "default_username",
// a localpart or a full Matrix ID). Sources, highest priority first:
//   1. <config_dir()>/provisioning.json (per user, every platform; for Windows
//      %APPDATA%\Tesseract\provisioning.json), JSON keys as above.
//   2. Windows: registry value under HKCU\Software\Tesseract-Matrix, then
//               HKLM\Software\Tesseract-Matrix (the NSIS installer's
//               /DEFAULT_HOMESERVER=<host> switch writes the HKLM homeserver).
//      macOS:   the preference of the same name (managed/MDM prefs honoured).
//      Linux:   /etc/tesseract/provisioning.json.
// Each value is resolved independently. Evaluated once per process.

// Homeserver the login view should use instead of "matrix.org", or nullopt when
// none is provisioned.
std::optional<std::string> provisioned_default_homeserver();

// Provisioned username exactly as configured (localpart or full MXID, trimmed,
// no leading '@'), or nullopt.
std::optional<std::string> provisioned_default_username();

// The provisioned username as a full Matrix ID ("@alice:example.org"): a full
// MXID is used as-is, a bare localpart is completed with the provisioned
// homeserver's server name. nullopt when it cannot be completed.
std::optional<std::string> provisioned_user_id();

// True when any login default is provisioned.
bool login_provisioned();

// Trims whitespace and trailing '/'; empty result -> nullopt.
std::optional<std::string> normalize_homeserver(std::string_view raw);

// Trims whitespace and a leading '@'; empty result -> nullopt.
std::optional<std::string> normalize_username(std::string_view raw);

// Server name of a homeserver setting: drops a scheme and any path
// ("https://matrix.example.com:8448/x" -> "matrix.example.com:8448").
std::string homeserver_server_name(std::string_view homeserver);

// Builds "@user:server" from a username (localpart or MXID) and a homeserver
// setting; nullopt when the username has no server part and `homeserver` is empty.
std::optional<std::string> make_user_id(std::string_view username,
                                        std::string_view homeserver);

struct ProvisionedValues
{
    std::optional<std::string> homeserver;
    std::optional<std::string> username;
};

// Extracts "default_homeserver" / "default_username" from provisioning.json
// text (normalized). Invalid JSON or missing/non-string keys -> nullopt fields.
ProvisionedValues parse_provisioning_values(std::string_view json_text);

// Homeserver only; see parse_provisioning_values().
std::optional<std::string> parse_provisioning_json(std::string_view json_text);

// Per-field merge: a value in `preferred` wins over the one in `fallback`.
ProvisionedValues merge_provisioned(ProvisionedValues preferred, ProvisionedValues fallback);

namespace detail
{
// Per-platform read (provisioning_<platform>.cpp); normalized by the caller.
ProvisionedValues read_platform_provisioning_raw();
} // namespace detail

} // namespace tesseract
