#![recursion_limit = "256"]

mod client;
mod highlight;
mod image_packs;
mod markdown;
mod matrix_uri;
mod media_preview;
mod text_utils;
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
    pub struct MarkdownFfiResult {
        pub formatted_body: String,
    }

    #[derive(Debug, PartialEq, Default, Clone)]
    pub struct RtcParticipantInfo {
        pub participant_id: String,
        pub user_id: String,
        pub device_id: String,
        pub is_audio_muted: bool,
        pub is_video_muted: bool,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct InviteInfo {
        pub room_id: String,
        pub room_name: String,
        pub room_avatar_url: String,
        pub room_topic: String,
        pub is_direct: bool,
        pub inviter_user_id: String,
        pub inviter_display_name: String,
        pub inviter_avatar_url: String,
        pub invited_at_ts: u64,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct RoomInfo {
        pub id: String,
        pub name: String,
        pub topic: String,
        pub topic_html: String,
        pub notification_count: u64,
        pub highlight_count: u64,
        pub unread_count: u64,
        pub muted: bool,
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
        pub is_low_priority: bool,
        pub is_encrypted: bool,
        pub has_active_call: bool,
        pub history_visibility: String,
        pub canonical_alias: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct HighlightSpan {
        pub text: String,
        pub r: u8,
        pub g: u8,
        pub b: u8,
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
    pub struct UserProfile {
        pub exists: bool,
        pub user_id: String,
        pub display_name: String,
        pub avatar_url: String,
        pub pronouns: String,
        pub tz: String,
        pub biography: String,
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
        pub file_filename: String,
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
        pub in_reply_to_image_url: String,
        pub in_reply_to_image_encrypted_json: String,
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
        pub thread_root_id: String,
        pub is_thread_root: bool,
        pub thread_reply_count: u64,
        pub thread_latest_sender_name: String,
        pub thread_latest_body: String,
        pub thread_latest_ts: u64,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct OpResult {
        pub ok: bool,
        pub message: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct UpdateResult {
        pub has_update: bool,
        pub version: String,
        pub url: String,
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

    #[derive(Debug, PartialEq, Default)]
    pub struct MediaPreviewConfigFfi {
        pub media_previews: u8,
        pub invite_avatars: bool,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct MediaPreviewOverrideFfi {
        pub has_media_previews: bool,
        pub media_previews: u8,
        pub join_rule: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct ThreadInfo {
        pub root_event_id: String,
        pub root_sender_name: String,
        pub root_body: String,
        pub root_timestamp: u64,
        pub latest_event_id: String,
        pub latest_sender_name: String,
        pub latest_body: String,
        pub latest_timestamp: u64,
        pub num_replies: u64,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct MatrixLinkResult {
        pub kind: u8,
        pub primary: String,
        pub event_id: String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct UrlSpan {
        pub start: usize,
        pub end:   usize,
        pub url:   String,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct QrGrantBitmap {
        pub ok: bool,
        pub message: String,
        pub pixels: Vec<u8>,
        pub side: u32,
    }

    #[derive(Debug, PartialEq, Default)]
    pub struct QrGrantAuth {
        pub ok: bool,
        pub message: String,
        pub verification_uri: String,
    }

    pub struct EventHandlerBridge;
    impl EventHandlerBridge {
        pub fn on_timeline_reset(&self, _room_id: &str, _snapshot: &Vec<TimelineEvent>) {}
        pub fn on_message_inserted(&self, _room_id: &str, _index: u64, _event: &TimelineEvent) {}
        pub fn on_message_updated(&self, _room_id: &str, _index: u64, _event: &TimelineEvent) {}
        pub fn on_message_removed(&self, _room_id: &str, _index: u64) {}
        pub fn on_thread_reset(&self, _room_id: &str, _thread_root: &str, _snapshot: &Vec<TimelineEvent>) {}
        pub fn on_thread_inserted(&self, _room_id: &str, _thread_root: &str, _index: u64, _event: &TimelineEvent) {}
        pub fn on_thread_updated(&self, _room_id: &str, _thread_root: &str, _index: u64, _event: &TimelineEvent) {}
        pub fn on_thread_removed(&self, _room_id: &str, _thread_root: &str, _index: u64) {}
        pub fn on_rooms_updated(&self, _rooms: &Vec<RoomInfo>) {}
        pub fn on_invites_updated(&self, _invites: &Vec<InviteInfo>) {}
        pub fn on_error(&self, _ctx: &str, _msg: &str, _soft_logout: bool) {}
        pub fn on_session_refreshed(&self, _json: &str) {}
        pub fn on_backup_progress(&self, _progress: &BackupProgress) {}
        pub fn on_enable_recovery_progress(
            &self,
            _step: u8,
            _recovery_key: &str,
            _backed_up: u32,
            _total: u32,
        ) {}
        pub fn on_image_packs_updated(&self) {}
        pub fn on_account_prefs_updated(&self, _json: &str) {}
        pub fn on_media_preview_config_updated(&self, _json: &str) {}
        pub fn on_threads_updated(&self, _room_id: &str) {}
        pub fn on_notification(
            &self,
            _room_id: &str,
            _room_name: &str,
            _sender: &str,
            _body: &str,
            _is_mention: bool,
        ) {
        }
        pub fn on_rtc_invitation(
            &self,
            _room_id: &str,
            _slot_id: &str,
            _caller_user_id: &str,
            _call_intent: &str,
            _lifetime_ms: u64,
            _notification_event_id: &str,
        ) {}
        pub fn on_rtc_participant_joined(&self, _session_id: u64, _info: &RtcParticipantInfo) {}
        pub fn on_rtc_participant_left(&self, _session_id: u64, _participant_id: &str) {}
        pub fn on_rtc_participant_updated(&self, _session_id: u64, _info: &RtcParticipantInfo) {}
        pub fn on_rtc_session_ended(&self, _session_id: u64, _reason: &str) {}
        pub fn on_rtc_video_frame(
            &self,
            _session_id: u64,
            _participant_id: &str,
            _width: u32,
            _height: u32,
            _rgba: &[u8],
        ) {
        }
        pub fn on_rtc_audio_frame(
            &self,
            _session_id: u64,
            _participant_id: &str,
            _samples: &[i16],
            _sample_rate: u32,
            _num_channels: u32,
        ) {
        }
    }

    pub fn persist_session(_user_id: &str, _session_json: &str) {}
}
