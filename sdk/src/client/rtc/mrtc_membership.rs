//! Our side of MatrixRTC membership: opening the room's `m.call` slot, and the
//! sticky `m.rtc.member` (and DM `m.rtc.invite`) events a call sends next to
//! the legacy `org.matrix.msc3401.call.member` one.

use matrix_sdk::{ruma::events::StateEventType, Room};

use super::model::{
    AppRef, EncryptionRef, SlotContent, SlotStatus, CALL_SLOT_ID, INVITE_TYPE, MEMBER_TYPE, SLOT_TYPE,
};
use super::sticky::send_sticky;
use base64ct::{Base64UrlUnpadded, Encoding};
use rand::RngCore;
use serde_json::{json, Value};
use std::time::Duration;
use tracing::{info, warn};

/// How long one `m.rtc.member` event keeps us joined: MSC4354's 5 minute floor.
/// A client that disappears without leaving (encrypted rooms cannot get a
/// delayed leave) therefore looks joined for at most this long.
pub const STICKY_DURATION: Duration = Duration::from_secs(300);
/// Resend before the event expires, so we never flicker out.
pub const REFRESH_EVERY: Duration = Duration::from_secs(150);
/// How long an invite rings (MSC4075 recommends 90 s).
pub const INVITE_LIFETIME_MS: u64 = 90_000;
/// The invite event stays sticky at least as long as it rings.
pub const INVITE_STICKY_DURATION: Duration = Duration::from_secs(120);

/// A fresh `member.id`: 16 random bytes, unpadded base64url (MSC4143 wants a
/// CSPRNG value, new for every join).
pub fn new_member_id() -> String {
    let mut b = [0u8; 16];
    rand::thread_rng().fill_bytes(&mut b);
    Base64UrlUnpadded::encode_string(&b)
}

pub struct JoinParams {
    pub member_id: String,
    pub user_id: String,
    pub device_id: String,
    /// The SFU WebSocket URL we publish on.
    pub sfu_url: String,
    /// The lk-jwt-service base URL (older clients use this one).
    pub service_url: String,
    pub audio_only: bool,
}

/// The `m.rtc.member` join content. Carries the fields matrix-js-sdk insists
/// on (`member.user_id`/`device_id`, both published transport spellings, both
/// sticky key names, both intent spellings).
pub fn build_member_join(p: &JoinParams) -> Value {
    let (intent, legacy_intent) = if p.audio_only { ("m.audio", "audio") } else { ("m.video", "video") };
    json!({
        "slot_id": CALL_SLOT_ID,
        "member": {
            "id": p.member_id,
            "membership": "join",
            "user_id": p.user_id,
            "device_id": p.device_id,
        },
        "application": {
            "type": "m.call",
            "intent": intent,
            "m.call.intent": legacy_intent,
            "capabilities": ["m.render_audio", "m.render_video"],
        },
        "transports": {
            "published": [{
                "type": "livekit",
                "url": p.sfu_url,
                "livekit_service_url": p.service_url,
            }],
            "can_subscribe": ["livekit"],
        },
        "sticky_key": p.member_id,
        "msc4354_sticky_key": p.member_id,
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LeaveCode {
    Leave,
    DelayedLeave,
    SlotClosed,
    TransportError,
    MediaError,
    CodecMismatch,
}

impl LeaveCode {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Leave => "leave",
            Self::DelayedLeave => "delayed_leave",
            Self::SlotClosed => "slot_closed",
            Self::TransportError => "transport_error",
            Self::MediaError => "media_error",
            Self::CodecMismatch => "codec_mismatch",
        }
    }
}

pub fn build_member_leave(slot_id: &str, member_id: &str, code: LeaveCode, reason: Option<&str>) -> Value {
    let mut leave_reason = json!({ "code": code.as_str() });
    if let Some(r) = reason {
        leave_reason["reason"] = json!(r);
    }
    json!({
        "slot_id": slot_id,
        "member": { "id": member_id, "membership": "leave" },
        "leave_reason": leave_reason,
        "sticky_key": member_id,
        "msc4354_sticky_key": member_id,
    })
}

/// Write MatrixRTC member events only when the server supports sticky events,
/// the room has an open slot and our power level lets us send the event.
pub fn should_write_member_events(sticky_supported: bool, slot_open: bool, can_send_member: bool) -> bool {
    sticky_supported && slot_open && can_send_member
}

/// The invite that rings a DM (`@room` mention; group rooms are not rung).
pub fn build_invite(now_ms: u64) -> Value {
    json!({
        "slot_id": CALL_SLOT_ID,
        "sender_ts": now_ms,
        "lifetime": INVITE_LIFETIME_MS,
        "m.mentions": { "room": true },
        "sticky_key": CALL_SLOT_ID,
        "msc4354_sticky_key": CALL_SLOT_ID,
    })
}

/// A withdrawal: the same sticky key with nothing else (MSC4075).
pub fn build_invite_withdrawal() -> Value {
    json!({ "sticky_key": CALL_SLOT_ID, "msc4354_sticky_key": CALL_SLOT_ID })
}

/// Whether the power levels let us send the `m.rtc.member` event.
pub async fn can_send_member_event(room: &Room) -> bool {
    use matrix_sdk::ruma::events::MessageLikeEventType;
    let Some(user) = room.client().user_id().map(|u| u.to_owned()) else {
        return false;
    };
    match room.power_levels().await {
        Ok(pl) => pl.user_can_send_message(&user, MessageLikeEventType::from(MEMBER_TYPE)),
        Err(_) => false,
    }
}

pub async fn send_invite(room: &Room, now_ms: u64) -> anyhow::Result<matrix_sdk::ruma::OwnedEventId> {
    send_sticky(room, INVITE_TYPE, build_invite(now_ms), INVITE_STICKY_DURATION).await
}

pub async fn withdraw_invite(room: &Room) -> anyhow::Result<()> {
    send_sticky(room, INVITE_TYPE, build_invite_withdrawal(), INVITE_STICKY_DURATION).await?;
    Ok(())
}

/// Where membership events go. The room in production; a recorder in tests.
#[async_trait::async_trait]
pub trait StickySink: Send + Sync {
    async fn send(&self, event_type: &str, content: Value, duration: Duration) -> anyhow::Result<()>;
}

struct RoomSink(Room);

#[async_trait::async_trait]
impl StickySink for RoomSink {
    async fn send(&self, event_type: &str, content: Value, duration: Duration) -> anyhow::Result<()> {
        send_sticky(&self.0, event_type, content, duration).await?;
        Ok(())
    }
}

/// Our `m.rtc.member` membership in the room's call slot.
pub struct MrtcMembership {
    sink: std::sync::Arc<dyn StickySink>,
    params: JoinParams,
    encrypted_at_join: bool,
    /// Held across every send, and `left` is only read or set under it, so a
    /// refresh that raced a leave cannot send its join after the leave.
    io: tokio::sync::Mutex<()>,
    left: std::sync::atomic::AtomicBool,
}

impl MrtcMembership {
    pub fn new(room: Room, params: JoinParams) -> Self {
        let encrypted_at_join = room.encryption_state().is_encrypted();
        Self::with_sink(std::sync::Arc::new(RoomSink(room)), params, encrypted_at_join)
    }

    pub fn with_sink(sink: std::sync::Arc<dyn StickySink>, params: JoinParams, encrypted_at_join: bool) -> Self {
        Self {
            sink,
            params,
            encrypted_at_join,
            io: tokio::sync::Mutex::new(()),
            left: std::sync::atomic::AtomicBool::new(false),
        }
    }

    pub fn member_id(&self) -> &str {
        &self.params.member_id
    }

    pub fn encrypted_at_join(&self) -> bool {
        self.encrypted_at_join
    }

    /// Send (or resend) the join event. Does nothing once we have left.
    pub async fn join(&self) -> anyhow::Result<()> {
        let _io = self.io.lock().await;
        if self.has_left() {
            return Ok(());
        }
        self.sink.send(MEMBER_TYPE, build_member_join(&self.params), STICKY_DURATION).await
    }

    /// Leave the slot once; later calls are no-ops.
    pub async fn leave(&self, code: LeaveCode, reason: Option<&str>) {
        let _io = self.io.lock().await;
        if self.left.swap(true, std::sync::atomic::Ordering::SeqCst) {
            return;
        }
        let body = build_member_leave(CALL_SLOT_ID, &self.params.member_id, code, reason);
        match self.sink.send(MEMBER_TYPE, body, STICKY_DURATION).await {
            Ok(()) => info!("rtc: left the MatrixRTC slot ({})", code.as_str()),
            Err(e) => warn!("rtc: MatrixRTC leave failed: {e}"),
        }
    }

    pub fn has_left(&self) -> bool {
        self.left.load(std::sync::atomic::Ordering::SeqCst)
    }
}

/// The open call slot for a room: encrypted rooms must declare `m.per_member`
/// (MSC4143), unencrypted rooms must not have an `encryption` object.
pub fn slot_for(room_encrypted: bool) -> SlotContent {
    SlotContent {
        status: SlotStatus::Open,
        application: Some(AppRef { kind: "m.call".to_owned() }),
        encryption: room_encrypted.then(|| EncryptionRef { kind: "m.per_member".to_owned() }),
    }
}

/// The open call slot as a `/createRoom` `initial_state` entry (MSC4196:
/// rooms created with a private preset start with calls enabled).
pub fn slot_initial_state_event(
    room_encrypted: bool,
) -> matrix_sdk::ruma::serde::Raw<matrix_sdk::ruma::events::AnyInitialStateEvent> {
    let json = serde_json::json!({
        "type": SLOT_TYPE,
        "state_key": CALL_SLOT_ID,
        "content": slot_for(room_encrypted),
    });
    // A JSON object built above always serializes.
    matrix_sdk::ruma::serde::Raw::from_json(
        serde_json::value::to_raw_value(&json).expect("slot initial state serializes"),
    )
}

/// Whether a slot is usable for a room: open, and declaring `m.per_member`
/// exactly when the room is encrypted (MSC4143 "relation to room encryption").
pub fn slot_matches_room(slot: &SlotContent, room_encrypted: bool) -> bool {
    if !slot.is_open() {
        return false;
    }
    match (&slot.encryption, room_encrypted) {
        (Some(e), true) => e.kind == "m.per_member",
        (None, false) => true,
        _ => false,
    }
}

/// Why we must leave right now, if at all: the slot was closed, or the room
/// became encrypted while we were joined without MatrixRTC encryption.
pub fn leave_reason_now(
    slot: Option<&SlotContent>,
    encrypted_at_join: bool,
    encrypted_now: bool,
) -> Option<(LeaveCode, &'static str)> {
    if slot.is_some_and(|s| s.status == SlotStatus::Closed) {
        return Some((LeaveCode::SlotClosed, "the slot was closed"));
    }
    if encrypted_now && !encrypted_at_join {
        return Some((LeaveCode::SlotClosed, "the room became encrypted"));
    }
    None
}

/// Open the slot only when the room has none and we may send the state event.
/// An existing slot, open or deliberately closed, is left alone.
pub fn should_open_slot(existing: Option<&SlotContent>, can_send_slot: bool) -> bool {
    existing.is_none() && can_send_slot
}

/// Whether the power levels let us send the slot state event.
pub async fn can_send_slot(room: &Room) -> bool {
    let Some(user) = room.client().user_id().map(|u| u.to_owned()) else {
        return false;
    };
    match room.power_levels().await {
        Ok(pl) => pl.user_can_send_state(&user, StateEventType::from(SLOT_TYPE)),
        Err(_) => false,
    }
}

/// Send the open `m.rtc.slot` (state key `m.call#room`).
pub async fn open_slot(room: &Room) -> anyhow::Result<()> {
    let encrypted = room.encryption_state().is_encrypted();
    let content = serde_json::to_value(slot_for(encrypted))?;
    let raw = serde_json::value::to_raw_value(&content)?;
    room.send_state_event_raw(SLOT_TYPE, CALL_SLOT_ID, raw).await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::client::rtc::model::{AppRef, SlotContent, SlotStatus};

    #[test]
    fn join_body_satisfies_matrix_js_sdk_validation() {
        let v = build_member_join(&JoinParams {
            member_id: "mid1".into(),
            user_id: "@a:x.org".into(),
            device_id: "DEV".into(),
            sfu_url: "wss://sfu.x.org".into(),
            service_url: "https://jwt.x.org".into(),
            audio_only: true,
        });
        assert!(v["slot_id"].as_str().unwrap().starts_with("m.call#"));
        assert_eq!(v["member"]["id"], "mid1");
        assert_eq!(v["member"]["user_id"], "@a:x.org");
        assert_eq!(v["member"]["device_id"], "DEV");
        assert_eq!(v["member"]["membership"], "join");
        assert_eq!(v["application"]["type"], "m.call");
        assert_eq!(v["transports"]["can_subscribe"], serde_json::json!(["livekit"]));
        let t = &v["transports"]["published"][0];
        assert_eq!(t["type"], "livekit");
        assert_eq!(t["url"], "wss://sfu.x.org");
        assert_eq!(t["livekit_service_url"], "https://jwt.x.org");
        assert_eq!(v["sticky_key"], "mid1");
        assert_eq!(v["msc4354_sticky_key"], "mid1");
    }

    #[test]
    fn join_body_carries_the_intent_in_both_spellings() {
        let mk = |audio_only| {
            build_member_join(&JoinParams {
                member_id: "m".into(), user_id: "@a:x".into(), device_id: "D".into(),
                sfu_url: "wss://s".into(), service_url: "https://j".into(), audio_only,
            })
        };
        assert_eq!(mk(true)["application"]["intent"], "m.audio");
        assert_eq!(mk(true)["application"]["m.call.intent"], "audio");
        assert_eq!(mk(false)["application"]["intent"], "m.video");
        assert_eq!(mk(false)["application"]["m.call.intent"], "video");
        let caps = mk(false)["application"]["capabilities"].clone();
        assert_eq!(caps, serde_json::json!(["m.render_audio", "m.render_video"]));
    }

    #[test]
    fn the_body_round_trips_through_our_own_parser() {
        let v = build_member_join(&JoinParams {
            member_id: "m".into(), user_id: "@a:x".into(), device_id: "D".into(),
            sfu_url: "wss://s".into(), service_url: "https://j".into(), audio_only: false,
        });
        let c: crate::client::rtc::model::MemberContent = serde_json::from_value(v).unwrap();
        assert!(c.is_join());
        assert_eq!(c.member.device_id.as_deref(), Some("D"));
    }

    #[test]
    fn member_ids_are_unique_and_random_looking() {
        let ids: std::collections::HashSet<String> = (0..200).map(|_| new_member_id()).collect();
        assert_eq!(ids.len(), 200);
        assert!(ids.iter().all(|i| i.len() >= 21 && !i.contains('@')));
    }

    #[test]
    fn leave_body_matches_the_msc() {
        let v = build_member_leave("m.call#room", "mid1", LeaveCode::SlotClosed, Some("closed"));
        assert_eq!(v["member"], serde_json::json!({"id": "mid1", "membership": "leave"}));
        assert_eq!(v["leave_reason"], serde_json::json!({"code": "slot_closed", "reason": "closed"}));
        assert_eq!(v["sticky_key"], "mid1");
        let plain = build_member_leave("m.call#room", "mid1", LeaveCode::Leave, None);
        assert!(plain["leave_reason"].get("reason").is_none());
    }

    #[test]
    fn leave_codes_use_the_spec_names() {
        for (c, s) in [
            (LeaveCode::Leave, "leave"), (LeaveCode::DelayedLeave, "delayed_leave"),
            (LeaveCode::SlotClosed, "slot_closed"), (LeaveCode::TransportError, "transport_error"),
            (LeaveCode::MediaError, "media_error"), (LeaveCode::CodecMismatch, "codec_mismatch"),
        ] {
            assert_eq!(c.as_str(), s);
        }
    }

    #[test]
    fn the_slot_must_agree_with_the_rooms_encryption() {
        assert!(slot_matches_room(&slot_for(true), true));
        assert!(slot_matches_room(&slot_for(false), false));
        assert!(!slot_matches_room(&slot_for(false), true), "encrypted room, slot without encryption");
        assert!(!slot_matches_room(&slot_for(true), false), "plain room, slot with encryption");
        let closed = SlotContent { status: SlotStatus::Closed, application: None, encryption: None };
        assert!(!slot_matches_room(&closed, false));
    }

    #[test]
    fn we_leave_when_the_slot_closes_or_the_room_becomes_encrypted() {
        let closed = SlotContent { status: SlotStatus::Closed, application: None, encryption: None };
        assert_eq!(leave_reason_now(Some(&closed), false, false), Some((LeaveCode::SlotClosed, "the slot was closed")));
        assert_eq!(
            leave_reason_now(Some(&slot_for(false)), false, true),
            Some((LeaveCode::SlotClosed, "the room became encrypted"))
        );
        // Nothing to do: slot open, encryption unchanged, or no slot state seen yet.
        assert_eq!(leave_reason_now(Some(&slot_for(true)), true, true), None);
        assert_eq!(leave_reason_now(None, false, false), None);
        // Encrypted from the start and the slot (still) says so.
        assert_eq!(leave_reason_now(Some(&slot_for(true)), true, true), None);
    }

    // ── MrtcMembership with a recording sink ────────────────────────────────

    #[derive(Default)]
    struct FakeSink {
        sent: std::sync::Mutex<Vec<(String, Value)>>,
    }

    #[async_trait::async_trait]
    impl StickySink for FakeSink {
        async fn send(&self, event_type: &str, content: Value, _d: Duration) -> anyhow::Result<()> {
            // Give a concurrent task the chance to interleave.
            tokio::task::yield_now().await;
            self.sent.lock().unwrap().push((event_type.to_owned(), content));
            tokio::task::yield_now().await;
            Ok(())
        }
    }

    fn membership(sink: &std::sync::Arc<FakeSink>) -> MrtcMembership {
        MrtcMembership::with_sink(
            sink.clone(),
            JoinParams {
                member_id: "mid1".into(), user_id: "@a:x".into(), device_id: "D".into(),
                sfu_url: "wss://s".into(), service_url: "https://j".into(), audio_only: false,
            },
            true,
        )
    }

    fn memberships_sent(sink: &FakeSink) -> Vec<&'static str> {
        sink.sent.lock().unwrap().iter().map(|(_, c)| {
            if c["member"]["membership"] == "leave" { "leave" } else { "join" }
        }).collect()
    }

    #[tokio::test]
    async fn join_sends_the_member_event() {
        let sink = std::sync::Arc::new(FakeSink::default());
        membership(&sink).join().await.unwrap();
        let sent = sink.sent.lock().unwrap();
        assert_eq!(sent.len(), 1);
        assert_eq!(sent[0].0, MEMBER_TYPE);
        assert_eq!(sent[0].1["member"]["id"], "mid1");
    }

    #[tokio::test]
    async fn a_join_after_a_leave_sends_nothing() {
        let sink = std::sync::Arc::new(FakeSink::default());
        let m = membership(&sink);
        m.join().await.unwrap();
        m.leave(LeaveCode::Leave, None).await;
        m.join().await.unwrap();
        assert_eq!(memberships_sent(&sink), vec!["join", "leave"]);
    }

    #[tokio::test]
    async fn leaving_twice_sends_one_leave() {
        let sink = std::sync::Arc::new(FakeSink::default());
        let m = membership(&sink);
        m.leave(LeaveCode::SlotClosed, Some("closed")).await;
        m.leave(LeaveCode::Leave, None).await;
        assert_eq!(memberships_sent(&sink), vec!["leave"]);
    }

    #[tokio::test]
    async fn a_refresh_racing_a_leave_cannot_follow_it() {
        for _ in 0..50 {
            let sink = std::sync::Arc::new(FakeSink::default());
            let m = membership(&sink);
            let (_, _) = tokio::join!(
                async { let _ = m.join().await; },
                m.leave(LeaveCode::SlotClosed, None),
            );
            let sent = memberships_sent(&sink);
            if let Some(pos) = sent.iter().position(|s| *s == "leave") {
                assert!(sent[pos..].iter().all(|s| *s == "leave"), "a join followed the leave: {sent:?}");
            }
        }
    }

    #[test]
    fn refresh_comes_well_before_expiry() {
        assert!(REFRESH_EVERY * 2 <= STICKY_DURATION);
        assert!(STICKY_DURATION >= std::time::Duration::from_secs(300), "MSC4354 floor");
    }

    #[test]
    fn member_events_are_written_only_when_sticky_slot_and_permission_line_up() {
        assert!(should_write_member_events(true, true, true));
        assert!(!should_write_member_events(false, true, true), "server without sticky");
        assert!(!should_write_member_events(true, false, true), "no open slot");
        assert!(!should_write_member_events(true, true, false), "may not send the event");
    }

    #[test]
    fn invite_body_follows_the_rewrite() {
        let v = build_invite(1_784_493_900_000);
        assert_eq!(v["slot_id"], "m.call#room");
        assert_eq!(v["lifetime"], 90_000);
        assert_eq!(v["sender_ts"], 1_784_493_900_000u64);
        assert_eq!(v["m.mentions"], serde_json::json!({"room": true}));
        assert_eq!(v["sticky_key"], "m.call#room");
        assert_eq!(v["msc4354_sticky_key"], "m.call#room");
        assert!(INVITE_STICKY_DURATION.as_millis() as u64 >= 90_000);
        let w = build_invite_withdrawal();
        assert_eq!(w, serde_json::json!({"sticky_key": "m.call#room", "msc4354_sticky_key": "m.call#room"}));
    }

    #[test]
    fn slot_for_encrypted_room_has_per_member() {
        let s = slot_for(true);
        assert!(s.is_open());
        assert_eq!(s.application.unwrap().kind, "m.call");
        assert_eq!(s.encryption.unwrap().kind, "m.per_member");
    }

    #[test]
    fn slot_for_plain_room_omits_encryption() {
        let s = slot_for(false);
        assert!(s.is_open());
        assert!(s.encryption.is_none());
        let v = serde_json::to_value(&s).unwrap();
        assert!(v.get("encryption").is_none(), "{v}");
    }

    #[test]
    fn initial_state_event_has_the_slot_shape() {
        let raw = slot_initial_state_event(true);
        let v: serde_json::Value = serde_json::from_str(raw.json().get()).unwrap();
        assert_eq!(v["type"], "org.matrix.msc4143.rtc.slot");
        assert_eq!(v["state_key"], "m.call#room");
        assert_eq!(v["content"]["encryption"]["type"], "m.per_member");
        let plain: serde_json::Value = serde_json::from_str(slot_initial_state_event(false).json().get()).unwrap();
        assert!(plain["content"].get("encryption").is_none());
    }

    #[test]
    fn open_slot_only_when_missing_and_permitted() {
        assert!(should_open_slot(None, true));
        assert!(!should_open_slot(None, false), "no permission to send the state event");
        assert!(!should_open_slot(Some(&slot_for(true)), true), "already open");
    }

    #[test]
    fn closed_slot_is_not_reopened_automatically() {
        let closed = SlotContent {
            status: SlotStatus::Closed,
            application: Some(AppRef { kind: "m.call".into() }),
            encryption: None,
        };
        assert!(!should_open_slot(Some(&closed), true), "closing is an admin decision");
    }
}
