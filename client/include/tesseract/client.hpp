#pragma once
#include "event_handler.hpp"
#include "types.hpp"

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

    // Non-copyable, movable.
    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&)                 noexcept;
    Client& operator=(Client&&)      noexcept;

    // ------------------------------------------------------------------
    // Authentication (OAuth 2.0 / Matrix Authentication Service)
    // ------------------------------------------------------------------

    /// First-phase output of begin_oauth(): the URL to open in the user's
    /// browser plus the loopback redirect URI we're listening on.
    struct OAuthFlow {
        bool        ok = false;
        std::string message;        ///< Error description if ok == false.
        std::string auth_url;       ///< Open this in the default browser.
        std::string redirect_uri;   ///< Loopback URI we'll receive the code on.

        explicit operator bool() const noexcept { return ok; }
    };

    /// Phase 1: discover the homeserver, register the client and produce the
    /// authorisation URL. Performs network I/O — call from a worker thread.
    OAuthFlow   begin_oauth(const std::string& homeserver);

    /// Phase 2: block until the user finishes authentication in the browser
    /// and the loopback listener receives the redirect, then exchange the
    /// code for tokens. Call from a worker thread.
    Result      await_oauth();

    /// Abort an in-flight OAuth flow (user closed the dialog).
    void        cancel_oauth();

    /// Open `url` in the user's default browser. Returns false if the platform
    /// helper failed; the UI can fall back to displaying the URL for copy/paste.
    static bool open_in_browser(const std::string& url);

    /// Restore a session previously saved with export_session().
    Result      restore_session(const std::string& session_json);

    /// Serialise the current session (empty on error / not logged in).
    std::string export_session() const;

    /// Log out and invalidate the current session.
    Result      logout();

    // ------------------------------------------------------------------
    // Sync loop
    // ------------------------------------------------------------------

    /// Start the background sync loop; events are delivered via `handler`.
    /// The handler pointer must remain valid until stop_sync() returns.
    void        start_sync(IEventHandler* handler);

    /// Stop the background sync loop.
    void        stop_sync();

    // ------------------------------------------------------------------
    // Rooms and messaging
    // ------------------------------------------------------------------

    std::vector<RoomInfo> list_rooms() const;

    Result               send_message(const std::string& room_id,
                                      const std::string& body);

    std::vector<Message> room_messages(const std::string& room_id,
                                       std::size_t        limit) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tesseract
