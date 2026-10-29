//! Who is in a MatrixRTC call: the room's `m.rtc.slot`, the sticky
//! `m.rtc.member` events seen by [`super::sticky`], joined with the legacy
//! `org.matrix.msc3401.call.member` state members.

use std::collections::HashSet;

use matrix_sdk::{
    ruma::{events::room::member::MembershipState, events::StateEventType, DeviceId, MilliSecondsSinceUnixEpoch, UInt, UserId},
    Room,
};

use super::members::{
    read_live_members, senders_to_keep, CallMember, JoinCheck, LkFocus, MemberSource, Participant, SELECTION_MULTI_SFU,
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

/// A [`CallMember`] from a joined `m.rtc.member` entry; `None` when the user
/// or device cannot be established.
pub fn call_member_from(e: &StickyEntry) -> Option<CallMember> {
    let m: MemberContent = serde_json::from_value(e.content.clone()).ok()?;
    if m.member.user_id.as_deref().is_some_and(|u| u != e.sender) {
        return None;
    }
    let device_id = m.member.device_id.clone().or_else(|| e.sender_device.clone())?;
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
            if m.foci_preferred.is_empty() {
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
        let device = m.member.device_id.as_deref().or(e.sender_device.as_deref());
        if device != Some(own_device) || out.iter().any(|(_, id)| *id == m.member.id) {
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

/// The room's `m.call` slot, if it has one.
pub async fn read_slot(room: &Room) -> Option<SlotContent> {
    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    for key in SLOT_KEYS {
        let Ok(Some(raw)) = room.get_state_event(StateEventType::from(SLOT_TYPE), key).await else {
            continue;
        };
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(r) => r.json().get().to_owned(),
            RawAnySyncOrStrippedState::Stripped(r) => r.json().get().to_owned(),
        };
        let Ok(v) = serde_json::from_str::<serde_json::Value>(&json) else { continue };
        if let Some(c) = v.get("content").and_then(|c| serde_json::from_value(c.clone()).ok()) {
            return Some(c);
        }
    }
    None
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
    static JOIN_TIMES: std::sync::OnceLock<std::sync::Mutex<std::collections::HashMap<String, std::collections::HashMap<String, u64>>>> =
        std::sync::OnceLock::new();
    let mut all = JOIN_TIMES.get_or_init(Default::default).lock().unwrap_or_else(|p| p.into_inner());
    stable_join_times(all.entry(room.room_id().to_string()).or_default(), &mut members);
    members
}

/// Legacy and MatrixRTC members together, excluding our own device, whose
/// senders are joined to the room (the member list is fetched if it must be).
pub async fn read_all_live_members(room: &Room, own_user: &UserId, own_device: &DeviceId) -> Vec<CallMember> {
    read_all_live_members_checked(room, own_user, own_device, JoinCheck::Strict).await
}

/// [`read_all_live_members`] with the sender check of your choice; `Cached`
/// never asks the homeserver.
pub async fn read_all_live_members_checked(
    room: &Room,
    own_user: &UserId,
    own_device: &DeviceId,
    check: JoinCheck,
) -> Vec<CallMember> {
    let legacy = read_live_members(room, own_user, own_device, check).await;
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
            sender_device: Some("SENDERDEV".into()),
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
        let e = entry("@a:x", join_content("@a:x", None), true, NOW - 1000, 3_600_000);
        assert_eq!(call_member_from(&e).unwrap().device_id, "SENDERDEV");
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
            foci_preferred: vec![], member_id: String::new(), source: s, intent: None, capabilities: None,
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
            source: MemberSource::MatrixRtc, intent: intent.map(str::to_owned), capabilities: None,
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
}
