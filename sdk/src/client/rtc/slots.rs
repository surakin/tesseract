//! Who is in a MatrixRTC call: the room's `m.rtc.slot`, the sticky
//! `m.rtc.member` events seen by [`super::sticky`], joined with the legacy
//! `org.matrix.msc3401.call.member` state members.

use std::collections::HashSet;

use matrix_sdk::{
    ruma::{events::room::member::MembershipState, events::StateEventType, DeviceId, MilliSecondsSinceUnixEpoch, UInt, UserId},
    Room,
};

use super::members::{
    read_live_members, senders_to_keep, usable_service_url, CallMember, JoinCheck, LkFocus, MemberSource, Participant,
    SELECTION_MULTI_SFU,
};
use super::model::{
    transport_is_livekit, MemberContent, SlotContent, CALL_SLOT_ID, MEMBER_TYPE, SLOT_TYPE,
};
use super::sticky::{live_entries, now_ms, StickyEntry};

/// The slot ids a `m.call` slot may use: the MSC's and the reference implementation's.
const SLOT_KEYS: [&str; 2] = [CALL_SLOT_ID, "m.call#ROOM"];

/// MSC4143 "joining a slot": the four conditions plus the encryption rules.
pub fn member_is_joined(
    m: &MemberContent,
    e: &StickyEntry,
    slot: Option<&SlotContent>,
    sender_in_room: bool,
    room_encrypted: bool,
    now_ms: u64,
) -> bool {
    let Some(app) = m.application.as_ref().filter(|a| a.kind == "m.call") else {
        return false;
    };
    let Some(slot) = slot.filter(|s| s.is_open()) else {
        return false;
    };
    if slot.application.as_ref().map(|a| a.kind.as_str()) != Some(app.kind.as_str()) {
        return false;
    }
    if !m.is_join() || !sender_in_room || e.end_ms() <= now_ms {
        return false;
    }
    // Encryption: required in encrypted rooms (event and slot), forbidden otherwise.
    if room_encrypted {
        // An encryption type we do not know cannot be joined (MSC4143).
        e.was_encrypted && slot.encryption.as_ref().is_some_and(|enc| enc.kind == "m.per_member")
    } else {
        slot.encryption.is_none()
    }
}

/// The device an `m.rtc.member` came from. `member.device_id` is only the
/// sender's claim; the device that actually sent the (encrypted) event is
/// known when decryption attributed one, so that one wins and a different claim
/// is ignored. A claim can be checked only on an encrypted event whose sender
/// device is known; otherwise it is kept, and with no claim the sender device
/// stands in.
pub fn member_device(claimed: Option<&str>, sender_device: Option<&str>) -> Option<String> {
    match (sender_device, claimed) {
        (Some(actual), Some(claimed)) if actual != claimed => {
            tracing::debug!("rtc: member event claims device {claimed} but was sent from {actual}; using {actual}");
            Some(actual.to_owned())
        }
        (Some(actual), _) => Some(actual.to_owned()),
        (None, claimed) => claimed.map(str::to_owned),
    }
}

/// A [`CallMember`] from a joined `m.rtc.member` entry; `None` when the user
/// or device cannot be established.
pub fn call_member_from(e: &StickyEntry) -> Option<CallMember> {
    let m: MemberContent = serde_json::from_value(e.content.clone()).ok()?;
    if m.member.user_id.as_deref().is_some_and(|u| u != e.sender) {
        return None;
    }
    let device_id = member_device(m.member.device_id.as_deref(), e.sender_device.as_deref())?;
    // A transport that names only the SFU `url` is kept with an empty service
    // URL: it cannot be subscribed to, but the member is not attributed to any
    // other SFU either (see `transport_map`).
    let foci_preferred: Vec<LkFocus> = m
        .transports
        .iter()
        .flat_map(|t| t.published.iter())
        .filter(|p| transport_is_livekit(&p.kind))
        .map(|p| LkFocus {
            service_url: p.livekit_service_url.clone().unwrap_or_default(),
            alias: String::new(),
            sfu_url: p.url.clone(),
            server_name: p.server_name.clone(),
        })
        .filter(|f| !f.key().is_empty())
        .collect();
    let app = m.application.as_ref();
    let joined_at = MilliSecondsSinceUnixEpoch(UInt::new(e.start_ms).unwrap_or(UInt::MIN));
    Some(CallMember {
        user_id: e.sender.clone(),
        device_id,
        state_key: m.member.id.clone(),
        joined_at,
        focus_selection: SELECTION_MULTI_SFU.to_owned(),
        foci_preferred,
        member_id: m.member.id.clone(),
        source: MemberSource::MatrixRtc,
        intent: app.and_then(|a| a.intent.clone().or_else(|| a.m_call_intent.clone())),
        capabilities: app.and_then(|a| a.capabilities.clone()),
        claimed_at_ms: None,
    })
}

/// One member per `(user, device)`, the newest join: a device that restarted
/// without leaving still has its old session live for a few minutes, and only
/// the session it is in now counts. Keeps first-seen order; on a tie the first
/// entry stays.
pub fn dedupe_by_device(members: Vec<CallMember>) -> Vec<CallMember> {
    let mut out: Vec<CallMember> = Vec::with_capacity(members.len());
    let mut at: std::collections::HashMap<(String, String), usize> = std::collections::HashMap::new();
    for m in members {
        match at.get(&m.key()) {
            Some(&i) => {
                if m.joined_at > out[i].joined_at {
                    out[i] = m;
                }
            }
            None => {
                at.insert(m.key(), out.len());
                out.push(m);
            }
        }
    }
    out
}

/// One entry per `(user, device)`: the MatrixRTC member wins, keeping the
/// legacy member's transports when the newer event names none.
pub fn merge_members(legacy: Vec<CallMember>, mrtc: Vec<CallMember>) -> Vec<CallMember> {
    let legacy = dedupe_by_device(legacy);
    let mrtc = dedupe_by_device(mrtc);
    let mut out: Vec<CallMember> = Vec::with_capacity(legacy.len() + mrtc.len());
    for mut m in mrtc {
        if let Some(l) = legacy.iter().find(|l| l.key() == m.key()) {
            // Also when its transports name no JWT service we can reach (an SFU URL only).
            if !m.foci_preferred.iter().any(|f| usable_service_url(&f.service_url)) {
                m.foci_preferred = l.foci_preferred.clone();
            }
        }
        out.push(m);
    }
    for l in legacy {
        if !out.iter().any(|m| m.key() == l.key()) {
            out.push(l);
        }
    }
    out
}

/// Our own `m.rtc.member` joins from this device that are still live under a
/// member id other than `current_member_id`: ghosts of an earlier session that
/// ended without leaving. Returns `(slot_id, member_id)` for each, so the new
/// session can withdraw them instead of showing peers two members for one device.
pub fn stale_own_members(
    entries: &[StickyEntry],
    own_user: &str,
    own_device: &str,
    current_member_id: &str,
    now_ms: u64,
) -> Vec<(String, String)> {
    let mut out: Vec<(String, String)> = Vec::new();
    for e in entries {
        if e.event_type != MEMBER_TYPE || e.sender != own_user || e.end_ms() <= now_ms {
            continue;
        }
        let Ok(m) = serde_json::from_value::<MemberContent>(e.content.clone()) else {
            continue;
        };
        if !m.is_join() || m.member.id == current_member_id {
            continue;
        }
        let device = member_device(m.member.device_id.as_deref(), e.sender_device.as_deref());
        if device.as_deref() != Some(own_device) || out.iter().any(|(_, id)| *id == m.member.id) {
            continue;
        }
        out.push((m.slot_id.clone(), m.member.id.clone()));
    }
    out
}

/// The MatrixRTC members implied by `entries` for a room whose slot is `slot`.
pub fn live_mrtc_members(
    entries: &[StickyEntry],
    slot: Option<&SlotContent>,
    sender_in_room: &dyn Fn(&str) -> bool,
    room_encrypted: bool,
    now_ms: u64,
) -> Vec<CallMember> {
    let members = entries
        .iter()
        .filter(|e| e.event_type == MEMBER_TYPE)
        .filter_map(|e| {
            let m: MemberContent = serde_json::from_value(e.content.clone()).ok()?;
            if !m.slot_id.eq_ignore_ascii_case(CALL_SLOT_ID) {
                return None;
            }
            if !member_is_joined(&m, e, slot, sender_in_room(&e.sender), room_encrypted, now_ms) {
                return None;
            }
            call_member_from(e)
        })
        .collect();
    dedupe_by_device(members)
}

/// What the room's `m.call` slot state looks like.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SlotState {
    /// No slot event under any of the slot's state keys.
    Absent,
    /// An event is there but is not a slot we can read: emptied content (how a
    /// state event is cleared), or a shape we do not know.
    Unreadable,
    /// A slot, open or closed.
    Present(SlotContent),
}

/// The slot state from the full JSON of the room's slot events, each with the
/// state key it lives under. The first readable slot wins; unreadable events
/// only count when no key holds a readable one. Also returns the state key a
/// replacement belongs under: the readable slot's own key, else [`CALL_SLOT_ID`].
pub fn slot_state_of(events: &[(&'static str, serde_json::Value)]) -> (SlotState, &'static str) {
    let mut unreadable = false;
    for (key, v) in events {
        match v.get("content").and_then(|c| serde_json::from_value::<SlotContent>(c.clone()).ok()) {
            Some(c) => return (SlotState::Present(c), key),
            None => unreadable = true,
        }
    }
    if unreadable {
        (SlotState::Unreadable, CALL_SLOT_ID)
    } else {
        (SlotState::Absent, CALL_SLOT_ID)
    }
}

/// What one state key's store read tells the slot state: nothing for a key with
/// no event, the event's JSON otherwise. An event that is not JSON, and a read
/// that failed, are as unreadable as one with odd content: a transient store
/// error must not make a closed slot look missing (and so be reopened).
pub fn slot_event_of_read<E>(read: Result<Option<String>, E>) -> Option<serde_json::Value> {
    match read {
        Ok(None) => None,
        Ok(Some(json)) => Some(serde_json::from_str(&json).unwrap_or(serde_json::Value::Null)),
        Err(_) => Some(serde_json::Value::Null),
    }
}

/// The room's `m.call` slot state and the state key it lives under, telling a
/// missing slot from one that is there but cleared or unreadable.
pub async fn read_slot_state_keyed(room: &Room) -> (SlotState, &'static str) {
    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    let mut events: Vec<(&'static str, serde_json::Value)> = Vec::new();
    for key in SLOT_KEYS {
        let read = room
            .get_state_event(StateEventType::from(SLOT_TYPE), key)
            .await
            .map(|found| {
                found.map(|raw| match &raw {
                    RawAnySyncOrStrippedState::Sync(r) => r.json().get().to_owned(),
                    RawAnySyncOrStrippedState::Stripped(r) => r.json().get().to_owned(),
                })
            });
        if let Some(event) = slot_event_of_read(read) {
            events.push((key, event));
        }
    }
    slot_state_of(&events)
}

/// The room's `m.call` slot state; see [`read_slot_state_keyed`].
pub async fn read_slot_state(room: &Room) -> SlotState {
    read_slot_state_keyed(room).await.0
}

/// The room's `m.call` slot, if it has a readable one.
pub async fn read_slot(room: &Room) -> Option<SlotContent> {
    match read_slot_state(room).await {
        SlotState::Present(c) => Some(c),
        SlotState::Absent | SlotState::Unreadable => None,
    }
}

/// The live MatrixRTC (sticky) members of `room`, excluding our own device,
/// whose senders pass `check`.
pub async fn read_mrtc_members(
    room: &Room,
    own_user: &UserId,
    own_device: &DeviceId,
    check: JoinCheck,
) -> Vec<CallMember> {
    let now = now_ms();
    let entries = live_entries(room);
    if entries.is_empty() {
        return Vec::new();
    }
    let in_room = senders_to_keep(room, entries.iter().map(|e| e.sender.clone()).collect(), check).await;
    let slot = read_slot(room).await;
    let encrypted = room.encryption_state().is_encrypted();
    let mut members: Vec<CallMember> =
        live_mrtc_members(&entries, slot.as_ref(), &|u| in_room.contains(u), encrypted, now)
            .into_iter()
            .filter(|m| !(m.user_id == own_user.as_str() && m.device_id == own_device.as_str()))
            .collect();
    let account = room.client().user_id().map(|u| u.to_string()).unwrap_or_default();
    join_times().lock().stable(&account, room.room_id().as_str(), &mut members);
    members
}

fn join_times() -> &'static parking_lot::Mutex<JoinTimes> {
    static JOIN_TIMES: std::sync::OnceLock<parking_lot::Mutex<JoinTimes>> = std::sync::OnceLock::new();
    JOIN_TIMES.get_or_init(Default::default)
}

/// Forget the join times kept for `account`'s view of `room_id`: its call ended.
pub fn forget_join_times(account: &str, room_id: &str) {
    join_times().lock().forget_room(account, room_id);
}

/// Legacy and MatrixRTC members together, excluding our own device, whose
/// senders are joined to the room (the member list is fetched if it must be).
pub async fn read_all_live_members(room: &Room, own_user: &UserId, own_device: &DeviceId) -> Vec<CallMember> {
    read_all_live_members_checked(room, own_user, own_device, JoinCheck::Strict).await
}

/// [`read_all_live_members`] with the sender check of your choice; `Cached` and
/// `KnownJoined` never ask the homeserver. Anything that decides who receives a
/// media key must use `Strict` or `KnownJoined`, never `Cached`.
pub async fn read_all_live_members_checked(
    room: &Room,
    own_user: &UserId,
    own_device: &DeviceId,
    check: JoinCheck,
) -> Vec<CallMember> {
    let legacy = read_live_members(room, own_user, own_device, check).await;
    if !legacy.is_empty() && room.encryption_state().is_encrypted() {
        // Legacy membership is a state event, which is never encrypted: those
        // members count in an encrypted room by the old protocol's rules (only
        // the sticky MatrixRTC ones must have arrived encrypted).
        tracing::debug!("rtc: {} cleartext legacy call member(s) accepted in an encrypted room", legacy.len());
    }
    let mrtc = read_mrtc_members(room, own_user, own_device, check).await;
    merge_members(legacy, mrtc)
}

/// A member's `joined_at` is the time of its newest sticky event, which moves
/// forward with every refresh. Keep the first time each `member.id` was seen
/// instead, and forget ids that are no longer live, so join order stays put.
pub fn stable_join_times(cache: &mut std::collections::HashMap<String, u64>, members: &mut [CallMember]) {
    for m in members.iter_mut() {
        let seen = u64::from(m.joined_at.0);
        let first = *cache.entry(m.member_id.clone()).or_insert(seen);
        m.joined_at = MilliSecondsSinceUnixEpoch(UInt::new(first.min(seen)).unwrap_or(UInt::MIN));
    }
    let live: HashSet<&str> = members.iter().map(|m| m.member_id.as_str()).collect();
    cache.retain(|id, _| live.contains(id.as_str()));
}

/// The first-seen join times of every room's members, per signed-in account:
/// two accounts in one room each have their own view, and one's refresh must
/// not purge the other's. A room is held only while it has live members, and
/// at most [`JoinTimes::MAX_ROOMS`] at once: inserting a room when full evicts
/// the room read longest ago. The room of a call we are in is read on every
/// reconcile pass, so it is the last to go; reading a room without a call
/// never evicts anything.
///
/// "Joined" is when we first saw the member's `member.id`, taken from its
/// newest sticky event: a member already in the call when we looked gets the
/// time of its latest refresh, not its real join. Order among members who were
/// there before we were is therefore by that refresh time; it is stable from
/// then on.
#[derive(Default)]
pub struct JoinTimes {
    rooms: std::collections::HashMap<(String, String), RoomJoinTimes>,
    /// Counts reads; stamps the room read.
    reads: u64,
}

struct RoomJoinTimes {
    first_seen: std::collections::HashMap<String, u64>,
    last_read: u64,
}

impl JoinTimes {
    pub const MAX_ROOMS: usize = 64;

    /// [`stable_join_times`] for `account`'s members of `room_id`.
    pub fn stable(&mut self, account: &str, room_id: &str, members: &mut [CallMember]) {
        let key = (account.to_owned(), room_id.to_owned());
        self.reads += 1;
        let mut first_seen = self.rooms.remove(&key).map(|r| r.first_seen).unwrap_or_default();
        stable_join_times(&mut first_seen, members);
        if first_seen.is_empty() {
            return;
        }
        // The room is out of the map here, so a full map means making room.
        if self.rooms.len() >= Self::MAX_ROOMS {
            let oldest = self.rooms.iter().min_by_key(|(_, r)| r.last_read).map(|(k, _)| k.clone());
            if let Some(oldest) = oldest {
                self.rooms.remove(&oldest);
            }
        }
        self.rooms.insert(key, RoomJoinTimes { first_seen, last_read: self.reads });
    }

    pub fn forget_room(&mut self, account: &str, room_id: &str) {
        self.rooms.remove(&(account.to_owned(), room_id.to_owned()));
    }

    pub fn len(&self) -> usize {
        self.rooms.len()
    }

    pub fn holds(&self, account: &str, room_id: &str) -> bool {
        self.rooms.contains_key(&(account.to_owned(), room_id.to_owned()))
    }
}

/// LiveKit identity -> `(user, device)` for the MatrixRTC members, whose
/// identity is a hash of their user and `member.id` (MSC4195).
pub fn hashed_identities(members: &[CallMember]) -> std::collections::HashMap<String, (String, String)> {
    members
        .iter()
        .filter(|m| m.source == MemberSource::MatrixRtc)
        .map(|m| {
            (
                super::identity::livekit_participant_identity(&m.user_id, &m.member_id),
                (m.user_id.clone(), m.device_id.clone()),
            )
        })
        .collect()
}

/// The "call in progress" view of MatrixRTC members.
pub fn participants_from_members(members: &[CallMember]) -> Vec<Participant> {
    members
        .iter()
        .map(|m| Participant {
            user_id: m.user_id.clone(),
            joined_at: m.joined_at,
            intent: match m.intent.as_deref() {
                Some("m.video" | "video") => Some("video"),
                Some("m.audio" | "audio") => Some("audio"),
                _ => None,
            },
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::client::rtc::model::{MemberContent, SlotContent};
    use crate::client::rtc::sticky::StickyEntry;
    use serde_json::json;

    const NOW: u64 = 10_000_000;

    fn slot(open: bool, encrypted: bool) -> SlotContent {
        let mut v = json!({"status": if open {"open"} else {"closed"}});
        if open {
            v["application"] = json!({"type": "m.call"});
        }
        if encrypted {
            v["encryption"] = json!({"type": "m.per_member"});
        }
        serde_json::from_value(v).unwrap()
    }

    fn join_content(user: &str, device: Option<&str>) -> serde_json::Value {
        let mut member = json!({"id": "mid1", "membership": "join"});
        if let Some(d) = device {
            member["device_id"] = json!(d);
        }
        member["user_id"] = json!(user);
        json!({
            "slot_id": "m.call#room",
            "member": member,
            "application": {"type": "m.call", "intent": "m.video", "capabilities": ["m.render_audio"]},
            "transports": {"published": [{"type": "livekit",
                "url": "wss://sfu.a.org", "livekit_service_url": "https://jwt.a.org"}],
                "can_subscribe": ["livekit"]},
            "sticky_key": "mid1"
        })
    }

    fn entry(user: &str, content: serde_json::Value, encrypted: bool, start: u64, dur: u64) -> StickyEntry {
        StickyEntry {
            event_id: "$e".into(),
            sender: user.into(),
            event_type: crate::client::rtc::model::MEMBER_TYPE.into(),
            sticky_key: "mid1".into(),
            start_ms: start,
            duration_ms: dur,
            was_encrypted: encrypted,
            sender_device: None,
            content,
        }
    }

    fn joined(user: &str, e: &StickyEntry, slot: Option<&SlotContent>, in_room: bool, room_enc: bool) -> bool {
        let m: MemberContent = serde_json::from_value(e.content.clone()).unwrap();
        member_is_joined(&m, e, slot, in_room, room_enc, NOW) && !user.is_empty()
    }

    #[test]
    fn open_slot_and_fresh_event_means_joined() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        assert!(joined("@a:x", &e, Some(&slot(true, true)), true, true));
    }

    #[test]
    fn closed_slot_means_left() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        assert!(!joined("@a:x", &e, Some(&slot(false, true)), true, true));
    }

    #[test]
    fn missing_slot_means_left() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        assert!(!joined("@a:x", &e, None, true, true));
    }

    #[test]
    fn sender_not_in_room_means_left() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        assert!(!joined("@a:x", &e, Some(&slot(true, true)), false, true));
    }

    #[test]
    fn expired_member_is_left() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 4_000_000, 3_600_000);
        assert!(!joined("@a:x", &e, Some(&slot(true, true)), true, true));
    }

    #[test]
    fn cleartext_member_in_encrypted_room_is_left() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), false, NOW - 1000, 3_600_000);
        assert!(!joined("@a:x", &e, Some(&slot(true, true)), true, true));
    }

    #[test]
    fn slot_without_encryption_in_encrypted_room_means_left() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        assert!(!joined("@a:x", &e, Some(&slot(true, false)), true, true));
    }

    #[test]
    fn slot_with_encryption_in_plain_room_means_left() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), false, NOW - 1000, 3_600_000);
        assert!(!joined("@a:x", &e, Some(&slot(true, true)), true, false));
        assert!(joined("@a:x", &e, Some(&slot(true, false)), true, false));
    }

    #[test]
    fn leave_event_is_not_a_join() {
        let leave = json!({"slot_id": "m.call#room",
            "member": {"id": "mid1", "membership": "leave"}, "sticky_key": "mid1"});
        let e = entry("@a:x", leave, true, NOW - 1000, 3_600_000);
        assert!(!joined("@a:x", &e, Some(&slot(true, true)), true, true));
    }

    #[test]
    fn call_member_takes_transport_and_intent() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        let m = call_member_from(&e).unwrap();
        assert_eq!((m.user_id.as_str(), m.device_id.as_str()), ("@a:x", "D"));
        assert_eq!(m.member_id, "mid1");
        assert_eq!(m.source, MemberSource::MatrixRtc);
        assert_eq!(m.focus_selection, "multi_sfu");
        assert_eq!(m.foci_preferred[0].service_url, "https://jwt.a.org");
        assert_eq!(m.foci_preferred[0].sfu_url.as_deref(), Some("wss://sfu.a.org"));
        assert_eq!(m.intent.as_deref(), Some("m.video"));
        assert_eq!(m.capabilities.as_deref(), Some(&["m.render_audio".to_owned()][..]));
    }

    #[test]
    fn device_falls_back_to_the_decrypting_device() {
        let mut e = entry("@a:x", join_content("@a:x", None), true, NOW - 1000, 3_600_000);
        e.sender_device = Some("SENDERDEV".into());
        assert_eq!(call_member_from(&e).unwrap().device_id, "SENDERDEV");
    }

    #[test]
    fn the_sending_device_beats_a_different_claimed_one() {
        // An encrypted event from device REAL claiming to be device D.
        let mut e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        e.sender_device = Some("REAL".into());
        assert_eq!(call_member_from(&e).unwrap().device_id, "REAL");
        // A claim that agrees is simply kept.
        e.sender_device = Some("D".into());
        assert_eq!(call_member_from(&e).unwrap().device_id, "D");
    }

    #[test]
    fn an_uncheckable_claim_is_kept() {
        // Cleartext (or an unattributed sender device): nothing to compare with.
        let e = entry("@a:x", join_content("@a:x", Some("D")), false, NOW - 1000, 3_600_000);
        assert_eq!(call_member_from(&e).unwrap().device_id, "D");
        assert_eq!(member_device(Some("D"), None).as_deref(), Some("D"));
        assert_eq!(member_device(None, Some("S")).as_deref(), Some("S"));
        assert_eq!(member_device(None, None), None);
    }

    #[test]
    fn our_own_stale_join_is_matched_by_the_sending_device_not_the_claim() {
        // A join claiming to be another device but sent from this one is ours.
        let mut e = entry("@me:x", join_content("@me:x", Some("OTHER")), true, NOW - 1_000, 300_000);
        e.sender_device = Some("D".into());
        assert_eq!(stale_own_members(&[e], "@me:x", "D", "current", NOW).len(), 1);
    }

    #[test]
    fn a_transport_with_only_an_sfu_url_has_no_service_url() {
        let mut c = join_content("@a:x", Some("D"));
        c["transports"]["published"][0] = json!({"type": "livekit", "url": "wss://sfu.a.org"});
        let m = call_member_from(&entry("@a:x", c, true, NOW - 1000, 3_600_000)).unwrap();
        assert_eq!(m.foci_preferred.len(), 1);
        assert_eq!(m.foci_preferred[0].service_url, "");
        assert_eq!(m.foci_preferred[0].sfu_url.as_deref(), Some("wss://sfu.a.org"));
    }

    #[test]
    fn a_member_with_an_unusable_transport_borrows_the_legacy_focus() {
        let mut c = join_content("@a:x", Some("D"));
        c["transports"]["published"][0] = json!({"type": "livekit", "url": "wss://sfu.a.org"});
        let mrtc = call_member_from(&entry("@a:x", c, true, NOW - 1000, 3_600_000)).unwrap();
        let legacy = CallMember {
            user_id: "@a:x".into(), device_id: "D".into(), state_key: "_@a:x_D_m.call".into(),
            joined_at: MilliSecondsSinceUnixEpoch::now(), focus_selection: "multi_sfu".into(),
            foci_preferred: vec![LkFocus {
                service_url: "https://jwt.a.org".into(), alias: "!r".into(), sfu_url: None, server_name: None,
            }],
            member_id: "@a:x:D".into(), source: MemberSource::Legacy, intent: None, capabilities: None, claimed_at_ms: None,
        };
        let merged = merge_members(vec![legacy], vec![mrtc]);
        assert_eq!(merged.len(), 1);
        assert_eq!(merged[0].source, MemberSource::MatrixRtc);
        assert_eq!(merged[0].foci_preferred[0].service_url, "https://jwt.a.org");
    }

    // ── The slot state ──────────────────────────────────────────────────────

    /// The slot events of the test, all under the default state key.
    fn state_of(events: &[serde_json::Value]) -> SlotState {
        let keyed: Vec<(&'static str, serde_json::Value)> = events.iter().map(|v| (SLOT_KEYS[0], v.clone())).collect();
        slot_state_of(&keyed).0
    }

    #[test]
    fn a_missing_slot_is_absent_and_a_cleared_one_is_not() {
        assert_eq!(state_of(&[]), SlotState::Absent);
        let open = json!({"type": "org.matrix.msc4143.rtc.slot", "content": {"status": "open", "application": {"type": "m.call"}}});
        let cleared = json!({"type": "org.matrix.msc4143.rtc.slot", "content": {}});
        assert!(matches!(state_of(&[open.clone()]), SlotState::Present(s) if s.is_open()));
        assert_eq!(state_of(&[cleared.clone()]), SlotState::Unreadable);
        assert_eq!(state_of(&[serde_json::Value::Null]), SlotState::Unreadable);
        // A readable slot under the other state key beats an unreadable one.
        assert!(matches!(state_of(&[cleared, open]), SlotState::Present(_)));
    }

    #[test]
    fn a_closed_slot_is_present_not_absent() {
        let closed = json!({"content": {"status": "closed"}});
        assert!(matches!(state_of(&[closed]), SlotState::Present(s) if !s.is_open()));
    }

    #[test]
    fn a_replacement_goes_under_the_state_key_the_slot_lives_under() {
        let open = json!({"content": {"status": "open", "application": {"type": "m.call"}}});
        let (state, key) = slot_state_of(&[("m.call#ROOM", open.clone())]);
        assert!(matches!(state, SlotState::Present(_)));
        assert_eq!(key, "m.call#ROOM", "not a second slot under m.call#room");
        let (_, key) = slot_state_of(&[("m.call#room", open)]);
        assert_eq!(key, "m.call#room");
        // Nothing readable: a new slot takes the MSC's key.
        assert_eq!(slot_state_of(&[]).1, CALL_SLOT_ID);
        assert_eq!(slot_state_of(&[("m.call#ROOM", json!({"content": {}}))]).1, CALL_SLOT_ID);
    }

    #[test]
    fn a_failed_store_read_is_unreadable_not_absent() {
        // No event under the key: nothing to report.
        assert_eq!(slot_event_of_read::<()>(Ok(None)), None);
        // A store error is as unreadable as a cleared event: it must never
        // look like a missing slot, which a call would then write.
        let failed = slot_event_of_read(Err(()));
        assert_eq!(failed, Some(serde_json::Value::Null));
        assert_eq!(state_of(&[failed.unwrap()]), SlotState::Unreadable);
        // Not even JSON: unreadable too.
        assert_eq!(slot_event_of_read::<()>(Ok(Some("not json".into()))), Some(serde_json::Value::Null));
        // A good event passes through.
        let ev = slot_event_of_read::<()>(Ok(Some(r#"{"content":{"status":"closed"}}"#.into()))).unwrap();
        assert!(matches!(state_of(&[ev]), SlotState::Present(s) if !s.is_open()));
    }

    #[test]
    fn member_without_any_device_is_skipped() {
        let mut e = entry("@a:x", join_content("@a:x", None), true, NOW - 1000, 3_600_000);
        e.sender_device = None;
        assert!(call_member_from(&e).is_none());
    }

    #[test]
    fn member_claiming_another_user_is_skipped() {
        let e = entry("@a:x", join_content("@someone-else:x", Some("D")), true, NOW - 1000, 3_600_000);
        assert!(call_member_from(&e).is_none());
    }

    #[test]
    fn dual_stack_member_counted_once() {
        let legacy = CallMember {
            user_id: "@a:x".into(),
            device_id: "D".into(),
            state_key: "_@a:x_D_m.call".into(),
            joined_at: MilliSecondsSinceUnixEpoch::now(),
            focus_selection: "multi_sfu".into(),
            foci_preferred: vec![],
            member_id: "@a:x:D".into(),
            source: MemberSource::Legacy,
            intent: None,
            capabilities: None,
            claimed_at_ms: None,
        };
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        let mrtc = call_member_from(&e).unwrap();
        let merged = merge_members(vec![legacy], vec![mrtc]);
        assert_eq!(merged.len(), 1);
        assert_eq!(merged[0].source, MemberSource::MatrixRtc);
    }

    #[test]
    fn legacy_only_and_mrtc_only_members_both_survive_merge() {
        let mk = |u: &str, d: &str, s: MemberSource| CallMember {
            user_id: u.into(), device_id: d.into(), state_key: String::new(),
            joined_at: MilliSecondsSinceUnixEpoch::now(), focus_selection: String::new(),
            foci_preferred: vec![], member_id: String::new(), source: s, intent: None, capabilities: None, claimed_at_ms: None,
        };
        let merged = merge_members(
            vec![mk("@a:x", "D", MemberSource::Legacy)],
            vec![mk("@b:x", "D", MemberSource::MatrixRtc)],
        );
        assert_eq!(merged.len(), 2);
    }

    fn entry_with_id(user: &str, id: &str, device: &str, start: u64) -> StickyEntry {
        let mut c = join_content(user, Some(device));
        c["member"]["id"] = json!(id);
        c["sticky_key"] = json!(id);
        entry(user, c, true, start, 300_000)
    }

    #[test]
    fn a_restarted_device_counts_once_with_its_newest_join() {
        let old = entry_with_id("@a:x", "old", "D", NOW - 200_000);
        let new = entry_with_id("@a:x", "new", "D", NOW - 5_000);
        let other = entry_with_id("@a:x", "phone", "P", NOW - 100_000);
        let s = slot(true, true);
        for entries in [vec![old.clone(), new.clone(), other.clone()], vec![new, other, old]] {
            let got = live_mrtc_members(&entries, Some(&s), &|_| true, true, NOW);
            let mut ids: Vec<&str> = got.iter().map(|m| m.member_id.as_str()).collect();
            ids.sort();
            assert_eq!(ids, vec!["new", "phone"]);
        }
    }

    #[test]
    fn dedupe_keeps_the_newest_per_device_and_the_first_on_a_tie() {
        let mk = |id: &str, dev: &str, at: u64| {
            let mut m = cm(None, "@a:x", at);
            m.device_id = dev.into();
            m.member_id = id.into();
            m
        };
        let got = dedupe_by_device(vec![mk("a", "D", 5), mk("b", "E", 1), mk("c", "D", 9), mk("d", "E", 1)]);
        assert_eq!(got.iter().map(|m| m.member_id.as_str()).collect::<Vec<_>>(), vec!["c", "b"]);
        assert!(dedupe_by_device(Vec::new()).is_empty());
    }

    #[test]
    fn merging_dedupes_each_side_before_combining() {
        let mk = |id: &str, at: u64, source: MemberSource| {
            let mut m = cm(None, "@a:x", at);
            m.member_id = id.into();
            m.source = source;
            m
        };
        let merged = merge_members(
            vec![mk("l1", 1, MemberSource::Legacy), mk("l2", 2, MemberSource::Legacy)],
            vec![mk("r1", 3, MemberSource::MatrixRtc), mk("r2", 7, MemberSource::MatrixRtc)],
        );
        assert_eq!(merged.len(), 1);
        assert_eq!(merged[0].member_id, "r2");
    }

    #[test]
    fn our_earlier_session_on_this_device_is_stale() {
        let ents = vec![
            entry_with_id("@me:x", "old", "D", NOW - 200_000),
            entry_with_id("@me:x", "current", "D", NOW - 1_000),
            entry_with_id("@me:x", "phone", "P", NOW - 1_000),
            entry_with_id("@them:x", "theirs", "D", NOW - 1_000),
            entry_with_id("@me:x", "expired", "D", NOW - 400_000),
        ];
        assert_eq!(
            stale_own_members(&ents, "@me:x", "D", "current", NOW),
            vec![("m.call#room".to_owned(), "old".to_owned())]
        );
    }

    #[test]
    fn leaves_and_withdrawals_are_not_stale_joins() {
        let mut left = entry_with_id("@me:x", "gone", "D", NOW - 1_000);
        left.content["member"]["membership"] = json!("leave");
        let mut withdrawn = entry_with_id("@me:x", "w", "D", NOW - 1_000);
        withdrawn.content = json!({"sticky_key": "w"});
        assert!(stale_own_members(&[left, withdrawn], "@me:x", "D", "current", NOW).is_empty());
    }

    #[test]
    fn a_stale_join_without_a_device_is_matched_by_the_sending_device() {
        let mut e = entry("@me:x", join_content("@me:x", None), true, NOW - 1_000, 300_000);
        e.sender_device = Some("D".into());
        assert_eq!(stale_own_members(&[e.clone()], "@me:x", "D", "current", NOW).len(), 1);
        assert!(stale_own_members(&[e], "@me:x", "OTHER", "current", NOW).is_empty());
    }

    fn cm(intent: Option<&str>, user: &str, at: u64) -> CallMember {
        CallMember {
            user_id: user.into(), device_id: "D".into(), state_key: String::new(),
            joined_at: MilliSecondsSinceUnixEpoch(UInt::new(at).unwrap()),
            focus_selection: String::new(), foci_preferred: vec![], member_id: String::new(),
            source: MemberSource::MatrixRtc, intent: intent.map(str::to_owned), capabilities: None, claimed_at_ms: None,
        }
    }

    #[test]
    fn room_with_only_mrtc_members_has_participants_and_intent() {
        let p = participants_from_members(&[cm(Some("m.video"), "@a:x", 5), cm(Some("audio"), "@b:x", 1), cm(None, "@c:x", 9)]);
        assert_eq!(p.iter().map(|p| p.intent).collect::<Vec<_>>(), vec![Some("video"), Some("audio"), None]);
        let (users, intent) = crate::client::rtc::members::summarize_participants(p);
        assert_eq!(users, vec!["@b:x", "@a:x", "@c:x"]);
        assert_eq!(intent, "video");
    }

    #[test]
    fn closed_slot_has_no_participants() {
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        let none = live_mrtc_members(&[e], Some(&slot(false, true)), &|_| true, true, NOW);
        assert!(none.is_empty());
    }

    #[test]
    fn hashed_identities_cover_matrixrtc_members_only() {
        let mrtc = call_member_from(&entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000)).unwrap();
        let mut legacy = mrtc.clone();
        legacy.source = MemberSource::Legacy;
        legacy.user_id = "@b:x".into();
        let map = hashed_identities(&[mrtc, legacy]);
        let id = crate::client::rtc::identity::livekit_participant_identity("@a:x", "mid1");
        assert_eq!(map.len(), 1);
        assert_eq!(map[&id], ("@a:x".to_owned(), "D".to_owned()));
    }

    #[test]
    fn unknown_slot_encryption_type_is_not_joined() {
        let mut unknown = slot(true, true);
        unknown.encryption.as_mut().unwrap().kind = "m.something_new".into();
        let e = entry("@a:x", join_content("@a:x", Some("D")), true, NOW - 1000, 3_600_000);
        assert!(!joined("@a:x", &e, Some(&unknown), true, true));
    }

    #[test]
    fn a_members_join_time_survives_its_refreshes() {
        let mut cache = std::collections::HashMap::new();
        let mk = |id: &str, at: u64| {
            let mut e = entry("@a:x", join_content("@a:x", Some("D")), true, at, 3_600_000);
            e.content["member"]["id"] = serde_json::json!(id);
            call_member_from(&e).unwrap()
        };
        let mut first = vec![mk("mid1", 1_000)];
        stable_join_times(&mut cache, &mut first);
        assert_eq!(u64::from(first[0].joined_at.0), 1_000);
        // The same member refreshed 150 s later: the newest event is later, the join time is not.
        let mut refreshed = vec![mk("mid1", 151_000)];
        stable_join_times(&mut cache, &mut refreshed);
        assert_eq!(u64::from(refreshed[0].joined_at.0), 1_000);
        // Gone and back under a new member id: a new join.
        stable_join_times(&mut cache, &mut Vec::new());
        let mut again = vec![mk("mid2", 400_000)];
        stable_join_times(&mut cache, &mut again);
        assert_eq!(u64::from(again[0].joined_at.0), 400_000);
        // Forgotten ids do not linger.
        assert_eq!(cache.len(), 1);
    }

    #[test]
    fn two_accounts_in_one_room_keep_their_own_join_times() {
        let mk = |id: &str, at: u64| {
            let mut e = entry("@a:x", join_content("@a:x", Some("D")), true, at, 3_600_000);
            e.content["member"]["id"] = serde_json::json!(id);
            call_member_from(&e).unwrap()
        };
        let mut times = JoinTimes::default();
        let mut first = vec![mk("mid1", 1_000)];
        times.stable("@alice:x", "!r:x", &mut first);
        // Another account reads the same room with nobody live: alice's entry stays.
        times.stable("@bob:x", "!r:x", &mut Vec::new());
        let mut refreshed = vec![mk("mid1", 151_000)];
        times.stable("@alice:x", "!r:x", &mut refreshed);
        assert_eq!(u64::from(refreshed[0].joined_at.0), 1_000);
    }

    #[test]
    fn rooms_without_live_members_are_not_kept_and_the_total_is_bounded() {
        let mk = |id: &str| {
            let mut e = entry("@a:x", join_content("@a:x", Some("D")), true, 1_000, 3_600_000);
            e.content["member"]["id"] = serde_json::json!(id);
            call_member_from(&e).unwrap()
        };
        let mut times = JoinTimes::default();
        times.stable("@a:x", "!quiet:x", &mut Vec::new());
        assert_eq!(times.len(), 0, "an empty room leaves nothing behind");
        for i in 0..(JoinTimes::MAX_ROOMS + 20) {
            times.stable("@a:x", &format!("!r{i}:x"), &mut vec![mk("m")]);
        }
        assert_eq!(times.len(), JoinTimes::MAX_ROOMS);
        // A call that ended is forgotten at once.
        times.forget_room("@a:x", "!r0:x");
        times.forget_room("@a:x", "!r1:x");
        assert!(times.len() <= JoinTimes::MAX_ROOMS);
        let before = times.len();
        times.forget_room("@a:x", "!never:x");
        assert_eq!(times.len(), before);
    }

    #[test]
    fn a_full_map_evicts_the_room_read_longest_ago_and_only_for_a_room_with_members() {
        let mk = |id: &str| {
            let mut e = entry("@a:x", join_content("@a:x", Some("D")), true, 1_000, 3_600_000);
            e.content["member"]["id"] = serde_json::json!(id);
            call_member_from(&e).unwrap()
        };
        let mut times = JoinTimes::default();
        for i in 0..JoinTimes::MAX_ROOMS {
            times.stable("@a:x", &format!("!r{i}:x"), &mut vec![mk("m")]);
        }
        assert_eq!(times.len(), JoinTimes::MAX_ROOMS);
        // The call we are in is read again on every pass: it is recent.
        times.stable("@a:x", "!r0:x", &mut vec![mk("m")]);
        // Reading an idle room (nobody in it) evicts nothing.
        times.stable("@a:x", "!idle:x", &mut Vec::new());
        assert_eq!(times.len(), JoinTimes::MAX_ROOMS);
        assert!(times.holds("@a:x", "!r1:x"));
        // A new room with members pushes out the least recently read one: !r1,
        // not the active call's !r0.
        times.stable("@a:x", "!new:x", &mut vec![mk("m")]);
        assert_eq!(times.len(), JoinTimes::MAX_ROOMS);
        assert!(!times.holds("@a:x", "!r1:x"));
        assert!(times.holds("@a:x", "!r0:x"), "the active call's room stays");
        assert!(times.holds("@a:x", "!new:x"));
        // Re-reading a room already held never evicts.
        times.stable("@a:x", "!r2:x", &mut vec![mk("m")]);
        assert!(times.holds("@a:x", "!r3:x"));
    }
}
