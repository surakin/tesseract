#![recursion_limit = "256"]

mod client;
mod image_packs;
mod oauth;
mod recent_emoji;
mod waveform;
mod waveform_store;

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
        pub id: String,
        pub name: String,
        pub topic: String,
        pub topic_html: String,
        pub notification_count: u64,
        pub highlight_count: u64,
        pub is_direct: bool,
        pub avatar_url: String,
        pub dm_avatar_url: String,
        pub dm_counterpart_user_id: String,
        pub last_message_body: String,
        pub last_message_sender_name: String,
        pub last_message_kind: String,
        pub last_message_sticker_url: String,
        pub last_message_thumbnail_url: String,
        pub last_activity_ts: u64,
        pub is_space: bool,
        pub is_favorite: bool,
        pub is_encrypted: bool,
        pub history_visibility: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct ReactionGroup {
        pub key: String,
        pub count: u64,
        pub reacted_by_me: bool,
        pub source_url: String,
        pub senders: Vec<String>,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct ReadReceipt {
        pub user_id: String,
        pub display_name: String,
        pub avatar_url: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct RoomMember {
        pub user_id: String,
        pub display_name: String,
        pub avatar_url: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct TimelineEvent {
        pub event_id: String,
        pub room_id: String,
        pub sender: String,
        pub sender_name: String,
        pub sender_avatar_url: String,
        pub body: String,
        pub timestamp: u64,
        pub msg_type: String,
        pub source_url: String,
        pub source_encrypted_json: String,
        pub width: u64,
        pub height: u64,
        pub file_url: String,
        pub file_encrypted_json: String,
        pub file_name: String,
        pub file_size: u64,
        pub image_filename: String,
        pub audio_url: String,
        pub audio_encrypted_json: String,
        pub audio_duration_ms: u64,
        pub audio_waveform: Vec<u16>,
        pub audio_mime: String,
        pub video_thumbnail_url: String,
        pub video_thumbnail_encrypted_json: String,
        pub image_thumbnail_url: String,
        pub image_thumbnail_encrypted_json: String,
        pub video_duration_ms: u64,
        pub video_mime: String,
        pub video_autoplay: bool,
        pub video_loop: bool,
        pub video_no_audio: bool,
        pub video_hide_controls: bool,
        pub video_gif: bool,
        pub reactions: Vec<ReactionGroup>,
        pub read_receipts: Vec<ReadReceipt>,
        pub in_reply_to_id: String,
        pub in_reply_to_sender_name: String,
        pub in_reply_to_body: String,
        pub is_edited: bool,
        pub formatted_body: String,
        pub blurhash: String,
        pub sticker_info_json: String,
        pub image_animated: bool,
        pub pending_state: String,
        pub pending_error: String,
        pub pending_recoverable: bool,
        pub pending_txn_id: String,
        pub location_lat: f64,
        pub location_lon: f64,
        pub location_description: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct OpResult {
        pub ok: bool,
        pub message: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct PaginateResult {
        pub ok: bool,
        pub message: String,
        pub reached_start: bool,
        pub reached_end: bool,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct OAuthBegin {
        pub ok: bool,
        pub message: String,
        pub auth_url: String,
        pub redirect_uri: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct BackupProgress {
        pub state: u8,
        pub imported_keys: u64,
        pub total_keys: u64,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct ImagePackFfi {
        pub id: String,
        pub display_name: String,
        pub avatar_url: String,
        pub attribution: String,
        pub usage_mask: u8,
        pub source_kind: String,
        pub source_room: String,
        pub source_state_key: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct ImageEntryFfi {
        pub pack_id: String,
        pub shortcode: String,
        pub url: String,
        pub body: String,
        pub info_json: String,
        pub usage_mask: u8,
        pub favorite: bool,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct DeviceFfi {
        pub device_id: String,
        pub display_name: String,
        pub last_seen_ip: String,
        pub last_seen_ts: u64,
        pub verification_state: u8,
        pub is_current: bool,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct DeleteDeviceBegin {
        pub ok: bool,
        pub message: String,
        pub needs_uia: bool,
        pub fallback_url: String,
        pub session: String,
    }

    pub struct EventHandlerBridge;
    impl EventHandlerBridge {
        pub fn on_timeline_reset(&self, _room_id: &str, _snapshot: &Vec<TimelineEvent>) {}
        pub fn on_message_inserted(&self, _room_id: &str, _index: u64, _event: &TimelineEvent) {}
        pub fn on_message_updated(&self, _room_id: &str, _index: u64, _event: &TimelineEvent) {}
        pub fn on_message_removed(&self, _room_id: &str, _index: u64) {}
        pub fn on_rooms_updated(&self, _rooms: &Vec<RoomInfo>) {}
        pub fn on_error(&self, _ctx: &str, _msg: &str, _soft_logout: bool) {}
        pub fn on_session_refreshed(&self, _json: &str) {}
        pub fn on_backup_progress(&self, _progress: &BackupProgress) {}
        pub fn on_image_packs_updated(&self) {}
        pub fn on_account_prefs_updated(&self, _json: &str) {}
        pub fn on_notification(
            &self,
            _room_id: &str,
            _room_name: &str,
            _sender: &str,
            _body: &str,
            _is_mention: bool,
        ) {
        }
    }
}
