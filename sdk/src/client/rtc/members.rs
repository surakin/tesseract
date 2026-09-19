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
    DeviceId, MilliSecondsSinceUnixEpoch, UserId,
};

pub const SELECTION_MULTI_SFU: &str = "multi_sfu";
pub const SELECTION_OLDEST: &str = "oldest_membership";

/// A LiveKit focus: the JWT service URL and the room alias.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LkFocus {
    pub service_url: String,
    pub alias: String,
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
pub fn members_from_call_member(
    content: &CallMemberEventContent,
    state_key: &str,
    sender: &UserId,
    origin_server_ts: MilliSecondsSinceUnixEpoch,
    own_user: &UserId,
    own_device: &DeviceId,
) -> Vec<CallMember> {
    content
        .active_memberships(Some(origin_server_ts))
        .into_iter()
        .filter(|m| m.is_call())
        .filter(|m| !(sender == own_user && m.device_id() == own_device))
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

/// The live memberships of one call-member event, excluding this device.
pub fn call_participants(
    content: &CallMemberEventContent,
    sender: &UserId,
    origin_server_ts: MilliSecondsSinceUnixEpoch,
    own_user: &UserId,
    own_device: &DeviceId,
) -> Vec<Participant> {
    use matrix_sdk::ruma::events::rtc::notification::CallIntent;

    content
        .active_memberships(Some(origin_server_ts))
        .into_iter()
        .filter(|m| m.is_call())
        .filter(|m| !(sender == own_user && m.device_id() == own_device))
        .map(|m| Participant {
            user_id: sender.to_string(),
            joined_at: m.created_ts().unwrap_or(origin_server_ts),
            intent: match m.call_intent() {
                Some(CallIntent::Video) => Some("video"),
                Some(CallIntent::Audio) => Some("audio"),
                _ => None,
            },
        })
        .collect()
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

/// Read every live remote call member of `room` from the local state store.
pub async fn read_live_members(
    room: &matrix_sdk::Room,
    own_user: &UserId,
    own_device: &DeviceId,
) -> Vec<CallMember> {
    use matrix_sdk::deserialized_responses::SyncOrStrippedState;
    use matrix_sdk::ruma::events::SyncStateEvent;

    let mut members = Vec::new();
    for raw in room
        .get_state_events_static::<CallMemberEventContent>()
        .await
        .unwrap_or_default()
    {
        if let Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(o))) = raw.deserialize() {
            members.extend(members_from_call_member(
                &o.content,
                o.state_key.as_ref(),
                &o.sender,
                o.origin_server_ts,
                own_user,
                own_device,
            ));
        }
    }
    members
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

    fn ts(ms: u32) -> MilliSecondsSinceUnixEpoch {
        let base = MilliSecondsSinceUnixEpoch::now().0 - UInt::from(60_000u32);
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
        let m = one(content("D1", "multi_sfu", "https://sfu.b.org", None), "k", "@b:b.org", 1);
        assert_eq!(m[0].focus_selection, "multi_sfu");
        assert_eq!(
            transport_of(&m[0], &m, None).unwrap().service_url,
            "https://sfu.b.org"
        );
    }

    #[test]
    fn oldest_membership_follows_oldest_member() {
        let mut all = one(content("D1", "oldest_membership", "https://sfu.b.org", None), "kb", "@b:b.org", 100);
        all.extend(one(content("D2", "oldest_membership", "https://sfu.c.org", None), "kc", "@c:c.org", 50));
        assert_eq!(transport_of(&all[0], &all, None).unwrap().service_url, "https://sfu.c.org");
        assert_eq!(transport_of(&all[1], &all, None).unwrap().service_url, "https://sfu.c.org");
    }

    #[test]
    fn oldest_membership_counts_our_own_membership() {
        let all = one(content("D1", "oldest_membership", "https://sfu.b.org", None), "kb", "@b:b.org", 100);
        let own = LkFocus { service_url: "https://sfu.a.org".into(), alias: "!r".into() };
        // we joined before b, so b (legacy) publishes on our SFU
        assert_eq!(
            transport_of(&all[0], &all, Some((ts(10), &own))).unwrap().service_url,
            "https://sfu.a.org"
        );
    }

    #[test]
    fn created_ts_beats_origin_server_ts() {
        let mut all = one(content("D1", "oldest_membership", "https://sfu.b.org", Some(ts(10))), "kb", "@b:b.org", 900);
        all.extend(one(content("D2", "oldest_membership", "https://sfu.c.org", None), "kc", "@c:c.org", 50));
        assert_eq!(transport_of(&all[1], &all, None).unwrap().service_url, "https://sfu.b.org");
    }

    #[test]
    fn oldest_tie_broken_by_state_key() {
        let mut all = one(content("D1", "oldest_membership", "https://sfu.b.org", None), "kb", "@b:b.org", 5);
        all.extend(one(content("D2", "oldest_membership", "https://sfu.c.org", None), "ka", "@c:c.org", 5));
        assert_eq!(transport_of(&all[0], &all, None).unwrap().service_url, "https://sfu.c.org");
    }

    #[test]
    fn unknown_selection_has_no_transport() {
        let m = one(content("D1", "something_new", "https://sfu.b.org", None), "k", "@b:b.org", 1);
        assert!(transport_of(&m[0], &m, None).is_none());
    }

    #[test]
    fn own_device_excluded_other_device_not() {
        assert!(one(content("MYDEV", "multi_sfu", "https://sfu.a.org", None), "k", "@me:a.org", 1).is_empty());
        assert_eq!(one(content("OTHER", "multi_sfu", "https://sfu.a.org", None), "k", "@me:a.org", 1).len(), 1);
    }

    #[test]
    fn leave_and_expired_are_ignored() {
        assert!(one(CallMemberEventContent::new_empty(None), "k", "@b:b.org", 1).is_empty());
        let long_ago = MilliSecondsSinceUnixEpoch(UInt::from(1_000u32));
        assert!(one(content("D1", "multi_sfu", "https://sfu.b.org", Some(long_ago)), "k", "@b:b.org", 1).is_empty());
    }

    #[test]
    fn member_without_service_url_has_no_transport() {
        let m = one(content("D1", "multi_sfu", "", None), "k", "@b:b.org", 1);
        assert!(m[0].foci_preferred.is_empty());
        assert!(transport_of(&m[0], &m, None).is_none());
    }

    #[test]
    fn desired_sfus_excludes_local_and_dedups() {
        let mut all = one(content("D1", "multi_sfu", "https://sfu.b.org", None), "k1", "@b:b.org", 1);
        all.extend(one(content("D2", "multi_sfu", "https://sfu.b.org", None), "k2", "@c:b.org", 2));
        all.extend(one(content("D3", "multi_sfu", "https://sfu.a.org", None), "k3", "@d:a.org", 3));
        let want = desired_sfus(&all, None, "https://sfu.a.org");
        assert_eq!(want.into_iter().collect::<Vec<_>>(), vec!["https://sfu.b.org".to_owned()]);
    }

    #[test]
    fn transport_map_lists_each_members_sfu() {
        let mut all = one(content("D1", "multi_sfu", "https://sfu.b.org", None), "k1", "@b:b.org", 1);
        all.extend(one(content("D2", "something_new", "https://sfu.c.org", None), "k2", "@c:c.org", 2));
        let map = transport_map(&all, None);
        assert_eq!(map.get(&("@b:b.org".to_owned(), "D1".to_owned())).unwrap(), "https://sfu.b.org");
        assert!(!map.contains_key(&("@c:c.org".to_owned(), "D2".to_owned())));
    }

    fn participants(c: &CallMemberEventContent, sender: &str, t: u32) -> Vec<Participant> {
        call_participants(
            c,
            &UserId::parse(sender).unwrap(),
            ts(t),
            user_id!("@me:a.org"),
            &owned_device_id!("MYDEV"),
        )
    }

    #[test]
    fn participants_exclude_this_device_leave_and_expired() {
        assert!(participants(&content("MYDEV", "multi_sfu", "https://s", None), "@me:a.org", 1).is_empty());
        assert_eq!(participants(&content("OTHER", "multi_sfu", "https://s", None), "@me:a.org", 1).len(), 1);
        assert!(participants(&CallMemberEventContent::new_empty(None), "@b:b.org", 1).is_empty());
        let long_ago = MilliSecondsSinceUnixEpoch(UInt::from(1_000u32));
        assert!(participants(&content("D", "multi_sfu", "https://s", Some(long_ago)), "@b:b.org", 1).is_empty());
    }

    #[test]
    fn summary_orders_by_join_time_and_dedups_users() {
        let mut all = participants(&content("D1", "multi_sfu", "https://s", None), "@late:b.org", 50);
        all.extend(participants(&content("D2", "multi_sfu", "https://s", None), "@early:b.org", 10));
        all.extend(participants(&content("D3", "multi_sfu", "https://s", None), "@late:b.org", 60));
        let (users, intent) = summarize_participants(all);
        assert_eq!(users, vec!["@early:b.org".to_owned(), "@late:b.org".to_owned()]);
        assert_eq!(intent, "");
    }

    #[test]
    fn summary_intent_prefers_video() {
        let p = |user: &str, intent| Participant {
            user_id: user.to_owned(),
            joined_at: ts(1),
            intent,
        };
        assert_eq!(summarize_participants(vec![p("@a:x", Some("audio"))]).1, "audio");
        assert_eq!(summarize_participants(vec![p("@a:x", Some("audio")), p("@b:x", Some("video"))]).1, "video");
        assert_eq!(summarize_participants(vec![p("@a:x", None)]).1, "");
    }

    #[test]
    fn created_ts_survives_serialization() {
        let created = ts(7);
        let json = serde_json::to_value(content("D1", "multi_sfu", "https://sfu.b.org", Some(created))).unwrap();
        assert_eq!(json["created_ts"], serde_json::to_value(created).unwrap());
        assert_eq!(json["focus_active"]["focus_selection"], "multi_sfu");
    }
}
