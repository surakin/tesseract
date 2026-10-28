#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace tesseract
{
class Client;
}

// Transport-independent half of the Linux UnifiedPush connectors. The Qt6
// (QtDBus) and GTK4 (GDBus) shells each keep only their D-Bus plumbing and
// delegate pusher bookkeeping, endpoint validation and payload parsing here.
namespace tesseract::up
{

inline constexpr const char* kPusherAppId = "im.gnomos.tesseract";
inline constexpr const char* kPusherAppName = "Tesseract";
inline constexpr const char* kPusherDeviceName = "Linux Desktop";
inline constexpr const char* kPusherLang = "en";

// Distributor registration token for an account: alphanumerics kept, every
// other character replaced with '_'.
std::string sanitize_token(const std::string& user_id);

// The endpoint comes from the UnifiedPush distributor over D-Bus and is
// untrusted. Requires an https URL with a non-empty host, no userinfo and no
// whitespace/control characters; an explicit port is kept. Path, query and
// fragment are discarded and replaced with the Matrix push gateway path.
// Returns nullopt when the endpoint is rejected.
std::optional<std::string> normalize_gateway_url(const std::string& endpoint);

// Extracts notification.room_id from a Matrix push payload; nullopt if the
// payload isn't JSON or the field is missing, non-string or empty.
std::optional<std::string> extract_push_room_id(std::string_view payload);

// Per-account state shared by both connectors.
class PusherCore
{
public:
    // Returns false if already started. Reads the persisted Notifications
    // toggle so a user who disabled push isn't silently re-registered.
    bool begin(tesseract::Client* client, const std::string& user_id);
    void end() { client_ = nullptr; }
    bool active() const { return client_ != nullptr; }
    const std::string& token() const { return token_; }

    void on_new_endpoint(const std::string& endpoint);
    void set_enabled(bool enabled);
    void remove_pusher();
    void on_message(std::string_view payload);

private:
    tesseract::Client* client_ = nullptr;
    std::string token_;
    // Last gateway URL derived from a distributor endpoint. Cached so a
    // re-enable after the user toggled notifications off can re-register the
    // pusher without waiting for a fresh distributor callback.
    std::string gateway_url_;
    bool enabled_ = true;
};

} // namespace tesseract::up
