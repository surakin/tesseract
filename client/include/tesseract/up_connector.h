#pragma once
#include <string>

namespace tesseract
{

class Client; // forward declaration — callers include client.h themselves

/// Abstract interface for a UnifiedPush D-Bus connector.
/// Implemented by LinuxUpConnectorQt (Qt6) and LinuxUpConnectorGtk (GTK4).
/// The field is null on Windows and macOS where UnifiedPush is not supported.
class IUpConnector
{
public:
    virtual ~IUpConnector() = default;

    /// Register with a UnifiedPush distributor and begin listening for pushes.
    /// `client` must remain alive for the lifetime of this connector.
    /// `user_id` is the canonical Matrix ID (e.g. "@alice:example.org"); it is
    /// used to derive a stable per-account push token.
    virtual void start(Client* client, const std::string& user_id) = 0;

    /// Release local D-Bus state on normal shutdown. The distributor and
    /// homeserver registrations are intentionally left intact so pushes
    /// continue to arrive while the app is closed.
    virtual void stop() = 0;

    /// Unregister from the distributor and remove the homeserver pusher.
    /// Call only on explicit user logout, not on normal app exit.
    virtual void logout() = 0;

    /// Toggle whether this device's pusher is registered with the homeserver.
    /// `false` — call `Client::remove_pusher` so the homeserver stops sending
    /// pushes to this device, and suppress any future `register_pusher` calls
    /// (including those triggered by a fresh distributor endpoint) until
    /// re-enabled. `true` — re-run `register_pusher` if the connector
    /// currently has a distributor endpoint cached. The D-Bus listener stays
    /// up either way so the user can flip back on without restarting.
    virtual void set_enabled(bool enabled) = 0;
};

} // namespace tesseract
