mod client;
mod oauth;

// Production cxx bridge — skipped during `cargo test` to avoid needing C++ linked.
// Tests use the pure-Rust stub module below instead.
#[cfg(not(test))]
mod bridge;
#[cfg(not(test))]
pub use bridge::ffi;

/// Pure-Rust stubs that mirror the cxx-generated shapes, compiled only during
/// `cargo test`. They let unit tests run without a C++ toolchain.
#[cfg(test)]
pub mod ffi {
    #[derive(Debug, PartialEq, Default)]
    pub struct RoomInfo {
        pub id:           String,
        pub name:         String,
        pub topic:        String,
        pub unread_count: u64,
        pub is_direct:    bool,
        pub avatar_url:   String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct TimelineEvent {
        pub event_id:          String,
        pub room_id:           String,
        pub sender:            String,
        pub sender_name:       String,
        pub sender_avatar_url: String,
        pub body:              String,
        pub timestamp:         u64,
        pub msg_type:          String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct OpResult {
        pub ok:      bool,
        pub message: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct OAuthBegin {
        pub ok:           bool,
        pub message:      String,
        pub auth_url:     String,
        pub redirect_uri: String,
    }

    pub struct EventHandlerBridge;
    impl EventHandlerBridge {
        pub fn on_timeline_reset(&self, _room_id: &str) {}
        pub fn on_message_event(&self, _event: &TimelineEvent) {}
        pub fn on_rooms_updated(&self, _rooms: &Vec<RoomInfo>) {}
        pub fn on_error(&self, _ctx: &str, _msg: &str) {}
        pub fn on_session_refreshed(&self, _json: &str) {}
    }
}
