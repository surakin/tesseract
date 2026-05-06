mod client;
mod oauth;

/// The cxx bridge declares the shared types and functions between Rust and C++.
/// Rust-side async operations are driven by an embedded tokio runtime so C++
/// sees a purely synchronous API.
#[cxx::bridge(namespace = "tesseract_ffi")]
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

    /// First-phase result of an OAuth flow: contains the URL the C++ side must
    /// open in the user's browser, plus the redirect URI we'll listen on.
    /// `await_oauth_callback` should be called next to block until the
    /// authorisation code is delivered back to the loopback listener.
    struct OAuthBegin {
        ok:           bool,
        message:      String,
        auth_url:     String,
        redirect_uri: String,
    }

    // -------------------------------------------------------------------------
    // C++ types that Rust calls back into (event notifications)
    // -------------------------------------------------------------------------
    unsafe extern "C++" {
        include!("tesseract/event_handler_bridge.hpp");

        type EventHandlerBridge;

        fn on_sync_token(self: &EventHandlerBridge, token: &str);
        fn on_message_event(self: &EventHandlerBridge, event: &TimelineEvent);
        fn on_rooms_updated(self: &EventHandlerBridge, rooms: &Vec<RoomInfo>);
        fn on_error(self: &EventHandlerBridge, context: &str, message: &str);
        /// Fired whenever the SDK rotates the OAuth tokens. The argument is
        /// the freshly-serialised session JSON; the UI must persist it so the
        /// next launch can `restore_session()`.
        fn on_session_refreshed(self: &EventHandlerBridge, session_json: &str);
    }

    // -------------------------------------------------------------------------
    // Rust-owned opaque type exposed to C++
    // -------------------------------------------------------------------------
    extern "Rust" {
        type ClientFfi;

        /// Allocate a new client (no network activity yet).
        fn client_create() -> Box<ClientFfi>;

        // ----- OAuth login (modern Matrix Authentication Service flow) -----

        /// Phase 1: discover the homeserver, register the OAuth client, start
        /// a localhost redirect listener, and produce the URL to open in the
        /// user's browser. Blocks on network I/O but does not wait for the
        /// callback.
        fn oauth_begin(
            self: &mut ClientFfi,
            homeserver: &str,
        ) -> OAuthBegin;

        /// Phase 2: block until the loopback listener receives the redirect,
        /// then exchange the code for tokens. Must be paired with a prior
        /// successful `oauth_begin`. Caller should run this on a worker
        /// thread so the UI doesn't freeze.
        fn oauth_await_callback(self: &mut ClientFfi) -> OpResult;

        /// Abort an in-flight OAuth flow (user closed the dialog).
        fn oauth_cancel(self: &mut ClientFfi);

        /// Restore a session from the JSON string saved by export_session().
        fn restore_session(self: &mut ClientFfi, session_json: &str) -> OpResult;

        /// Serialise the current session to JSON (empty on error / not logged in).
        fn export_session(self: &ClientFfi) -> String;

        /// Start the sync loop; callbacks are delivered via `handler`.
        fn start_sync(self: &mut ClientFfi, handler: UniquePtr<EventHandlerBridge>);

        /// Stop the sync loop.
        fn stop_sync(self: &mut ClientFfi);

        /// Return the list of joined rooms.
        fn list_rooms(self: &ClientFfi) -> Vec<RoomInfo>;

        /// Send a plain-text message to a room.
        fn send_message(self: &mut ClientFfi, room_id: &str, body: &str) -> OpResult;

        /// Fetch recent messages from a room (newest-first; returns empty until timeline is cached).
        fn room_messages(self: &ClientFfi, room_id: &str, limit: u64) -> Vec<TimelineEvent>;

        /// Revoke OAuth tokens at the MAS, drop the in-memory client, and
        /// remove the on-disk SQLite store. Caller is also expected to delete
        /// any persisted session JSON it owns (see SessionStore on the C++ side).
        fn logout(self: &mut ClientFfi) -> OpResult;
    }
}

pub use client::ClientFfi;

pub fn client_create() -> Box<ClientFfi> {
    Box::new(ClientFfi::new())
}
