#![recursion_limit = "256"]

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
        pub id:                String,
        pub name:              String,
        pub topic:             String,
        pub unread_count:      u64,
        pub is_direct:         bool,
        pub avatar_url:        String,
        pub last_message_body: String,
        pub last_activity_ts:  u64,
        pub is_space:          bool,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct ReactionGroup {
        pub key:           String,
        pub count:         u64,
        pub reacted_by_me: bool,
        pub source_json:   String,
        pub senders:       Vec<String>,
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
        pub source_json:       String,
        pub width:             u64,
        pub height:            u64,
        pub file_json:         String,
        pub file_name:         String,
        pub file_size:         u64,
        pub image_filename:    String,
        pub reactions:         Vec<ReactionGroup>,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct OpResult {
        pub ok:      bool,
        pub message: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct PaginateResult {
        pub ok:            bool,
        pub message:       String,
        pub reached_start: bool,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct OAuthBegin {
        pub ok:           bool,
        pub message:      String,
        pub auth_url:     String,
        pub redirect_uri: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct BackupProgress {
        pub state:         u8,
        pub imported_keys: u64,
        pub total_keys:    u64,
    }

    pub struct EventHandlerBridge;
    impl EventHandlerBridge {
        pub fn on_timeline_reset(&self, _room_id: &str) {}
        pub fn on_message_event(&self, _event: &TimelineEvent) {}
        pub fn on_message_prepended(&self, _event: &TimelineEvent) {}
        pub fn on_rooms_updated(&self, _rooms: &Vec<RoomInfo>) {}
        pub fn on_error(&self, _ctx: &str, _msg: &str, _soft_logout: bool) {}
        pub fn on_session_refreshed(&self, _json: &str) {}
        pub fn on_backup_progress(&self, _progress: &BackupProgress) {}
    }
}
