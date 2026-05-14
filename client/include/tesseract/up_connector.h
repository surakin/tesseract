#pragma once
#include <string>

namespace tesseract {

class Client; // forward declaration — callers include client.h themselves

/// Abstract interface for a UnifiedPush D-Bus connector.
/// Implemented by LinuxUpConnectorQt (Qt6) and LinuxUpConnectorGtk (GTK4).
/// The field is null on Windows and macOS where UnifiedPush is not supported.
class IUpConnector {
public:
    virtual ~IUpConnector() = default;

    /// Register with a UnifiedPush distributor and begin listening for pushes.
    /// `client` must remain alive for the lifetime of this connector.
    /// `user_id` is the canonical Matrix ID (e.g. "@alice:example.org"); it is
    /// used to derive a stable per-account push token.
    virtual void start(Client* client, const std::string& user_id) = 0;

    /// Unregister from the distributor and tear down the D-Bus listener.
    virtual void stop() = 0;
};

} // namespace tesseract
