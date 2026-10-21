//! Wire types of the MatrixRTC protocol (MSC4143 / MSC4196 / MSC4195 / MSC4075),
//! with the unstable names and the reference-implementation spellings that
//! matrix-js-sdk still uses. Plain serde: sending and receiving go through
//! the callers, these only parse and validate content.

use serde::{Deserialize, Serialize};
use serde_json::Value;

use super::signaling::{RtcKeyContent, RtcKeyMember, RtcKeySession};

pub const SLOT_TYPE: &str = "org.matrix.msc4143.rtc.slot";
pub const MEMBER_TYPE: &str = "org.matrix.msc4143.rtc.member";
/// Unstable name of `m.rtc.invite` (an earlier iteration used this first).
pub const INVITE_TYPE: &str = "org.matrix.msc4075.rtc.notification";
pub const DECLINE_TYPE: &str = "org.matrix.msc4310.rtc.decline";
/// Unstable name of the `m.rtc.encryption_key` to-device event.
pub const KEY_TYPE: &str = "io.element.call.encryption_keys";
pub const CALL_SLOT_ID: &str = "m.call#room";

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SlotStatus {
    Open,
    Closed,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct AppRef {
    #[serde(rename = "type")]
    pub kind: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EncryptionRef {
    #[serde(rename = "type")]
    pub kind: String,
}

/// `m.rtc.slot` content.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SlotContent {
    pub status: SlotStatus,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub application: Option<AppRef>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub encryption: Option<EncryptionRef>,
}

impl SlotContent {
    /// MSC4143: a slot without a valid application object is closed.
    pub fn is_open(&self) -> bool {
        self.status == SlotStatus::Open && self.application.is_some()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Membership {
    Join,
    Leave,
}

fn default_join() -> Membership {
    Membership::Join
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallIntent {
    Audio,
    Video,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemberId {
    pub id: String,
    /// Absent on joins from older implementations; that means `join`.
    #[serde(default = "default_join")]
    pub membership: Membership,
    /// Reference-implementation extras (the MSC itself has no such fields).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub user_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub device_id: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CallApplication {
    #[serde(rename = "type")]
    pub kind: String,
    /// `m.audio` / `m.video` (MSC4196).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub intent: Option<String>,
    /// `audio` / `video` (reference implementation).
    #[serde(default, rename = "m.call.intent", skip_serializing_if = "Option::is_none")]
    pub m_call_intent: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub capabilities: Option<Vec<String>>,
}

impl CallApplication {
    pub fn intent(&self) -> Option<CallIntent> {
        match self.intent.as_deref().or(self.m_call_intent.as_deref())? {
            "m.audio" | "audio" => Some(CallIntent::Audio),
            "m.video" | "video" => Some(CallIntent::Video),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct PublishedTransport {
    #[serde(rename = "type", default)]
    pub kind: String,
    /// The SFU WebSocket URL (MSC4195).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    /// The lk-jwt-service base URL (reference-implementation spelling).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub livekit_service_url: Option<String>,
    /// Set when the member publishes on another homeserver's SFU.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub server_name: Option<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Transports {
    #[serde(default)]
    pub published: Vec<PublishedTransport>,
    #[serde(default)]
    pub can_subscribe: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LeaveReason {
    pub code: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

/// `m.rtc.member` content (join or leave).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemberContent {
    #[serde(default)]
    pub slot_id: String,
    pub member: MemberId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub application: Option<CallApplication>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub transports: Option<Transports>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub leave_reason: Option<LeaveReason>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sticky_key: Option<String>,
    /// The unstable name of `sticky_key`; senders may include both.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub msc4354_sticky_key: Option<String>,
}

impl MemberContent {
    pub fn is_join(&self) -> bool {
        self.member.membership == Membership::Join
    }

    pub fn sticky_key(&self) -> Option<&str> {
        self.sticky_key.as_deref().or(self.msc4354_sticky_key.as_deref())
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Mentions {
    #[serde(default)]
    pub user_ids: Vec<String>,
    #[serde(default)]
    pub room: bool,
}

/// `m.rtc.invite` content (MSC4075 rewrite). Old-style ring notifications
/// (`notification_type`, no `slot_id`) deliberately fail to parse.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct InviteContent {
    pub slot_id: String,
    pub sender_ts: u64,
    pub lifetime: u64,
    #[serde(rename = "m.mentions")]
    pub mentions: Mentions,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sticky_key: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub msc4354_sticky_key: Option<String>,
}

impl InviteContent {
    /// An invite must target a set of users or the room.
    pub fn validate(&self) -> Result<(), String> {
        if !self.mentions.room && self.mentions.user_ids.is_empty() {
            return Err("invite has no target in m.mentions".to_owned());
        }
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Reference {
    pub rel_type: String,
    pub event_id: String,
}

/// `m.rtc.decline` content.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeclineContent {
    #[serde(rename = "m.relates_to")]
    pub relates_to: Reference,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sticky_key: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub msc4354_sticky_key: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MediaKey {
    pub index: u8,
    pub key: String,
}

/// `m.rtc.encryption_key` to-device content, in the stable shape and/or
/// Element's legacy `keys` / `member` / `session` shape.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct MediaKeyContent {
    pub room_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub member_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub media_key: Option<MediaKey>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub keys: Option<RtcKeyContent>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub member: Option<RtcKeyMember>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session: Option<RtcKeySession>,
}

impl MediaKeyContent {
    /// `(index, base64 key)` from whichever shape is present.
    pub fn media_key_parts(&self) -> Option<(u8, String)> {
        if let Some(k) = &self.media_key {
            return Some((k.index, k.key.clone()));
        }
        self.keys.as_ref().map(|k| (k.index, k.key.clone()))
    }
}

/// Whether a transport `type` names LiveKit: `livekit` (reference
/// implementation), `m.livekit` (MSC4195) or a registry-prefixed unstable
/// form such as `msc4195.livekit`.
pub fn transport_is_livekit(kind: &str) -> bool {
    kind == "livekit" || kind.ends_with(".livekit")
}

/// The sticky key of an event's content (`sticky_key` or the unstable name).
pub fn sticky_key_of(content: &Value) -> Option<String> {
    content
        .get("sticky_key")
        .or_else(|| content.get("msc4354_sticky_key"))
        .and_then(Value::as_str)
        .map(str::to_owned)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn slot_from_4196_example_is_open() {
        let c: SlotContent = serde_json::from_value(json!({
            "status": "open",
            "application": { "type": "m.call" },
            "encryption": { "type": "m.per_member" }
        }))
        .unwrap();
        assert!(c.is_open());
        assert_eq!(c.encryption.unwrap().kind, "m.per_member");
    }

    #[test]
    fn slot_without_application_counts_as_closed() {
        let c: SlotContent = serde_json::from_value(json!({ "status": "open" })).unwrap();
        assert!(!c.is_open());
        let c: SlotContent = serde_json::from_value(json!({ "status": "closed" })).unwrap();
        assert!(!c.is_open());
    }

    #[test]
    fn join_member_from_the_msc_examples() {
        let c: MemberContent = serde_json::from_value(json!({
            "slot_id": "m.call#room",
            "member": { "id": "xyzABCDEF0123", "membership": "join",
                        "user_id": "@alice:alice.com", "device_id": "DEV" },
            "application": { "type": "m.call", "intent": "m.audio",
                             "capabilities": ["m.render_audio", "m.render_video"] },
            "transports": {
                "published": [{ "type": "livekit", "url": "wss://livekit.alice.com",
                                "livekit_service_url": "https://jwt.alice.com",
                                "server_name": "bob.com" }],
                "can_subscribe": ["livekit"]
            },
            "sticky_key": "xyzABCDEF0123"
        }))
        .unwrap();
        assert!(c.is_join());
        assert_eq!(c.member.id, "xyzABCDEF0123");
        assert_eq!(c.sticky_key.as_deref(), Some("xyzABCDEF0123"));
        let app = c.application.as_ref().unwrap();
        assert_eq!(app.intent(), Some(CallIntent::Audio));
        assert_eq!(app.capabilities.as_deref().unwrap().len(), 2);
        let t = &c.transports.as_ref().unwrap().published[0];
        assert_eq!(t.url.as_deref(), Some("wss://livekit.alice.com"));
        assert_eq!(t.server_name.as_deref(), Some("bob.com"));
        assert_eq!(c.member.user_id.as_deref(), Some("@alice:alice.com"));
    }

    #[test]
    fn reference_implementation_intent_spelling_is_understood() {
        let c: MemberContent = serde_json::from_value(json!({
            "slot_id": "m.call#ROOM",
            "member": { "id": "m", "user_id": "@a:x", "device_id": "D" },
            "application": { "type": "m.call", "m.call.intent": "video" },
            "msc4354_sticky_key": "m"
        }))
        .unwrap();
        assert!(c.is_join(), "membership defaults to join");
        assert_eq!(c.application.as_ref().unwrap().intent(), Some(CallIntent::Video));
        assert_eq!(c.sticky_key(), Some("m"));
    }

    #[test]
    fn leave_member_with_reason() {
        let c: MemberContent = serde_json::from_value(json!({
            "slot_id": "m.call#room",
            "member": { "id": "m1", "membership": "leave" },
            "leave_reason": { "code": "delayed_leave" },
            "sticky_key": "m1"
        }))
        .unwrap();
        assert!(!c.is_join());
        assert_eq!(c.leave_reason.unwrap().code, "delayed_leave");
    }

    #[test]
    fn legacy_leave_without_slot_id_parses() {
        let c: MemberContent = serde_json::from_value(json!({
            "member": { "id": "m1", "membership": "leave" }
        }))
        .unwrap();
        assert!(c.slot_id.is_empty());
        assert!(!c.is_join());
    }

    #[test]
    fn invite_from_the_msc_example() {
        let c: InviteContent = serde_json::from_value(json!({
            "slot_id": "m.call#room",
            "sender_ts": 1784493900000u64,
            "lifetime": 90000,
            "m.mentions": { "user_ids": ["@alice:example.org"] },
            "sticky_key": "m.call#room"
        }))
        .unwrap();
        assert!(c.validate().is_ok());
        assert_eq!(c.lifetime, 90_000);
    }

    #[test]
    fn invite_without_mentions_is_invalid() {
        let c: InviteContent = serde_json::from_value(json!({
            "slot_id": "m.call#room", "sender_ts": 1, "lifetime": 1,
            "m.mentions": {}
        }))
        .unwrap();
        assert!(c.validate().is_err());
        let room: InviteContent = serde_json::from_value(json!({
            "slot_id": "m.call#room", "sender_ts": 1, "lifetime": 1,
            "m.mentions": { "room": true }
        }))
        .unwrap();
        assert!(room.validate().is_ok());
    }

    #[test]
    fn legacy_notification_is_not_an_invite() {
        let legacy = json!({
            "notification_type": "ring", "sender_ts": 1, "lifetime": 30000,
            "m.mentions": { "room": true }
        });
        assert!(serde_json::from_value::<InviteContent>(legacy).is_err());
    }

    #[test]
    fn decline_from_the_msc_example() {
        let c: DeclineContent = serde_json::from_value(json!({
            "m.relates_to": { "rel_type": "m.reference", "event_id": "$1" },
            "sticky_key": "$1"
        }))
        .unwrap();
        assert_eq!(c.relates_to.event_id, "$1");
    }

    #[test]
    fn key_event_in_the_stable_shape() {
        let c: MediaKeyContent = serde_json::from_value(json!({
            "room_id": "!r:x", "member_id": "m1",
            "media_key": { "index": 3, "key": "AAAA" }
        }))
        .unwrap();
        assert_eq!(c.media_key_parts(), Some((3, "AAAA".to_owned())));
    }

    #[test]
    fn key_event_in_element_s_legacy_shape() {
        let c: MediaKeyContent = serde_json::from_value(json!({
            "room_id": "!r:x",
            "keys": { "key": "BBBB", "index": 7 },
            "member": { "claimed_device_id": "DEV", "id": "@a:x:DEV" },
            "session": { "call_id": "", "application": "m.call", "scope": "m.room" }
        }))
        .unwrap();
        assert_eq!(c.media_key_parts(), Some((7, "BBBB".to_owned())));
        assert!(c.member_id.is_none());
    }

    #[test]
    fn transport_kind_variants() {
        for k in ["livekit", "m.livekit", "msc4195.livekit", "org.matrix.msc4195.livekit"] {
            assert!(transport_is_livekit(k), "{k}");
        }
        for k in ["", "webrtc", "livekit2", "notlivekit"] {
            assert!(!transport_is_livekit(k), "{k}");
        }
    }

    #[test]
    fn sticky_key_is_read_from_either_name() {
        assert_eq!(sticky_key_of(&json!({"sticky_key": "a"})).as_deref(), Some("a"));
        assert_eq!(sticky_key_of(&json!({"msc4354_sticky_key": "b"})).as_deref(), Some("b"));
        assert_eq!(sticky_key_of(&json!({"x": 1})), None);
    }
}
