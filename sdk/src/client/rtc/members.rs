//! Live call members and the LiveKit SFU each one publishes on.
//!
//! Element Call (`compatibility` mode) selects SFUs with `multi_sfu`: every
//! client publishes on its own homeserver's SFU and subscribes on the SFU of
//! every other member. Legacy clients use `oldest_membership`, where all
//! members share the oldest member's SFU. This mirrors matrix-js-sdk's
//! `CallMembership.getTransport()`.

use std::collections::BTreeSet;

use matrix_sdk::ruma::{
    events::call::member::{ActiveFocus, CallMemberEventContent, Focus},
    DeviceId, MilliSecondsSinceUnixEpoch, UInt, UserId,
};

pub const SELECTION_MULTI_SFU: &str = "multi_sfu";
pub const SELECTION_OLDEST: &str = "oldest_membership";

/// A LiveKit focus: the JWT service URL and the room alias.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LkFocus {
    pub service_url: String,
    pub alias: String,
    /// The SFU WebSocket URL, when the member's event names one (MSC4195).
    pub sfu_url: Option<String>,
    /// Set when the member publishes on another homeserver's SFU.
    pub server_name: Option<String>,
}

impl LkFocus {
    /// What identifies this SFU: the JWT service URL when known, else the SFU URL.
    pub fn key(&self) -> &str {
        if self.service_url.is_empty() {
            self.sfu_url.as_deref().unwrap_or("")
        } else {
            &self.service_url
        }
    }
}

/// Which membership format a call member was read from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemberSource {
    Legacy,
    MatrixRtc,
}

/// One live (unexpired, joined) call member device.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CallMember {
    pub user_id: String,
    pub device_id: String,
    pub state_key: String,
    pub joined_at: MilliSecondsSinceUnixEpoch,
    pub focus_selection: String,
    pub foci_preferred: Vec<LkFocus>,
    /// The MatrixRTC `member.id` (for legacy members, the state key).
    pub member_id: String,
    pub source: MemberSource,
    /// `m.audio` / `m.video` (or the reference implementation's spelling).
    pub intent: Option<String>,
    pub capabilities: Option<Vec<String>>,
}

impl CallMember {
    pub fn key(&self) -> (String, String) {
        (self.user_id.clone(), self.device_id.clone())
    }
}

/// Extract the live members from one `org.matrix.msc3401.call.member` event.
///
/// Skips expired and leave memberships, non-call applications and our own
/// user+device (a stale membership of a previous session).
#[cfg(test)]
pub fn members_from_call_member(
    content: &CallMemberEventContent,
    state_key: &str,
    sender: &UserId,
    origin_server_ts: MilliSecondsSinceUnixEpoch,
    own_user: &UserId,
    own_device: &DeviceId,
) -> Vec<CallMember> {
    members_from_call_member_inner(content, state_key, sender, origin_server_ts, Some((own_user, own_device)))
}

fn members_from_call_member_inner(
    content: &CallMemberEventContent,
    state_key: &str,
    sender: &UserId,
    origin_server_ts: MilliSecondsSinceUnixEpoch,
    exclude: Option<(&UserId, &DeviceId)>,
) -> Vec<CallMember> {
    content
        .active_memberships(Some(origin_server_ts))
        .into_iter()
        .filter(|m| m.is_call())
        .filter(|m| !exclude.is_some_and(|(u, d)| sender == u && m.device_id() == d))
        .map(|m| {
            let focus_selection = match m.focus_active() {
                ActiveFocus::Livekit(a) => a.focus_selection.as_str().to_owned(),
                _ => String::new(),
            };
            let foci_preferred = m
                .foci_preferred()
                .iter()
                .filter_map(|f| match f {
                    Focus::Livekit(lk) if !lk.service_url.is_empty() => Some(LkFocus {
                        service_url: lk.service_url.clone(),
                        alias: lk.alias.clone(),
                        sfu_url: None,
                        server_name: None,
                    }),
                    _ => None,
                })
                .collect();
            CallMember {
                user_id: sender.to_string(),
                device_id: m.device_id().to_string(),
                state_key: state_key.to_owned(),
                joined_at: m.created_ts().unwrap_or(origin_server_ts),
                focus_selection,
                foci_preferred,
                member_id: state_key.to_owned(),
                source: MemberSource::Legacy,
                intent: match m.call_intent() {
                    Some(matrix_sdk::ruma::events::rtc::notification::CallIntent::Video) => Some("video".to_owned()),
                    Some(matrix_sdk::ruma::events::rtc::notification::CallIntent::Audio) => Some("audio".to_owned()),
                    _ => None,
                },
                capabilities: None,
            }
        })
        .collect()
}

/// The transport of the oldest membership, counting our own (`own`) too.
/// Ties are broken by state key so every client agrees.
fn oldest_transport(
    members: &[CallMember],
    own: Option<(MilliSecondsSinceUnixEpoch, &LkFocus)>,
) -> Option<LkFocus> {
    let mut best: Option<(MilliSecondsSinceUnixEpoch, &str, &LkFocus)> =
        own.map(|(ts, f)| (ts, "", f));
    for m in members {
        let Some(first) = m.foci_preferred.first() else {
            continue;
        };
        let cand = (m.joined_at, m.state_key.as_str(), first);
        if best.map_or(true, |b| (cand.0, cand.1) < (b.0, b.1)) {
            best = Some(cand);
        }
    }
    best.map(|(_, _, f)| f.clone())
}

/// The SFU a member publishes its media on, or `None` if its selection method
/// is unknown or it advertises no LiveKit focus.
pub fn transport_of(
    member: &CallMember,
    members: &[CallMember],
    own: Option<(MilliSecondsSinceUnixEpoch, &LkFocus)>,
) -> Option<LkFocus> {
    match member.focus_selection.as_str() {
        SELECTION_MULTI_SFU => member.foci_preferred.first().cloned(),
        SELECTION_OLDEST => oldest_transport(members, own),
        _ => None,
    }
}

/// The remote SFUs we must be subscribed to: every member's transport except
/// our own local SFU (which the primary connection already covers).
pub fn desired_sfus(
    members: &[CallMember],
    own: Option<(MilliSecondsSinceUnixEpoch, &LkFocus)>,
    local_service_url: &str,
) -> BTreeSet<String> {
    members
        .iter()
        .filter_map(|m| transport_of(m, members, own))
        .map(|f| f.service_url)
        .filter(|u| u != local_service_url)
        .collect()
}

/// Which SFU each live member publishes on, keyed by `(user_id, device_id)`.
/// Members whose transport can't be determined are left out.
pub fn transport_map(
    members: &[CallMember],
    own: Option<(MilliSecondsSinceUnixEpoch, &LkFocus)>,
) -> std::collections::HashMap<(String, String), String> {
    members
        .iter()
        .filter_map(|m| Some((m.key(), transport_of(m, members, own)?.service_url)))
        .collect()
}

/// One user with a live call membership, for the "call in progress" banner.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Participant {
    pub user_id: String,
    pub joined_at: MilliSecondsSinceUnixEpoch,
    /// `"audio"` / `"video"` from the membership's `m.call.intent`.
    pub intent: Option<&'static str>,
}

/// Distinct users, oldest join first, and the call's overall intent: `"video"`
/// if any member is in it with video, else `"audio"` if any declared audio,
/// else empty.
pub fn summarize_participants(mut participants: Vec<Participant>) -> (Vec<String>, String) {
    participants.sort_by(|a, b| (a.joined_at, &a.user_id).cmp(&(b.joined_at, &b.user_id)));
    let intent = if participants.iter().any(|p| p.intent == Some("video")) {
        "video"
    } else if participants.iter().any(|p| p.intent == Some("audio")) {
        "audio"
    } else {
        ""
    };
    let mut users: Vec<String> = Vec::new();
    for p in &participants {
        if !users.contains(&p.user_id) {
            users.push(p.user_id.clone());
        }
    }
    (users, intent.to_owned())
}

/// The live members of one `org.matrix.msc3401.call.member` event, from its
/// raw content. ruma's typed model is tried first (it also covers the old
/// `memberships` array); when it finds no membership, the flat session form is
/// read leniently: newer clients leave out fields ruma insists on (such as
/// `livekit_alias`), which makes ruma silently see an empty membership.
pub fn members_from_event_json(
    content: &serde_json::Value,
    state_key: &str,
    sender: &UserId,
    origin_server_ts: MilliSecondsSinceUnixEpoch,
    exclude: Option<(&UserId, &DeviceId)>,
    now_ms: u64,
) -> Vec<CallMember> {
    if let Ok(typed) = serde_json::from_value::<CallMemberEventContent>(content.clone()) {
        if !typed.memberships().is_empty() {
            return members_from_call_member_inner(&typed, state_key, sender, origin_server_ts, exclude);
        }
    }
    let Some(obj) = content.as_object() else { return Vec::new() };
    if obj.get("application").and_then(serde_json::Value::as_str) != Some("m.call") {
        return Vec::new();
    }
    let Some(device_id) = obj.get("device_id").and_then(serde_json::Value::as_str) else {
        return Vec::new();
    };
    if exclude.is_some_and(|(u, d)| sender == u && d.as_str() == device_id) {
        return Vec::new();
    }
    let origin_ms = u64::from(origin_server_ts.0);
    let base_ms = obj.get("created_ts").and_then(serde_json::Value::as_u64).unwrap_or(origin_ms);
    let expires_ms = obj.get("expires").and_then(serde_json::Value::as_u64).unwrap_or(4 * 3_600_000);
    if now_ms > base_ms.saturating_add(expires_ms) {
        return Vec::new();
    }
    let foci_preferred = obj
        .get("foci_preferred")
        .and_then(serde_json::Value::as_array)
        .into_iter()
        .flatten()
        .filter(|f| f.get("type").and_then(serde_json::Value::as_str) == Some("livekit"))
        .filter_map(|f| {
            let service_url = f.get("livekit_service_url")?.as_str()?.to_owned();
            (!service_url.is_empty()).then(|| LkFocus {
                service_url,
                alias: f.get("livekit_alias").and_then(serde_json::Value::as_str).unwrap_or_default().to_owned(),
                sfu_url: None,
                server_name: None,
            })
        })
        .collect();
    let focus_selection = content
        .pointer("/focus_active/focus_selection")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_owned();
    let intent = match obj.get("m.call.intent").and_then(serde_json::Value::as_str) {
        Some(i @ ("audio" | "video")) => Some(i.to_owned()),
        _ => None,
    };
    vec![CallMember {
        user_id: sender.to_string(),
        device_id: device_id.to_owned(),
        state_key: state_key.to_owned(),
        joined_at: MilliSecondsSinceUnixEpoch(UInt::new(base_ms).unwrap_or(UInt::MIN)),
        focus_selection,
        foci_preferred,
        member_id: state_key.to_owned(),
        source: MemberSource::Legacy,
        intent,
        capabilities: None,
    }]
}

/// The `call_id` of a call member event, from the flat session form or the
/// first membership of the old `memberships` array; empty when there is none.
pub fn call_id_of(content: &serde_json::Value) -> String {
    content
        .get("call_id")
        .or_else(|| content.pointer("/memberships/0/call_id"))
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_owned()
}

/// Every live call member of `room` (legacy format), optionally leaving out
/// one user+device, read from the raw state events.
pub async fn read_call_members(
    room: &matrix_sdk::Room,
    exclude: Option<(&UserId, &DeviceId)>,
) -> Vec<CallMember> {
    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    use matrix_sdk::ruma::events::StateEventType;

    let now_ms = u64::from(MilliSecondsSinceUnixEpoch::now().0);
    let mut members = Vec::new();
    for raw in room
        .get_state_events(StateEventType::from("org.matrix.msc3401.call.member"))
        .await
        .unwrap_or_default()
    {
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(r) => r.json().get(),
            RawAnySyncOrStrippedState::Stripped(_) => continue,
        };
        let Ok(v) = serde_json::from_str::<serde_json::Value>(json) else { continue };
        let (Some(sender), Some(state_key), Some(origin)) = (
            v.get("sender").and_then(serde_json::Value::as_str).and_then(|s| UserId::parse(s).ok()),
            v.get("state_key").and_then(serde_json::Value::as_str),
            v.get("origin_server_ts").and_then(serde_json::Value::as_u64),
        ) else {
            continue;
        };
        let origin = MilliSecondsSinceUnixEpoch(UInt::new(origin).unwrap_or(UInt::MIN));
        members.extend(members_from_event_json(
            v.get("content").unwrap_or(&serde_json::Value::Null),
            state_key,
            &sender,
            origin,
            exclude,
            now_ms,
        ));
    }
    members
}

/// Read every live remote call member of `room` from the local state store.
pub async fn read_live_members(
    room: &matrix_sdk::Room,
    own_user: &UserId,
    own_device: &DeviceId,
) -> Vec<CallMember> {
    read_call_members(room, Some((own_user, own_device))).await
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use matrix_sdk::ruma::{
        events::call::member::{
            ActiveFocus, ActiveLivekitFocus, Application, CallApplicationContent,
            CallMemberEventContent, CallScope, Focus, LivekitFocus,
        },
        owned_device_id, user_id, UInt,
    };

    use super::*;

    /// A minute ago plus `ms`. The base is read once, so equal arguments give
    /// equal timestamps even when the clock ticks between calls (ties must
    /// stay ties for the state-key tie-break tests).
    fn ts(ms: u32) -> MilliSecondsSinceUnixEpoch {
        static BASE: std::sync::OnceLock<UInt> = std::sync::OnceLock::new();
        let base =
            *BASE.get_or_init(|| MilliSecondsSinceUnixEpoch::now().0 - UInt::from(60_000u32));
        MilliSecondsSinceUnixEpoch(base + UInt::from(ms))
    }

    /// Build a member event as the wire would carry it; `selection` is the
    /// `focus_active.focus_selection` string.
    fn content(
        device: &str,
        selection: &str,
        url: &str,
        created: Option<MilliSecondsSinceUnixEpoch>,
    ) -> CallMemberEventContent {
        let mut c = CallMemberEventContent::new(
            Application::Call(CallApplicationContent::new(String::new(), CallScope::Room)),
            device.into(),
            ActiveFocus::Livekit(ActiveLivekitFocus::new()),
            vec![Focus::Livekit(LivekitFocus::new(
                "!room:a.org".to_owned(),
                url.to_owned(),
            ))],
            created,
            Some(Duration::from_secs(3600)),
        );
        let mut json = serde_json::to_value(&c).unwrap();
        json["focus_active"]["focus_selection"] = selection.into();
        c = serde_json::from_value(json).unwrap();
        c
    }

    fn members(
        c: &CallMemberEventContent,
        key: &str,
        sender: &str,
        origin: MilliSecondsSinceUnixEpoch,
    ) -> Vec<CallMember> {
        members_from_call_member(
            c,
            key,
            &UserId::parse(sender).unwrap(),
            origin,
            user_id!("@me:a.org"),
            &owned_device_id!("MYDEV"),
        )
    }

    fn one(c: CallMemberEventContent, key: &str, sender: &str, t: u32) -> Vec<CallMember> {
        members(&c, key, sender, ts(t))
    }

    #[test]
    fn multi_sfu_uses_own_first_focus() {
        let m = one(
            content("D1", "multi_sfu", "https://sfu.b.org", None),
            "k",
            "@b:b.org",
            1,
        );
        assert_eq!(m[0].focus_selection, "multi_sfu");
        assert_eq!(
            transport_of(&m[0], &m, None).unwrap().service_url,
            "https://sfu.b.org"
        );
    }

    #[test]
    fn oldest_membership_follows_oldest_member() {
        let mut all = one(
            content("D1", "oldest_membership", "https://sfu.b.org", None),
            "kb",
            "@b:b.org",
            100,
        );
        all.extend(one(
            content("D2", "oldest_membership", "https://sfu.c.org", None),
            "kc",
            "@c:c.org",
            50,
        ));
        assert_eq!(
            transport_of(&all[0], &all, None).unwrap().service_url,
            "https://sfu.c.org"
        );
        assert_eq!(
            transport_of(&all[1], &all, None).unwrap().service_url,
            "https://sfu.c.org"
        );
    }

    #[test]
    fn oldest_membership_counts_our_own_membership() {
        let all = one(content("D1", "oldest_membership", "https://sfu.b.org", None), "kb", "@b:b.org", 100);
        let own = LkFocus { service_url: "https://sfu.a.org".into(), alias: "!r".into(), sfu_url: None, server_name: None };
        // we joined before b, so b (legacy) publishes on our SFU
        assert_eq!(
            transport_of(&all[0], &all, Some((ts(10), &own)))
                .unwrap()
                .service_url,
            "https://sfu.a.org"
        );
    }

    #[test]
    fn created_ts_beats_origin_server_ts() {
        let mut all = one(
            content("D1", "oldest_membership", "https://sfu.b.org", Some(ts(10))),
            "kb",
            "@b:b.org",
            900,
        );
        all.extend(one(
            content("D2", "oldest_membership", "https://sfu.c.org", None),
            "kc",
            "@c:c.org",
            50,
        ));
        assert_eq!(
            transport_of(&all[1], &all, None).unwrap().service_url,
            "https://sfu.b.org"
        );
    }

    #[test]
    fn oldest_tie_broken_by_state_key() {
        let mut all = one(
            content("D1", "oldest_membership", "https://sfu.b.org", None),
            "kb",
            "@b:b.org",
            5,
        );
        all.extend(one(
            content("D2", "oldest_membership", "https://sfu.c.org", None),
            "ka",
            "@c:c.org",
            5,
        ));
        assert_eq!(
            transport_of(&all[0], &all, None).unwrap().service_url,
            "https://sfu.c.org"
        );
    }

    #[test]
    fn unknown_selection_has_no_transport() {
        let m = one(
            content("D1", "something_new", "https://sfu.b.org", None),
            "k",
            "@b:b.org",
            1,
        );
        assert!(transport_of(&m[0], &m, None).is_none());
    }

    #[test]
    fn own_device_excluded_other_device_not() {
        assert!(one(
            content("MYDEV", "multi_sfu", "https://sfu.a.org", None),
            "k",
            "@me:a.org",
            1
        )
        .is_empty());
        assert_eq!(
            one(
                content("OTHER", "multi_sfu", "https://sfu.a.org", None),
                "k",
                "@me:a.org",
                1
            )
            .len(),
            1
        );
    }

    #[test]
    fn leave_and_expired_are_ignored() {
        assert!(one(CallMemberEventContent::new_empty(None), "k", "@b:b.org", 1).is_empty());
        let long_ago = MilliSecondsSinceUnixEpoch(UInt::from(1_000u32));
        assert!(one(
            content("D1", "multi_sfu", "https://sfu.b.org", Some(long_ago)),
            "k",
            "@b:b.org",
            1
        )
        .is_empty());
    }

    #[test]
    fn member_without_service_url_has_no_transport() {
        let m = one(content("D1", "multi_sfu", "", None), "k", "@b:b.org", 1);
        assert!(m[0].foci_preferred.is_empty());
        assert!(transport_of(&m[0], &m, None).is_none());
    }

    #[test]
    fn desired_sfus_excludes_local_and_dedups() {
        let mut all = one(
            content("D1", "multi_sfu", "https://sfu.b.org", None),
            "k1",
            "@b:b.org",
            1,
        );
        all.extend(one(
            content("D2", "multi_sfu", "https://sfu.b.org", None),
            "k2",
            "@c:b.org",
            2,
        ));
        all.extend(one(
            content("D3", "multi_sfu", "https://sfu.a.org", None),
            "k3",
            "@d:a.org",
            3,
        ));
        let want = desired_sfus(&all, None, "https://sfu.a.org");
        assert_eq!(
            want.into_iter().collect::<Vec<_>>(),
            vec!["https://sfu.b.org".to_owned()]
        );
    }

    #[test]
    fn transport_map_lists_each_members_sfu() {
        let mut all = one(
            content("D1", "multi_sfu", "https://sfu.b.org", None),
            "k1",
            "@b:b.org",
            1,
        );
        all.extend(one(
            content("D2", "something_new", "https://sfu.c.org", None),
            "k2",
            "@c:c.org",
            2,
        ));
        let map = transport_map(&all, None);
        assert_eq!(
            map.get(&("@b:b.org".to_owned(), "D1".to_owned())).unwrap(),
            "https://sfu.b.org"
        );
        assert!(!map.contains_key(&("@c:c.org".to_owned(), "D2".to_owned())));
    }

    /// The "call in progress" participants of one event, through the same
    /// raw-JSON path production uses.
    fn participants(c: &CallMemberEventContent, sender: &str, t: u32) -> Vec<Participant> {
        let members = members_from_event_json(
            &serde_json::to_value(c).unwrap(),
            "_k_m.call",
            &UserId::parse(sender).unwrap(),
            ts(t),
            Some((user_id!("@me:a.org"), &owned_device_id!("MYDEV"))),
            u64::from(MilliSecondsSinceUnixEpoch::now().0),
        );
        crate::client::rtc::slots::participants_from_members(&members)
    }

    #[test]
    fn participants_exclude_this_device_leave_and_expired() {
        assert!(participants(
            &content("MYDEV", "multi_sfu", "https://s", None),
            "@me:a.org",
            1
        )
        .is_empty());
        assert_eq!(
            participants(
                &content("OTHER", "multi_sfu", "https://s", None),
                "@me:a.org",
                1
            )
            .len(),
            1
        );
        assert!(participants(&CallMemberEventContent::new_empty(None), "@b:b.org", 1).is_empty());
        let long_ago = MilliSecondsSinceUnixEpoch(UInt::from(1_000u32));
        assert!(participants(
            &content("D", "multi_sfu", "https://s", Some(long_ago)),
            "@b:b.org",
            1
        )
        .is_empty());
    }

    #[test]
    fn summary_orders_by_join_time_and_dedups_users() {
        let mut all = participants(
            &content("D1", "multi_sfu", "https://s", None),
            "@late:b.org",
            50,
        );
        all.extend(participants(
            &content("D2", "multi_sfu", "https://s", None),
            "@early:b.org",
            10,
        ));
        all.extend(participants(
            &content("D3", "multi_sfu", "https://s", None),
            "@late:b.org",
            60,
        ));
        let (users, intent) = summarize_participants(all);
        assert_eq!(
            users,
            vec!["@early:b.org".to_owned(), "@late:b.org".to_owned()]
        );
        assert_eq!(intent, "");
    }

    #[test]
    fn summary_intent_prefers_video() {
        let p = |user: &str, intent| Participant {
            user_id: user.to_owned(),
            joined_at: ts(1),
            intent,
        };
        assert_eq!(
            summarize_participants(vec![p("@a:x", Some("audio"))]).1,
            "audio"
        );
        assert_eq!(
            summarize_participants(vec![p("@a:x", Some("audio")), p("@b:x", Some("video"))]).1,
            "video"
        );
        assert_eq!(summarize_participants(vec![p("@a:x", None)]).1, "");
    }

    #[test]
    fn created_ts_survives_serialization() {
        let created = ts(7);
        let json = serde_json::to_value(content(
            "D1",
            "multi_sfu",
            "https://sfu.b.org",
            Some(created),
        ))
        .unwrap();
        assert_eq!(json["created_ts"], serde_json::to_value(created).unwrap());
        assert_eq!(json["focus_active"]["focus_selection"], "multi_sfu");
    }

    // ── members_from_event_json: events ruma's typed model cannot read ─────────

    /// ruma checks expiry against the real clock, so the tests do too.
    fn now_ms() -> u64 {
        u64::from(MilliSecondsSinceUnixEpoch::now().0)
    }

    fn at_ms(ms: u64) -> MilliSecondsSinceUnixEpoch {
        MilliSecondsSinceUnixEpoch(UInt::new(ms).unwrap())
    }

    /// The member event Element's phone app wrote: no `livekit_alias`.
    fn phone_content() -> serde_json::Value {
        serde_json::json!({
            "application": "m.call", "call_id": "",
            "device_id": "PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw",
            "expires": 14_400_000,
            "foci_preferred": [{"livekit_service_url": "https://jwt.example.org/livekit/jwt", "type": "livekit"}],
            "focus_active": {"focus_selection": "multi_sfu", "type": "livekit"},
            "m.call.intent": "video",
            "membershipID": "@surak:example.org:PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw",
            "scope": "m.room"
        })
    }

    fn json_members(content: serde_json::Value, origin: u64, exclude: Option<(&UserId, &DeviceId)>) -> Vec<CallMember> {
        members_from_event_json(
            &content,
            "_@surak:example.org_PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw_m.call",
            user_id!("@surak:example.org"),
            at_ms(origin),
            exclude,
            now_ms(),
        )
    }

    #[test]
    fn a_member_event_without_livekit_alias_still_counts() {
        let got = json_members(phone_content(), now_ms() - 60_000, None);
        assert_eq!(got.len(), 1);
        let m = &got[0];
        assert_eq!(m.user_id, "@surak:example.org");
        assert_eq!(m.device_id, "PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw");
        assert_eq!(m.focus_selection, "multi_sfu");
        assert_eq!(m.foci_preferred[0].service_url, "https://jwt.example.org/livekit/jwt");
        assert_eq!(m.intent.as_deref(), Some("video"));
        assert_eq!(m.source, MemberSource::Legacy);
    }

    #[test]
    fn an_event_with_a_livekit_alias_still_counts_once() {
        let mut c = phone_content();
        c["foci_preferred"][0]["livekit_alias"] = serde_json::json!("!r:example.org");
        assert_eq!(json_members(c, now_ms() - 60_000, None).len(), 1);
    }

    #[test]
    fn an_expired_membership_is_ignored() {
        // Written five hours ago with a four hour lifetime.
        assert!(json_members(phone_content(), now_ms() - 5 * 3_600_000, None).is_empty());
        // `created_ts` wins over the event time.
        let mut c = phone_content();
        c["created_ts"] = serde_json::json!(now_ms() - 3_600_000);
        assert_eq!(json_members(c, now_ms() - 5 * 3_600_000, None).len(), 1);
    }

    #[test]
    fn empty_other_application_and_deviceless_content_is_ignored() {
        assert!(json_members(serde_json::json!({}), now_ms(), None).is_empty(), "a leave");
        let mut other = phone_content();
        other["application"] = serde_json::json!("m.whiteboard");
        assert!(json_members(other, now_ms(), None).is_empty());
        let mut no_device = phone_content();
        no_device.as_object_mut().unwrap().remove("device_id");
        assert!(json_members(no_device, now_ms(), None).is_empty());
    }

    #[test]
    fn our_own_device_is_excluded_when_asked() {
        let me = user_id!("@surak:example.org");
        let dev = owned_device_id!("PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw");
        assert!(json_members(phone_content(), now_ms(), Some((me, &dev))).is_empty());
        let other = owned_device_id!("SOMEOTHER");
        assert_eq!(json_members(phone_content(), now_ms(), Some((me, &other))).len(), 1);
    }

    #[test]
    fn an_event_without_a_service_url_is_a_member_without_a_transport() {
        let mut c = phone_content();
        c["foci_preferred"] = serde_json::json!([]);
        let got = json_members(c, now_ms(), None);
        assert_eq!(got.len(), 1);
        assert!(got[0].foci_preferred.is_empty());
    }

    #[test]
    fn call_id_is_read_from_either_membership_form() {
        assert_eq!(call_id_of(&serde_json::json!({"call_id": "abc", "application": "m.call"})), "abc");
        assert_eq!(
            call_id_of(&serde_json::json!({"memberships": [{"application": "m.call", "call_id": "xyz"}]})),
            "xyz"
        );
        assert_eq!(call_id_of(&serde_json::json!({"memberships": [{"application": "m.call"}]})), "");
        assert_eq!(call_id_of(&serde_json::json!({})), "");
    }
}
