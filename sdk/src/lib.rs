mod client;

/// The cxx bridge declares the shared types and functions between Rust and C++.
/// Rust-side async operations are driven by an embedded tokio runtime so C++
/// sees a purely synchronous API.
#[cxx::bridge(namespace = "matrix_ffi")]
pub mod ffi {
    // -------------------------------------------------------------------------
    // Shared plain-data types (zero-cost copies across the FFI boundary)
    // -------------------------------------------------------------------------

    /// Lightweight room descriptor returned by list_rooms().
    struct RoomInfo {
        id:            String,
        name:          String,
        topic:         String,
        unread_count:  u64,
        is_direct:     bool,
    }

    /// A single timeline event (message).
    struct TimelineEvent {
        event_id:  String,
        room_id:   String,
        sender:    String,
        body:      String,
        /// Unix timestamp in milliseconds.
        timestamp: u64,
        /// "m.text" | "m.image" | "m.file" | …
        msg_type:  String,
    }

    /// Outcome of an asynchronous SDK operation.
    struct OpResult {
        ok:      bool,
        message: String,
    }

    // -------------------------------------------------------------------------
    // C++ types that Rust calls back into (event notifications)
    // -------------------------------------------------------------------------
    unsafe extern "C++" {
        include!("matrix/event_handler_bridge.hpp");

        type EventHandlerBridge;

        fn on_sync_token(self: &EventHandlerBridge, token: &str);
        fn on_message_event(self: &EventHandlerBridge, event: &TimelineEvent);
        fn on_rooms_updated(self: &EventHandlerBridge, rooms: &Vec<RoomInfo>);
        fn on_error(self: &EventHandlerBridge, context: &str, message: &str);
    }

    // -------------------------------------------------------------------------
    // Rust-owned opaque type exposed to C++
    // -------------------------------------------------------------------------
    extern "Rust" {
        type MatrixClientFfi;

        /// Allocate a new client (no network activity yet).
        fn matrix_client_create() -> Box<MatrixClientFfi>;

        /// Attempt password login.  Blocks until the SDK completes the request.
        fn login_password(
            self: &mut MatrixClientFfi,
            homeserver: &str,
            username:   &str,
            password:   &str,
        ) -> OpResult;

        /// Restore a previously persisted session from a JSON blob.
        fn restore_session(self: &mut MatrixClientFfi, session_json: &str) -> OpResult;

        /// Serialise the current session to JSON (empty string if not logged in).
        fn export_session(self: &MatrixClientFfi) -> String;

        /// Start background sync; fires C++ callbacks as events arrive.
        /// Takes ownership of the handler – call stop_sync() to release it.
        fn start_sync(self: &mut MatrixClientFfi, handler: UniquePtr<EventHandlerBridge>);

        /// Signal the background sync loop to stop.
        fn stop_sync(self: &mut MatrixClientFfi);

        /// Snapshot of all joined rooms at this instant.
        fn list_rooms(self: &MatrixClientFfi) -> Vec<RoomInfo>;

        /// Fetch the last `limit` messages for a room.  Blocks.
        fn room_messages(
            self: &MatrixClientFfi,
            room_id: &str,
            limit:   u64,
        ) -> Vec<TimelineEvent>;

        /// Send a plain-text message.  Blocks.
        fn send_message(
            self: &MatrixClientFfi,
            room_id: &str,
            body:    &str,
        ) -> OpResult;

        /// Log out and invalidate the session.
        fn logout(self: &mut MatrixClientFfi) -> OpResult;
    }
}

pub use client::MatrixClientFfi;

pub fn matrix_client_create() -> Box<MatrixClientFfi> {
    Box::new(MatrixClientFfi::new())
}
