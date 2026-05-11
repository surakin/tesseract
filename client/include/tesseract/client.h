#pragma once
#include "event_handler.h"
#include "types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward-declare the Rust opaque type so we don't need to pull in cxx headers
// in every translation unit that just uses this interface.
namespace tesseract_ffi { class ClientFfi; }

namespace tesseract {

/// Result of a client operation.
struct Result {
    bool        ok      = false;
    std::string message;

    explicit operator bool() const noexcept { return ok; }
};

/// High-level C++ Matrix client.
///
/// Thread-safety: methods may be called from any thread; the underlying Rust
/// runtime serialises concurrent calls.  Event callbacks arrive on a background
/// thread – marshal them to the UI thread as required.
class Client {
public:
    Client();
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&)                 noexcept;
    Client& operator=(Client&&)      noexcept;

    // ------------------------------------------------------------------
    // Authentication (OAuth 2.0 / Matrix Authentication Service)
    // ------------------------------------------------------------------

    struct OAuthFlow {
        bool        ok = false;
        std::string message;
        std::string auth_url;
        std::string redirect_uri;

        explicit operator bool() const noexcept { return ok; }
    };

    OAuthFlow   begin_oauth(const std::string& homeserver);
    Result      await_oauth();
    void        cancel_oauth();

    static bool open_in_browser(const std::string& url);

    Result      restore_session(const std::string& session_json);
    std::string export_session() const;
    Result      logout();

    // ------------------------------------------------------------------
    // Sync loop
    // ------------------------------------------------------------------

    void start_sync(IEventHandler* handler);
    void stop_sync();

    // ------------------------------------------------------------------
    // Room list (utility — push pipeline drives UI updates via callbacks)
    // ------------------------------------------------------------------

    std::vector<RoomInfo> list_rooms() const;

    // ------------------------------------------------------------------
    // Timeline subscription (Step 2)
    // ------------------------------------------------------------------

    /// Subscribe to a room's timeline. Emits on_timeline_reset then
    /// on_message for cached items, followed by live on_message callbacks.
    /// Call paginate_back() immediately after to load message history.
    Result subscribe_room(const std::string& room_id);

    /// Unsubscribe from a room's timeline and cancel its background task.
    void   unsubscribe_room(const std::string& room_id);

    /// Paginate backwards in a subscribed room. History arrives via
    /// on_message callbacks in oldest-first order.
    Result paginate_back(const std::string& room_id, std::uint16_t count);

    // ------------------------------------------------------------------
    // Messaging
    // ------------------------------------------------------------------

    Result send_message(const std::string& room_id, const std::string& body);

    // ------------------------------------------------------------------
    // Identity
    // ------------------------------------------------------------------

    /// Returns the current user's Matrix ID, e.g. @alice:example.org.
    /// Returns an empty string when not logged in.
    std::string get_user_id() const;

    // ------------------------------------------------------------------
    // Media
    // ------------------------------------------------------------------

    /// Download the avatar image for the given room and return the raw bytes
    /// (JPEG or PNG). Returns an empty vector when no avatar is set or on
    /// error. The SDK's SQLite media cache is consulted first, so repeat
    /// calls for the same avatar are instant.
    std::vector<uint8_t> fetch_avatar_bytes(const std::string& room_id);

    /// Download arbitrary mxc:// media and return the raw bytes. Returns an
    /// empty vector when the URL is invalid, the media is unavailable, or the
    /// client is not logged in. The SDK's SQLite media cache is consulted
    /// first, so repeat calls for the same URL are instant.
    std::vector<uint8_t> fetch_media_bytes(const std::string& mxc_url);

    // ------------------------------------------------------------------
    // Spaces
    // ------------------------------------------------------------------

    /// Returns the room IDs of all direct children of a space (via
    /// `m.space.child` state events). Only returns IDs of rooms the
    /// client is a member of. Returns an empty vector when not logged in
    /// or when `space_id` is not a known space.
    std::vector<std::string> space_children(const std::string& space_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tesseract
