#pragma once
#include "event_handler.hpp"
#include "types.hpp"

#include <memory>
#include <string>
#include <vector>

// Forward-declare the Rust opaque type so we don't need to pull in cxx headers
// in every translation unit that just uses this interface.
namespace matrix_ffi { class MatrixClientFfi; }

namespace matrix {

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
class MatrixClient {
public:
    MatrixClient();
    ~MatrixClient();

    // Non-copyable, movable.
    MatrixClient(const MatrixClient&)            = delete;
    MatrixClient& operator=(const MatrixClient&) = delete;
    MatrixClient(MatrixClient&&)                 noexcept;
    MatrixClient& operator=(MatrixClient&&)      noexcept;

    // ------------------------------------------------------------------
    // Authentication
    // ------------------------------------------------------------------

    /// Connect with a username + password.
    Result login(const std::string& homeserver,
                 const std::string& username,
                 const std::string& password);

    /// Restore a session previously saved with export_session().
    Result restore_session(const std::string& session_json);

    /// Serialise the current session (empty on error / not logged in).
    std::string export_session() const;

    /// Log out and invalidate the server-side session.
    Result logout();

    // ------------------------------------------------------------------
    // Sync
    // ------------------------------------------------------------------

    /// Start the background sync loop.  handler must outlive stop_sync().
    void start_sync(IEventHandler* handler);

    /// Stop the background sync loop and release the handler reference.
    void stop_sync();

    // ------------------------------------------------------------------
    // Rooms & messages
    // ------------------------------------------------------------------

    std::vector<RoomInfo> list_rooms() const;

    std::vector<Message>  room_messages(const std::string& room_id,
                                        uint64_t            limit = 50) const;

    Result send_message(const std::string& room_id,
                        const std::string& body);

private:
    // rust::Box<matrix_ffi::MatrixClientFfi> — stored as void* to keep cxx out
    // of this public header.  Properly typed in client.cpp.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace matrix
