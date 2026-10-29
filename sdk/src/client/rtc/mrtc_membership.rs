//! Our side of MatrixRTC membership: opening the room's `m.call` slot, and the
//! sticky `m.rtc.member` (and DM `m.rtc.invite`) events a call sends next to
//! the legacy `org.matrix.msc3401.call.member` one.

use matrix_sdk::{
    ruma::{
        events::{room::power_levels::RoomPowerLevels, StateEventType},
        UserId,
    },
    Room,
};

use super::model::{
    AppRef, EncryptionRef, SlotContent, SlotStatus, CALL_SLOT_ID, INVITE_TYPE, MEMBER_TYPE, SLOT_TYPE,
};
use super::slots::SlotState;
use super::sticky::send_sticky;
use base64ct::{Base64UrlUnpadded, Encoding};
use rand::RngCore;
use serde_json::{json, Value};
use std::time::{Duration, Instant};
use tracing::{info, warn};

/// How long one `m.rtc.member` event keeps us joined: MSC4354's 5 minute floor.
/// A client that disappears without leaving (encrypted rooms cannot get a
/// delayed leave) therefore looks joined for at most this long.
pub const STICKY_DURATION: Duration = Duration::from_secs(300);
/// Resend before the event expires, so we never flicker out.
pub const REFRESH_EVERY: Duration = Duration::from_secs(150);
/// After a failed refresh, retry this soon, doubling up to [`REFRESH_RETRY_MAX`]:
/// the next regular refresh would otherwise come at the instant the event expires.
pub const REFRESH_RETRY_MIN: Duration = Duration::from_secs(15);
pub const REFRESH_RETRY_MAX: Duration = Duration::from_secs(60);
/// Pause before the one retry of a failed leave.
const LEAVE_RETRY_DELAY: Duration = Duration::from_millis(500);
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

/// How long to wait before the next refresh, given how many in a row failed:
/// the regular cadence after a success, else 15 s doubling to a 60 s ceiling.
pub fn refresh_delay(consecutive_failures: u32) -> Duration {
    if consecutive_failures == 0 {
        return REFRESH_EVERY;
    }
    let doublings = (consecutive_failures - 1).min(16);
    (REFRESH_RETRY_MIN * 2u32.pow(doublings)).min(REFRESH_RETRY_MAX)
}

/// Whether media is flowing in the clear in a room that has since become
/// encrypted: `use_e2ee` is fixed when the call starts, so the call must end.
pub fn encryption_flipped_mid_call(use_e2ee: bool, encrypted_now: bool) -> bool {
    encrypted_now && !use_e2ee
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
    /// Held across every send, so a refresh that raced a leave cannot send its
    /// join after the leave. The value is whether the leave has been delivered.
    io: tokio::sync::Mutex<bool>,
    /// We are leaving or have left: no more joins or refreshes. Says nothing
    /// about whether the leave itself got out (that is the value of `io`).
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
            io: tokio::sync::Mutex::new(false),
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

    /// Leave the slot. The leave is delivered once: it is retried once when the
    /// send fails, later calls are no-ops after a delivery and try again after
    /// a failure or a cancelled call. Returns whether it is delivered.
    pub async fn leave(&self, code: LeaveCode, reason: Option<&str>) -> bool {
        self.leave_after(code, reason, LEAVE_RETRY_DELAY).await
    }

    async fn leave_after(&self, code: LeaveCode, reason: Option<&str>, retry_delay: Duration) -> bool {
        // Before waiting for the lock, so a queued refresh sends nothing.
        self.left.store(true, std::sync::atomic::Ordering::SeqCst);
        let mut delivered = self.io.lock().await;
        if *delivered {
            return true;
        }
        let body = build_member_leave(CALL_SLOT_ID, &self.params.member_id, code, reason);
        let mut sent = self.sink.send(MEMBER_TYPE, body.clone(), STICKY_DURATION).await;
        if let Err(e) = &sent {
            warn!("rtc: MatrixRTC leave failed, retrying once: {e}");
            tokio::time::sleep(retry_delay).await;
            sent = self.sink.send(MEMBER_TYPE, body, STICKY_DURATION).await;
        }
        match sent {
            Ok(()) => {
                *delivered = true;
                info!("rtc: left the MatrixRTC slot ({})", code.as_str());
            }
            Err(e) => warn!("rtc: MatrixRTC leave failed: {e}"),
        }
        *delivered
    }

    /// Withdraw earlier joins of ours (`(slot_id, member_id)`, see
    /// [`super::slots::stale_own_members`]) that a new session replaces.
    /// Best effort: a failure only means the ghost lives out its lifetime.
    pub async fn withdraw_stale(&self, stale: &[(String, String)]) {
        let _io = self.io.lock().await;
        for (slot_id, member_id) in stale {
            let body = build_member_leave(slot_id, member_id, LeaveCode::Leave, Some("replaced by a new session"));
            match self.sink.send(MEMBER_TYPE, body, STICKY_DURATION).await {
                Ok(()) => info!("rtc: withdrew a stale MatrixRTC join of ours ({member_id})"),
                Err(e) => warn!("rtc: could not withdraw a stale MatrixRTC join ({member_id}): {e}"),
            }
        }
    }

    /// Resend the join before it expires until we leave. A failed resend is
    /// retried soon (see [`refresh_delay`]) instead of at the expiry instant.
    pub async fn run_refresh(&self) {
        let mut failures = 0u32;
        loop {
            tokio::time::sleep(refresh_delay(failures)).await;
            if self.has_left() {
                break;
            }
            match self.join().await {
                Ok(()) => failures = 0,
                Err(e) => {
                    failures = failures.saturating_add(1);
                    warn!("rtc: MatrixRTC membership refresh failed: {e}");
                }
            }
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

/// Follows the room's slot while we are joined, to leave when it goes away. A
/// slot that is closed means calls are off. A slot that reads as missing can
/// also be a store error, a cache re-restore or a sync gap, and reconcile
/// passes can come milliseconds apart (they also run on member changes), so a
/// missing slot ends the call only when it has been missing on
/// [`SlotWatch::MISSES_TO_LEAVE`] reads that span [`SlotWatch::MISS_PERIOD`].
/// A slot that is there but cannot be read (a shape we do not know, or a read
/// that failed) is not evidence either way: it is logged and ignored.
///
/// A slot we wrote ourselves ([`Self::wrote_slot`]) is not judged for
/// [`SlotWatch::WRITE_GRACE`]: its echo can reach the store later than that
/// of the passes which ask, and would read as missing. A leave it asked for
/// stays asked for until [`Self::delivered`], so one that failed is sent again.
#[derive(Default)]
pub struct SlotWatch {
    /// When the slot first read as missing, in the current run of misses.
    first_miss: Option<Instant>,
    misses: u32,
    /// The slot is not judged before this.
    grace_until: Option<Instant>,
    /// The leave asked for and not yet delivered.
    due: Option<(LeaveCode, &'static str)>,
}

impl SlotWatch {
    pub const MISSES_TO_LEAVE: u32 = 2;
    pub const MISS_PERIOD: Duration = Duration::from_secs(15);
    /// How long after we wrote or replaced the slot it is left unjudged.
    pub const WRITE_GRACE: Duration = Duration::from_secs(60);

    /// We wrote (or replaced) the slot at `now`.
    pub fn wrote_slot(&mut self, now: Instant) {
        self.grace_until = Some(now + Self::WRITE_GRACE);
        self.first_miss = None;
        self.misses = 0;
    }

    /// Whether a leave was asked for and is not delivered yet.
    pub fn leave_due(&self) -> bool {
        self.due.is_some()
    }

    /// The leave that was asked for went out.
    pub fn delivered(&mut self) {
        self.due = None;
    }

    /// Feed the slot as read this pass, at `now`; returns why to leave now, if
    /// so. Once it has said to leave it says so again on every pass, until
    /// [`Self::delivered`].
    pub fn observe(
        &mut self,
        state: &SlotState,
        encrypted_at_join: bool,
        encrypted_now: bool,
        now: Instant,
    ) -> Option<(LeaveCode, &'static str)> {
        if self.due.is_some() {
            return self.due;
        }
        let judged = self.grace_until.is_none_or(|g| now >= g);
        let slot = match state {
            SlotState::Present(slot) => {
                self.first_miss = None;
                self.misses = 0;
                judged.then_some(slot)
            }
            SlotState::Unreadable => {
                warn!("rtc: the room's call slot cannot be read; staying in the call");
                None
            }
            SlotState::Absent if !judged => {
                self.first_miss = None;
                self.misses = 0;
                None
            }
            SlotState::Absent => {
                let since = *self.first_miss.get_or_insert(now);
                self.misses += 1;
                if self.misses >= Self::MISSES_TO_LEAVE && now.saturating_duration_since(since) >= Self::MISS_PERIOD {
                    self.due = Some((LeaveCode::SlotClosed, "the slot was removed"));
                    return self.due;
                }
                None
            }
        };
        self.due = leave_reason_now(slot, encrypted_at_join, encrypted_now);
        self.due
    }
}

/// What starting a call does about the room's `m.call` slot.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlotPlan {
    /// The slot is open and fits the room: use it.
    Use,
    /// Write a fresh open slot that fits the room.
    Write,
    /// Leave the slot alone: the call goes on without MatrixRTC membership.
    Skip,
}

/// Decide what to do about the slot, given what the room has.
///
/// - No slot: write one if we may.
/// - An open slot that disagrees with the room's encryption (it was opened
///   before the room became encrypted): no member can be joined in it, so
///   replace it if we may, but only when nobody else is in the call: others
///   mid-call run on the legacy events, and swapping the slot under them would
///   have us write a sticky member next to a call they cannot see it in.
/// - A closed slot, or one that is there but cleared or unreadable: leave it.
///   Closing or clearing the slot is how a room says calls are off (or the
///   event is a shape we do not know and must not overwrite), and starting a
///   call is not consent to undo that.
pub fn plan_slot(state: &SlotState, room_encrypted: bool, can_send_slot: bool, others_present: bool) -> SlotPlan {
    // `others_present` is [`others_may_be_present`]: true when the member read
    // was cut short too, so an existing slot is never replaced on a guess.
    match state {
        SlotState::Absent if can_send_slot => SlotPlan::Write,
        SlotState::Present(s) if s.is_open() && slot_matches_room(s, room_encrypted) => SlotPlan::Use,
        SlotState::Present(s) if s.is_open() && can_send_slot && !others_present => SlotPlan::Write,
        SlotState::Absent | SlotState::Unreadable | SlotState::Present(_) => SlotPlan::Skip,
    }
}

/// Whether somebody else may be in the call: members are known, or the member
/// read did not finish (a big room's list timed out), in which case an empty
/// list proves nothing.
pub fn others_may_be_present(known_members: usize, member_read_complete: bool) -> bool {
    known_members > 0 || !member_read_complete
}

/// The legacy state events a call runs on (MSC3401).
const LEGACY_CALL_TYPE: &str = "org.matrix.msc3401.call";
const LEGACY_MEMBER_TYPE: &str = "org.matrix.msc3401.call.member";

/// Whether the power levels let `user` join a call: the legacy `call.member`
/// state event the call runs on. The sticky `m.rtc.member` is not part of this
/// gate: where the room forbids it the session degrades to a legacy-only call
/// (see `should_write_member_events`).
pub fn may_join_call(pl: &RoomPowerLevels, user: &UserId) -> bool {
    pl.user_can_send_state(user, StateEventType::from(LEGACY_MEMBER_TYPE))
}

/// Whether the power levels let `user` start a call: join permission plus the
/// legacy `call` state event. `m.rtc.slot` is not part of this gate either: a
/// session that may not write it runs legacy-only (see `plan_slot`).
pub fn may_start_call(pl: &RoomPowerLevels, user: &UserId) -> bool {
    may_join_call(pl, user) && pl.user_can_send_state(user, StateEventType::from(LEGACY_CALL_TYPE))
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

/// Send the open `m.rtc.slot` under `state_key`: `m.call#room` for a new slot,
/// or the key a mismatched slot being replaced lives under (the reference
/// implementation writes `m.call#ROOM`), so that no second slot appears.
pub async fn open_slot(room: &Room, state_key: &str) -> anyhow::Result<()> {
    let encrypted = room.encryption_state().is_encrypted();
    let content = serde_json::to_value(slot_for(encrypted))?;
    let raw = serde_json::value::to_raw_value(&content)?;
    room.send_state_event_raw(SLOT_TYPE, state_key, raw).await?;
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
    fn an_unfinished_member_read_never_lets_an_existing_slot_be_replaced() {
        let old = SlotState::Present(slot_for(false));
        // Read complete, nobody there: replace the stale slot.
        assert_eq!(plan_slot(&old, true, true, others_may_be_present(0, true)), SlotPlan::Write);
        // Read timed out with nobody known: others may be mid-call, so leave it.
        assert_eq!(plan_slot(&old, true, true, others_may_be_present(0, false)), SlotPlan::Skip);
        // Writing when there is no slot at all stays fine either way.
        assert_eq!(plan_slot(&SlotState::Absent, true, true, others_may_be_present(0, false)), SlotPlan::Write);
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

    fn secs(t0: Instant, n: u64) -> Instant {
        t0 + Duration::from_secs(n)
    }

    #[test]
    fn a_slot_that_stays_gone_for_the_whole_period_ends_the_call() {
        let t0 = Instant::now();
        let open = SlotState::Present(slot_for(true));
        let mut w = SlotWatch::default();
        assert_eq!(w.observe(&open, true, true, t0), None);
        assert_eq!(w.observe(&SlotState::Absent, true, true, secs(t0, 1)), None, "the first miss");
        assert_eq!(w.observe(&SlotState::Absent, true, true, secs(t0, 10)), None, "two reads, too soon");
        assert_eq!(
            w.observe(&SlotState::Absent, true, true, secs(t0, 16)),
            Some((LeaveCode::SlotClosed, "the slot was removed")),
            "missing for 15 s over several reads"
        );
    }

    #[test]
    fn two_reads_milliseconds_apart_do_not_end_the_call() {
        let t0 = Instant::now();
        let mut w = SlotWatch::default();
        for ms in [0, 5, 20, 40] {
            let at = t0 + Duration::from_millis(ms);
            assert_eq!(w.observe(&SlotState::Absent, true, true, at), None, "{ms} ms");
        }
        // A long gap with a single read is not two observations.
        let mut w = SlotWatch::default();
        assert_eq!(w.observe(&SlotState::Absent, true, true, secs(t0, 100)), None);
    }

    #[test]
    fn a_slot_that_cannot_be_read_never_ends_the_call() {
        let t0 = Instant::now();
        let mut w = SlotWatch::default();
        for n in 0..10 {
            assert_eq!(w.observe(&SlotState::Unreadable, true, true, secs(t0, n * 30)), None);
        }
        // Neither does it start or reset the clock of real misses.
        let mut w = SlotWatch::default();
        assert_eq!(w.observe(&SlotState::Absent, true, true, t0), None);
        assert_eq!(w.observe(&SlotState::Unreadable, true, true, secs(t0, 20)), None);
        assert_eq!(
            w.observe(&SlotState::Absent, true, true, secs(t0, 30)),
            Some((LeaveCode::SlotClosed, "the slot was removed"))
        );
    }

    #[test]
    fn a_slot_that_comes_back_resets_the_count() {
        let t0 = Instant::now();
        let open = SlotState::Present(slot_for(false));
        let mut w = SlotWatch::default();
        assert_eq!(w.observe(&SlotState::Absent, false, false, t0), None);
        assert_eq!(w.observe(&open, false, false, secs(t0, 14)), None);
        assert_eq!(w.observe(&SlotState::Absent, false, false, secs(t0, 20)), None, "the earlier miss no longer counts");
        assert_eq!(w.observe(&SlotState::Absent, false, false, secs(t0, 40)), Some((LeaveCode::SlotClosed, "the slot was removed")));
    }

    #[test]
    fn a_closed_slot_and_an_encryption_flip_still_end_the_call_at_once() {
        let t0 = Instant::now();
        let closed = SlotState::Present(SlotContent { status: SlotStatus::Closed, application: None, encryption: None });
        let mut w = SlotWatch::default();
        assert_eq!(w.observe(&closed, false, false, t0), Some((LeaveCode::SlotClosed, "the slot was closed")));
        let mut w = SlotWatch::default();
        // The room became encrypted while the slot cannot be read: that reason is not delayed.
        assert_eq!(
            w.observe(&SlotState::Absent, false, true, t0),
            Some((LeaveCode::SlotClosed, "the room became encrypted"))
        );
        assert_eq!(
            w.observe(&SlotState::Unreadable, false, true, t0),
            Some((LeaveCode::SlotClosed, "the room became encrypted"))
        );
    }

    #[test]
    fn a_slot_we_just_wrote_is_not_judged_while_its_echo_may_be_on_the_way() {
        let t0 = Instant::now();
        let closed = SlotState::Present(SlotContent { status: SlotStatus::Closed, application: None, encryption: None });
        let mut w = SlotWatch::default();
        w.wrote_slot(t0);
        // Reads that still show the old (missing) state over 50 s do not count...
        for n in [1, 10, 20, 30, 50] {
            assert_eq!(w.observe(&SlotState::Absent, true, true, secs(t0, n)), None, "{n} s");
        }
        assert_eq!(w.observe(&closed, true, true, secs(t0, 55)), None);
        // ...and the grace leaves no half-counted misses behind: the run starts afterwards.
        assert_eq!(w.observe(&SlotState::Absent, true, true, secs(t0, 61)), None, "first miss after the grace");
        assert_eq!(w.observe(&SlotState::Absent, true, true, secs(t0, 70)), None);
        assert_eq!(
            w.observe(&SlotState::Absent, true, true, secs(t0, 77)),
            Some((LeaveCode::SlotClosed, "the slot was removed"))
        );
        // The room turning encrypted is not about the slot we wrote.
        let mut w = SlotWatch::default();
        w.wrote_slot(t0);
        assert_eq!(
            w.observe(&SlotState::Absent, false, true, secs(t0, 1)),
            Some((LeaveCode::SlotClosed, "the room became encrypted"))
        );
    }

    #[test]
    fn a_leave_that_failed_is_asked_for_again_until_it_is_delivered() {
        let t0 = Instant::now();
        let closed = SlotState::Present(SlotContent { status: SlotStatus::Closed, application: None, encryption: None });
        let mut w = SlotWatch::default();
        assert!(!w.leave_due());
        let ask = w.observe(&closed, true, true, t0);
        assert_eq!(ask, Some((LeaveCode::SlotClosed, "the slot was closed")));
        assert!(w.leave_due());
        // The send failed; the next passes ask again, whatever the slot says by then.
        let open = SlotState::Present(slot_for(true));
        assert_eq!(w.observe(&open, true, true, secs(t0, 30)), ask);
        assert_eq!(w.observe(&SlotState::Unreadable, true, true, secs(t0, 60)), ask);
        w.delivered();
        assert!(!w.leave_due());
        assert_eq!(w.observe(&open, true, true, secs(t0, 90)), None);
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

    /// Fails its first `fail_first` sends, then records the rest; `hang_first`
    /// makes the very first send never finish (a request stuck in flight).
    struct FlakySink {
        fail_first: usize,
        hang_first: bool,
        attempts: std::sync::atomic::AtomicUsize,
        sent: std::sync::Mutex<Vec<Value>>,
    }

    impl FlakySink {
        fn new(fail_first: usize, hang_first: bool) -> std::sync::Arc<Self> {
            std::sync::Arc::new(Self {
                fail_first,
                hang_first,
                attempts: Default::default(),
                sent: Default::default(),
            })
        }

        fn attempts(&self) -> usize {
            self.attempts.load(std::sync::atomic::Ordering::SeqCst)
        }

        fn kinds(&self) -> Vec<&'static str> {
            self.sent
                .lock()
                .unwrap()
                .iter()
                .map(|c| if c["member"]["membership"] == "leave" { "leave" } else { "join" })
                .collect()
        }
    }

    #[async_trait::async_trait]
    impl StickySink for FlakySink {
        async fn send(&self, _t: &str, content: Value, _d: Duration) -> anyhow::Result<()> {
            let n = self.attempts.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            if n == 0 && self.hang_first {
                std::future::pending::<()>().await;
            }
            if n < self.fail_first {
                anyhow::bail!("offline");
            }
            self.sent.lock().unwrap().push(content);
            Ok(())
        }
    }

    fn flaky_membership(sink: &std::sync::Arc<FlakySink>) -> MrtcMembership {
        MrtcMembership::with_sink(
            sink.clone(),
            JoinParams {
                member_id: "mid1".into(), user_id: "@a:x".into(), device_id: "D".into(),
                sfu_url: "wss://s".into(), service_url: "https://j".into(), audio_only: false,
            },
            true,
        )
    }

    #[test]
    fn refresh_delay_backs_off_then_recovers() {
        let s = |n| Duration::from_secs(n);
        assert_eq!(refresh_delay(0), REFRESH_EVERY, "regular cadence after a success");
        assert_eq!(
            (1..=6).map(refresh_delay).collect::<Vec<_>>(),
            vec![s(15), s(30), s(60), s(60), s(60), s(60)]
        );
        assert_eq!(refresh_delay(u32::MAX), REFRESH_RETRY_MAX, "no overflow");
        assert!(REFRESH_RETRY_MAX < STICKY_DURATION - REFRESH_EVERY, "a retry lands before the expiry");
    }

    #[test]
    fn an_encrypted_room_ends_only_a_call_that_started_in_the_clear() {
        assert!(encryption_flipped_mid_call(false, true));
        assert!(!encryption_flipped_mid_call(true, true), "encrypted from the start");
        assert!(!encryption_flipped_mid_call(false, false));
        assert!(!encryption_flipped_mid_call(true, false));
    }

    #[tokio::test]
    async fn a_failed_leave_is_retried_once() {
        let sink = FlakySink::new(1, false);
        let m = flaky_membership(&sink);
        assert!(m.leave_after(LeaveCode::Leave, None, Duration::ZERO).await);
        assert_eq!(sink.attempts(), 2);
        assert_eq!(sink.kinds(), vec!["leave"]);
    }

    #[tokio::test]
    async fn a_leave_that_never_got_out_is_attempted_again_later() {
        let sink = FlakySink::new(2, false);
        let m = flaky_membership(&sink);
        assert!(!m.leave_after(LeaveCode::Leave, None, Duration::ZERO).await);
        assert_eq!(sink.attempts(), 2, "one send and one retry");
        assert!(m.has_left(), "no refresh may follow a failed leave");
        m.join().await.unwrap();
        assert_eq!(sink.attempts(), 2, "a join after the leave sends nothing");
        assert!(m.leave_after(LeaveCode::Leave, None, Duration::ZERO).await);
        assert_eq!(sink.kinds(), vec!["leave"]);
        // Delivered: further leaves are no-ops.
        assert!(m.leave_after(LeaveCode::Leave, None, Duration::ZERO).await);
        assert_eq!(sink.attempts(), 3);
    }

    #[tokio::test]
    async fn a_cancelled_leave_still_happens_when_asked_again() {
        // The send hangs; the caller (the reconcile task) is aborted mid-leave.
        let sink = FlakySink::new(0, true);
        let m = flaky_membership(&sink);
        let cancelled =
            tokio::time::timeout(Duration::from_millis(20), m.leave(LeaveCode::SlotClosed, None)).await;
        assert!(cancelled.is_err());
        assert!(m.has_left());
        assert!(sink.kinds().is_empty());
        // end_call's own leave must not be mistaken for a repeat.
        assert!(m.leave(LeaveCode::Leave, None).await);
        assert_eq!(sink.kinds(), vec!["leave"]);
    }

    #[tokio::test(start_paused = true)]
    async fn refresh_retries_soon_after_a_failure_and_stops_after_leaving() {
        let sink = FlakySink::new(2, false);
        let m = std::sync::Arc::new(flaky_membership(&sink));
        let task = {
            let m = m.clone();
            tokio::spawn(async move { m.run_refresh().await })
        };
        let at = |secs| tokio::time::sleep(Duration::from_secs(secs));
        at(151).await; // first refresh at 150 s fails
        assert_eq!(sink.attempts(), 1);
        at(15).await; // retried 15 s later, fails again
        assert_eq!(sink.attempts(), 2);
        at(30).await; // then 30 s later, succeeds
        assert_eq!(sink.attempts(), 3);
        assert_eq!(sink.kinds(), vec!["join"]);
        at(150).await; // back on the regular cadence
        assert_eq!(sink.attempts(), 4);
        m.leave_after(LeaveCode::Leave, None, Duration::ZERO).await;
        at(150).await;
        task.await.unwrap();
        assert_eq!(sink.kinds(), vec!["join", "join", "leave"]);
    }

    /// Its first send waits for `release` (a request stuck in flight), then
    /// everything is recorded.
    struct GatedSink {
        gate: tokio::sync::Notify,
        attempts: std::sync::atomic::AtomicUsize,
        sent: std::sync::Mutex<Vec<Value>>,
    }

    #[async_trait::async_trait]
    impl StickySink for GatedSink {
        async fn send(&self, _t: &str, content: Value, _d: Duration) -> anyhow::Result<()> {
            if self.attempts.fetch_add(1, std::sync::atomic::Ordering::SeqCst) == 0 {
                self.gate.notified().await;
            }
            self.sent.lock().unwrap().push(content);
            Ok(())
        }
    }

    #[tokio::test(start_paused = true)]
    async fn a_leave_landing_during_an_in_flight_refresh_stops_the_refresh_for_good() {
        let sink = std::sync::Arc::new(GatedSink {
            gate: tokio::sync::Notify::new(),
            attempts: Default::default(),
            sent: Default::default(),
        });
        let m = std::sync::Arc::new(MrtcMembership::with_sink(
            sink.clone(),
            JoinParams {
                member_id: "mid1".into(), user_id: "@a:x".into(), device_id: "D".into(),
                sfu_url: "wss://s".into(), service_url: "https://j".into(), audio_only: false,
            },
            true,
        ));
        let task = {
            let m = m.clone();
            tokio::spawn(async move { m.run_refresh().await })
        };
        tokio::time::sleep(REFRESH_EVERY + Duration::from_secs(1)).await;
        assert_eq!(sink.attempts.load(std::sync::atomic::Ordering::SeqCst), 1, "the refresh is in flight");
        let leaver = {
            let m = m.clone();
            tokio::spawn(async move { m.leave_after(LeaveCode::Leave, None, Duration::ZERO).await })
        };
        for _ in 0..5 {
            tokio::task::yield_now().await;
        }
        assert!(m.has_left(), "the leave is queued behind the join");
        assert_eq!(sink.attempts.load(std::sync::atomic::Ordering::SeqCst), 1);
        sink.gate.notify_one();
        assert!(leaver.await.unwrap());
        // The loop ends instead of sending another join on its next turn.
        tokio::time::sleep(REFRESH_EVERY * 3).await;
        task.await.unwrap();
        let kinds: Vec<&str> = sink
            .sent
            .lock()
            .unwrap()
            .iter()
            .map(|c| if c["member"]["membership"] == "leave" { "leave" } else { "join" })
            .collect();
        assert_eq!(kinds, vec!["join", "leave"]);
    }

    #[tokio::test]
    async fn stale_joins_of_ours_are_withdrawn_with_their_own_ids() {
        let sink = std::sync::Arc::new(FakeSink::default());
        let m = membership(&sink);
        m.withdraw_stale(&[("m.call#room".into(), "old1".into()), ("m.call#ROOM".into(), "old2".into())]).await;
        let sent = sink.sent.lock().unwrap();
        assert_eq!(sent.len(), 2);
        assert_eq!(sent[0].1["member"], serde_json::json!({"id": "old1", "membership": "leave"}));
        assert_eq!(sent[0].1["sticky_key"], "old1");
        assert_eq!(sent[1].1["slot_id"], "m.call#ROOM");
        assert!(!m.has_left(), "withdrawing an old join is not leaving the new one");
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
    fn a_missing_slot_is_opened_when_permitted() {
        assert_eq!(plan_slot(&SlotState::Absent, true, true, false), SlotPlan::Write);
        assert_eq!(plan_slot(&SlotState::Absent, false, false, false), SlotPlan::Skip, "no permission to send the state event");
        assert_eq!(plan_slot(&SlotState::Present(slot_for(true)), true, true, false), SlotPlan::Use, "already open");
        assert_eq!(plan_slot(&SlotState::Present(slot_for(false)), false, false, false), SlotPlan::Use);
    }

    #[test]
    fn a_slot_opened_before_the_room_was_encrypted_is_replaced() {
        let old = SlotState::Present(slot_for(false));
        assert_eq!(plan_slot(&old, true, true, false), SlotPlan::Write);
        assert_eq!(plan_slot(&old, true, false, false), SlotPlan::Skip, "cannot fix it without permission");
        // The other way round (an encrypted slot in a plain room) is just as wrong.
        assert_eq!(plan_slot(&SlotState::Present(slot_for(true)), false, true, false), SlotPlan::Write);
    }

    #[test]
    fn others_in_the_call_do_not_stop_a_missing_slot_being_written_but_do_stop_a_replacement() {
        // Writing an absent slot is fine with others around (their calls run
        // on the legacy events; the slot just was never opened).
        assert_eq!(plan_slot(&SlotState::Absent, true, true, true), SlotPlan::Write);
        assert_eq!(plan_slot(&SlotState::Absent, true, false, true), SlotPlan::Skip);
        // Replacing an open slot that does not fit the room is left to a
        // joiner who is alone.
        let old = SlotState::Present(slot_for(false));
        assert_eq!(plan_slot(&old, true, true, false), SlotPlan::Write);
        assert_eq!(plan_slot(&old, true, true, true), SlotPlan::Skip, "others are mid-call");
        // A slot that fits is used whoever is present.
        assert_eq!(plan_slot(&SlotState::Present(slot_for(true)), true, true, true), SlotPlan::Use);
    }

    #[test]
    fn a_cleared_or_unreadable_slot_is_left_alone() {
        assert_eq!(plan_slot(&SlotState::Unreadable, true, true, false), SlotPlan::Skip);
        assert_eq!(plan_slot(&SlotState::Unreadable, false, true, false), SlotPlan::Skip);
    }

    #[test]
    fn closed_slot_is_not_reopened_automatically() {
        let closed = SlotContent {
            status: SlotStatus::Closed,
            application: Some(AppRef { kind: "m.call".into() }),
            encryption: None,
        };
        assert_eq!(plan_slot(&SlotState::Present(closed), false, true, false), SlotPlan::Skip, "closing is an admin decision");
        // Open status without an application object counts as closed (MSC4143).
        let no_app = SlotContent { status: SlotStatus::Open, application: None, encryption: None };
        assert_eq!(plan_slot(&SlotState::Present(no_app), false, true, false), SlotPlan::Skip);
    }

    // ── Who may start or join a call ────────────────────────────────────────

    fn levels_for(events: serde_json::Value, my_level: i64) -> (RoomPowerLevels, matrix_sdk::ruma::OwnedUserId) {
        use matrix_sdk::ruma::{events::room::power_levels::RoomPowerLevelsSource, room_version_rules::AuthorizationRules};
        let me: matrix_sdk::ruma::OwnedUserId = "@u:x".parse().unwrap();
        let content: matrix_sdk::ruma::events::room::power_levels::RoomPowerLevelsEventContent =
            serde_json::from_value(serde_json::json!({
                "users": { "@u:x": my_level }, "users_default": 0, "events": events,
                "events_default": 0, "state_default": 50,
            }))
            .unwrap();
        let pl = RoomPowerLevels::new(
            RoomPowerLevelsSource::Original(content),
            &AuthorizationRules::V1,
            Vec::<matrix_sdk::ruma::OwnedUserId>::new(),
        );
        (pl, me)
    }

    #[test]
    fn the_legacy_events_decide_when_the_room_sets_no_matrixrtc_levels() {
        let (pl, me) = levels_for(serde_json::json!({}), 50);
        assert!(may_start_call(&pl, &me) && may_join_call(&pl, &me));
        let (pl, me) = levels_for(serde_json::json!({}), 10);
        assert!(!may_start_call(&pl, &me) && !may_join_call(&pl, &me), "state_default is 50");
        let (pl, me) = levels_for(serde_json::json!({LEGACY_MEMBER_TYPE: 0}), 0);
        assert!(may_join_call(&pl, &me));
        assert!(!may_start_call(&pl, &me), "starting also needs the legacy call event");
    }

    #[test]
    fn matrixrtc_levels_do_not_block_a_legacy_call() {
        let legacy_open = serde_json::json!({LEGACY_CALL_TYPE: 0, LEGACY_MEMBER_TYPE: 0});
        let with = |extra: serde_json::Value| {
            let mut e = legacy_open.clone();
            e.as_object_mut().unwrap().extend(extra.as_object().unwrap().clone());
            e
        };
        // The slot or the sticky member needs more than we have: the call
        // still starts and joins, legacy-only.
        let (pl, me) = levels_for(with(serde_json::json!({SLOT_TYPE: 50})), 0);
        assert!(may_join_call(&pl, &me) && may_start_call(&pl, &me));
        let (pl, me) = levels_for(with(serde_json::json!({MEMBER_TYPE: 50})), 0);
        assert!(may_join_call(&pl, &me) && may_start_call(&pl, &me));
        // The legacy levels still decide.
        let (pl, me) = levels_for(serde_json::json!({LEGACY_MEMBER_TYPE: 50, LEGACY_CALL_TYPE: 0}), 0);
        assert!(!may_join_call(&pl, &me) && !may_start_call(&pl, &me));
    }
}
