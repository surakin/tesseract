pub use super::client::ClientFfi;

pub fn client_create() -> Box<ClientFfi> {
    Box::new(ClientFfi::new())
}

#[cxx::bridge(namespace = "tesseract_ffi")]
pub mod ffi {
    // -------------------------------------------------------------------------
    // Shared plain-data types
    // -------------------------------------------------------------------------

    /// Lightweight room descriptor returned by list_rooms().
    struct RoomInfo {
        id:            String,
        name:          String,
        topic:         String,
        unread_count:  u64,
        is_direct:     bool,
        /// mxc:// URI of the room avatar, empty string if none.
        avatar_url:    String,
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

    /// First-phase result of an OAuth flow.
    struct OAuthBegin {
        ok:           bool,
        message:      String,
        auth_url:     String,
        redirect_uri: String,
    }

    // -------------------------------------------------------------------------
    // C++ types that Rust calls back into
    // -------------------------------------------------------------------------
    unsafe extern "C++" {
        include!("tesseract/event_handler_bridge.h");

        type EventHandlerBridge;

        fn on_message_event(self: &EventHandlerBridge, event: &TimelineEvent);
        fn on_rooms_updated(self: &EventHandlerBridge, rooms: &Vec<RoomInfo>);
        fn on_error(self: &EventHandlerBridge, context: &str, message: &str);
        fn on_session_refreshed(self: &EventHandlerBridge, session_json: &str);
        /// Fired when a room's timeline is reset (room selected / subscribed).
        /// The UI should clear its message view for this room_id.
        fn on_timeline_reset(self: &EventHandlerBridge, room_id: &str);
    }

    // -------------------------------------------------------------------------
    // Rust-owned opaque type exposed to C++
    // -------------------------------------------------------------------------
    extern "Rust" {
        type ClientFfi;

        fn client_create() -> Box<ClientFfi>;

        // ----- OAuth -----

        fn oauth_begin(self: &mut ClientFfi, homeserver: &str) -> OAuthBegin;
        fn oauth_await_callback(self: &mut ClientFfi) -> OpResult;
        fn oauth_cancel(self: &mut ClientFfi);

        // ----- Session -----

        fn restore_session(self: &mut ClientFfi, session_json: &str) -> OpResult;
        fn export_session(self: &ClientFfi) -> String;

        // ----- Sync -----

        fn start_sync(self: &mut ClientFfi, handler: UniquePtr<EventHandlerBridge>);
        fn stop_sync(self: &mut ClientFfi);

        // ----- Room list -----

        fn list_rooms(self: &ClientFfi) -> Vec<RoomInfo>;

        // ----- Timeline subscription (Step 2) -----

        /// Subscribe to a room's timeline. Emits on_timeline_reset then
        /// on_message_event for each cached item, and on_message_event for
        /// every subsequent live update. Call paginate_back to load history.
        fn subscribe_room(self: &mut ClientFfi, room_id: &str) -> OpResult;

        /// Unsubscribe from a room's timeline and cancel its background task.
        fn unsubscribe_room(self: &mut ClientFfi, room_id: &str);

        /// Paginate backwards in a subscribed room's timeline. New items
        /// arrive via on_message_event callbacks in oldest-first order.
        fn paginate_back(self: &mut ClientFfi, room_id: &str, count: u16) -> OpResult;

        // ----- Messaging -----

        fn send_message(self: &mut ClientFfi, room_id: &str, body: &str) -> OpResult;

        // ----- Media -----

        /// Download the avatar image for a room and return the raw bytes
        /// (typically JPEG or PNG). Returns an empty Vec when no avatar is set
        /// or the download fails. The SDK media cache is consulted first so
        /// subsequent calls are instant.
        fn fetch_avatar_bytes(self: &mut ClientFfi, room_id: &str) -> Vec<u8>;

        // ----- Session teardown -----

        fn logout(self: &mut ClientFfi) -> OpResult;
    }
}
