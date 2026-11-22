//! Registry of state event types shown (or hidden) in the timeline, and the
//! visibility rule that gates their rows, and the per-type builders of the
//! `m.state` rows themselves.

use std::collections::{BTreeMap, BTreeSet};

use matrix_sdk::ruma::events::room::guest_access::GuestAccess;
use matrix_sdk::ruma::events::room::history_visibility::HistoryVisibility;
use matrix_sdk::ruma::room::JoinRule;

use crate::text_utils::strip_bidi_overrides;

#[cfg(not(test))]
use super::timeline_convert::{ffi_event_defaults, sender_profile_ffi, SenderProfileFfi};
#[cfg(not(test))]
use crate::ffi::TimelineEvent;
#[cfg(not(test))]
use matrix_sdk_ui::timeline::{EventTimelineItem, TimelineItemContent};

/// How a state event type reaches the timeline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum StateTier {
    /// Always shown (name, topic, pins, tombstone).
    Always,
    /// Shown under "Show state events in the timeline".
    StateSetting,
    /// Never a timeline row, not even under "Show unhandled events".
    Hidden,
}

/// Every state event type Tesseract knows, with its tier. One entry per line:
/// the coverage script parses this text.
pub(crate) const STATE_EVENT_KINDS: &[(&str, StateTier)] = &[
    ("m.room.name", StateTier::Always),
    ("m.room.topic", StateTier::Always),
    ("m.room.pinned_events", StateTier::Always),
    ("m.room.tombstone", StateTier::Always),
    ("m.room.join_rules", StateTier::StateSetting),
    ("m.room.history_visibility", StateTier::StateSetting),
    ("m.room.guest_access", StateTier::StateSetting),
    ("m.room.encryption", StateTier::StateSetting),
    ("m.room.avatar", StateTier::StateSetting),
    ("m.room.canonical_alias", StateTier::StateSetting),
    ("m.room.create", StateTier::StateSetting),
    ("m.room.power_levels", StateTier::StateSetting),
    ("m.room.third_party_invite", StateTier::StateSetting),
    ("m.room.server_acl", StateTier::StateSetting),
    ("m.space.child", StateTier::StateSetting),
    ("m.space.parent", StateTier::StateSetting),
    ("m.policy.rule.room", StateTier::StateSetting),
    ("m.policy.rule.server", StateTier::StateSetting),
    ("m.policy.rule.user", StateTier::StateSetting),
    ("m.bridge", StateTier::StateSetting),
    ("uk.half-shot.bridge", StateTier::StateSetting),
    ("io.element.functional_members", StateTier::StateSetting),
    ("com.beeper.room_features", StateTier::StateSetting),
    ("im.ponies.room_emotes", StateTier::StateSetting),
    ("org.matrix.msc3401.call.member", StateTier::Hidden),
    ("m.call.member", StateTier::Hidden),
    ("org.matrix.msc4143.rtc.member", StateTier::Hidden),
    ("m.rtc.member", StateTier::Hidden),
    ("org.matrix.msc4143.rtc.slot", StateTier::Hidden),
    ("m.rtc.slot", StateTier::Hidden),
];

/// Tier of a state event type; `None` for a type the registry does not know.
pub(crate) fn state_tier(event_type: &str) -> Option<StateTier> {
    STATE_EVENT_KINDS
        .iter()
        .find(|(t, _)| *t == event_type)
        .map(|(_, tier)| *tier)
}

/// Whether a state event of this type is kept out of the timeline entirely.
pub(crate) fn state_event_hidden(event_type: &str) -> bool {
    state_tier(event_type) == Some(StateTier::Hidden)
}

/// Whether a row reaches the UI given the state-event and unhandled-event
/// settings. `state_type` is the row's `state_type`; for `dev.unhandled` rows
/// (which carry no `state_type`) pass the body, "type" or "type [state_key]".
/// `m.state` rows need `show_state` unless their type is always-on (only
/// `m.state` rows are exempt); `dev.unhandled` rows of a known state type need
/// `show_state`, any other `dev.unhandled` row needs `show_unhandled`.
pub(crate) fn state_event_visible(
    msg_type: &str,
    state_type: &str,
    show_state: bool,
    show_unhandled: bool,
) -> bool {
    match msg_type {
        "m.state" => state_tier(state_type) == Some(StateTier::Always) || show_state,
        "dev.unhandled" => {
            let event_type = state_type.split(" [").next().unwrap_or(state_type);
            if dev_unhandled_passthrough(event_type) {
                show_state
            } else {
                show_unhandled
            }
        }
        _ => true,
    }
}

/// The string `state_event_visible` keys on for a row: the body of a
/// `dev.unhandled` row (it carries no `state_type`), the `state_type` of an
/// `m.state` row, nothing for any other row.
pub(crate) fn visibility_key<'a>(msg_type: &str, body: &'a str, state_type: &'a str) -> &'a str {
    match msg_type {
        "dev.unhandled" => body,
        "m.state" => state_type,
        _ => "",
    }
}

/// State-setting types `state_event_row` does not turn into `m.state` rows;
/// they still surface as raw `dev.unhandled` rows under "Show state events".
pub(crate) fn dev_unhandled_passthrough(event_type: &str) -> bool {
    matches!(
        event_type,
        "m.bridge"
            | "uk.half-shot.bridge"
            | "io.element.functional_members"
            | "com.beeper.room_features"
            | "im.ponies.room_emotes"
    )
}

type Params = (&'static str, Vec<String>);

fn set_or_changed(old_present: bool) -> &'static str {
    if old_present {
        "changed"
    } else {
        "set"
    }
}

pub(crate) fn join_rules_params(new: &JoinRule, old: Option<&JoinRule>) -> Params {
    (
        set_or_changed(old.is_some()),
        vec![
            new.as_str().to_owned(),
            old.map(|o| o.as_str().to_owned()).unwrap_or_default(),
        ],
    )
}

pub(crate) fn history_visibility_params(
    new: &HistoryVisibility,
    old: Option<&HistoryVisibility>,
) -> Params {
    (
        set_or_changed(old.is_some()),
        vec![
            new.as_str().to_owned(),
            old.map(|o| o.as_str().to_owned()).unwrap_or_default(),
        ],
    )
}

pub(crate) fn guest_access_params(new: &GuestAccess, old: Option<&GuestAccess>) -> Params {
    (
        set_or_changed(old.is_some()),
        vec![
            new.as_str().to_owned(),
            old.map(|o| o.as_str().to_owned()).unwrap_or_default(),
        ],
    )
}

/// `prev_has_avatar` is `None` when there was no previous content.
pub(crate) fn avatar_change(has_avatar: bool, prev_has_avatar: Option<bool>) -> &'static str {
    if !has_avatar {
        "removed"
    } else if prev_has_avatar == Some(true) {
        "changed"
    } else {
        "set"
    }
}

pub(crate) fn canonical_alias_params(new: Option<&str>, old: Option<&str>) -> Params {
    let change = match (new, old) {
        (None, _) => "removed",
        (Some(_), Some(_)) => "changed",
        (Some(_), None) => "set",
    };
    (
        change,
        vec![
            strip_bidi_overrides(new.unwrap_or_default()),
            strip_bidi_overrides(old.unwrap_or_default()),
        ],
    )
}

pub(crate) fn space_link_change(has_via: bool) -> &'static str {
    if has_via {
        "added"
    } else {
        "removed"
    }
}

/// An empty rule (no entity) is a removal.
pub(crate) fn policy_rule_params(entity: &str, recommendation: &str, reason: &str) -> Params {
    let change = if entity.is_empty() { "removed" } else { "set" };
    (
        change,
        vec![
            strip_bidi_overrides(entity),
            recommendation.to_owned(),
            strip_bidi_overrides(reason),
        ],
    )
}

/// The `m.state` row parts for a state event the SDK could not parse that is
/// really a removal: `m.space.child`, `m.space.parent` and `m.policy.rule.*`
/// are removed by sending `{}` content, which fails to deserialize. `None` for
/// every other type (those keep their existing handling).
pub(crate) struct FailedStateRemoval {
    pub(crate) state_type: &'static str,
    pub(crate) state_key: String,
    pub(crate) change: &'static str,
    pub(crate) params: Vec<String>,
}

pub(crate) fn failed_state_removal(
    event_type: &str,
    state_key: &str,
) -> Option<FailedStateRemoval> {
    let removable = matches!(
        event_type,
        "m.space.child"
            | "m.space.parent"
            | "m.policy.rule.room"
            | "m.policy.rule.server"
            | "m.policy.rule.user"
    );
    if !removable {
        return None;
    }
    let (state_type, _) = STATE_EVENT_KINDS.iter().find(|(t, _)| *t == event_type)?;
    Some(FailedStateRemoval {
        state_type,
        state_key: state_key.to_owned(),
        change: "removed",
        params: Vec::new(),
    })
}

/// One param per line of `server_acl_changes` output.
pub(crate) fn server_acl_params(changes: &str) -> Vec<String> {
    changes.lines().map(str::to_owned).collect()
}

/// Maximum number of `m.room.power_levels` difference params before the
/// remainder collapses into a `"more|||N"` entry.
const POWER_LEVEL_PARAM_CAP: usize = 50;

/// The power levels of one `m.room.power_levels` content, flattened for diffing.
/// `fields` is in the fixed order of `POWER_LEVEL_FIELDS`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PowerLevelsSnapshot {
    pub(crate) fields: [(&'static str, i64); 7],
    pub(crate) users: BTreeMap<String, i64>,
    pub(crate) events: BTreeMap<String, i64>,
}

impl PowerLevelsSnapshot {
    /// The levels the spec assumes when a room has no previous power levels.
    pub(crate) fn spec_default() -> Self {
        Self {
            fields: [
                ("users_default", 0),
                ("events_default", 0),
                ("state_default", 50),
                ("ban", 50),
                ("kick", 50),
                ("redact", 50),
                ("invite", 0),
            ],
            users: BTreeMap::new(),
            events: BTreeMap::new(),
        }
    }
}

/// One `"scope|key|old|new"` param per difference (the new levels compared with
/// the previous ones, or the spec defaults when there are none), capped.
pub(crate) fn power_levels_params(
    new: &PowerLevelsSnapshot,
    old: Option<&PowerLevelsSnapshot>,
) -> Vec<String> {
    let default = PowerLevelsSnapshot::spec_default();
    let old = old.unwrap_or(&default);
    let mut out: Vec<String> = Vec::new();
    for (n, o) in new.fields.iter().zip(old.fields.iter()) {
        if n.1 != o.1 {
            out.push(format!("field|{}|{}|{}", n.0, o.1, n.1));
        }
    }
    let users_default = |s: &PowerLevelsSnapshot| s.fields[0].1;
    let keys: BTreeSet<&String> = new.users.keys().chain(old.users.keys()).collect();
    for k in keys {
        let n = new.users.get(k).copied().unwrap_or(users_default(new));
        let o = old.users.get(k).copied().unwrap_or(users_default(old));
        if n != o {
            out.push(format!("user|{k}|{o}|{n}"));
        }
    }
    let keys: BTreeSet<&String> = new.events.keys().chain(old.events.keys()).collect();
    for k in keys {
        let n = new.events.get(k).copied();
        let o = old.events.get(k).copied();
        if n != o {
            let show = |v: Option<i64>| v.map(|v| v.to_string()).unwrap_or_default();
            out.push(format!("event|{k}|{}|{}", show(o), show(n)));
        }
    }
    if out.len() > POWER_LEVEL_PARAM_CAP {
        let more = out.len() - POWER_LEVEL_PARAM_CAP;
        out.truncate(POWER_LEVEL_PARAM_CAP);
        out.push(format!("more|||{more}"));
    }
    out
}

/// Flatten any power-levels content (original or possibly redacted: same
/// field names) into a `PowerLevelsSnapshot`.
#[cfg(not(test))]
macro_rules! power_levels_snapshot {
    ($c:expr) => {{
        let c = &$c;
        PowerLevelsSnapshot {
            fields: [
                ("users_default", i64::from(c.users_default)),
                ("events_default", i64::from(c.events_default)),
                ("state_default", i64::from(c.state_default)),
                ("ban", i64::from(c.ban)),
                ("kick", i64::from(c.kick)),
                ("redact", i64::from(c.redact)),
                ("invite", i64::from(c.invite)),
            ],
            users: c
                .users
                .iter()
                .map(|(k, v)| (k.to_string(), i64::from(*v)))
                .collect(),
            events: c
                .events
                .iter()
                .map(|(k, v)| (k.to_string(), i64::from(*v)))
                .collect(),
        }
    }};
}

/// The `m.state` row for a state event that has no dedicated row type, or
/// `None` for redacted content, `_Custom` and types Tesseract does not model
/// (those stay on the `dev.unhandled` path). Name, topic, pins and tombstone
/// are handled by their own rows before this is reached.
#[cfg(not(test))]
pub(crate) fn state_event_row(item: &EventTimelineItem, room_id: &str) -> Option<TimelineEvent> {
    use matrix_sdk::ruma::events::StateEventContentChange::Original;
    use matrix_sdk_ui::timeline::AnyOtherStateEventContentChange as A;

    let TimelineItemContent::OtherState(state) = item.content() else {
        return None;
    };
    let (state_type, (change, params)): (&str, Params) = match state.content() {
        A::RoomJoinRules(Original {
            content,
            prev_content,
        }) => (
            "m.room.join_rules",
            join_rules_params(
                &content.join_rule,
                prev_content.as_ref().map(|p| &p.join_rule),
            ),
        ),
        A::RoomHistoryVisibility(Original {
            content,
            prev_content,
        }) => (
            "m.room.history_visibility",
            history_visibility_params(
                &content.history_visibility,
                prev_content.as_ref().map(|p| &p.history_visibility),
            ),
        ),
        A::RoomGuestAccess(Original {
            content,
            prev_content,
        }) => (
            "m.room.guest_access",
            guest_access_params(
                &content.guest_access,
                prev_content.as_ref().and_then(|p| p.guest_access.as_ref()),
            ),
        ),
        A::RoomEncryption(Original { content, .. }) => (
            "m.room.encryption",
            ("set", vec![content.algorithm.as_str().to_owned()]),
        ),
        A::RoomAvatar(Original {
            content,
            prev_content,
        }) => (
            "m.room.avatar",
            (
                avatar_change(
                    content.url.is_some(),
                    prev_content.as_ref().map(|p| p.url.is_some()),
                ),
                Vec::new(),
            ),
        ),
        A::RoomCanonicalAlias(Original {
            content,
            prev_content,
        }) => (
            "m.room.canonical_alias",
            canonical_alias_params(
                content.alias.as_ref().map(|a| a.as_str()),
                prev_content
                    .as_ref()
                    .and_then(|p| p.alias.as_ref())
                    .map(|a| a.as_str()),
            ),
        ),
        A::RoomCreate(Original { content, .. }) => (
            "m.room.create",
            (
                "set",
                vec![
                    content.room_version.to_string(),
                    content
                        .predecessor
                        .as_ref()
                        .map(|p| p.room_id.to_string())
                        .unwrap_or_default(),
                ],
            ),
        ),
        A::RoomPowerLevels(Original {
            content,
            prev_content,
        }) => {
            let new = power_levels_snapshot!(content);
            let old = prev_content.as_ref().map(|p| power_levels_snapshot!(p));
            (
                "m.room.power_levels",
                (
                    set_or_changed(old.is_some()),
                    power_levels_params(&new, old.as_ref()),
                ),
            )
        }
        A::RoomThirdPartyInvite(Original { content, .. }) => (
            "m.room.third_party_invite",
            ("set", vec![strip_bidi_overrides(&content.display_name)]),
        ),
        A::RoomServerAcl(Original {
            content,
            prev_content,
        }) => {
            // A missing previous ACL means the spec defaults: no entries,
            // IP literals allowed.
            let (old_allow, old_deny, old_ip) = prev_content
                .as_ref()
                .map(|pc| (pc.allow.clone(), pc.deny.clone(), pc.allow_ip_literals))
                .unwrap_or((Vec::new(), Vec::new(), true));
            let lines = super::timeline_convert::server_acl_changes(
                &content.allow,
                &content.deny,
                content.allow_ip_literals,
                &old_allow,
                &old_deny,
                old_ip,
            );
            ("m.room.server_acl", ("changed", server_acl_params(&lines)))
        }
        A::SpaceChild(Original { content, .. }) => (
            "m.space.child",
            (space_link_change(!content.via.is_empty()), Vec::new()),
        ),
        A::SpaceParent(Original { content, .. }) => (
            "m.space.parent",
            (space_link_change(!content.via.is_empty()), Vec::new()),
        ),
        A::PolicyRuleRoom(Original { content, .. }) => (
            "m.policy.rule.room",
            policy_rule_params(
                &content.0.entity,
                content.0.recommendation.as_str(),
                &content.0.reason,
            ),
        ),
        A::PolicyRuleServer(Original { content, .. }) => (
            "m.policy.rule.server",
            policy_rule_params(
                &content.0.entity,
                content.0.recommendation.as_str(),
                &content.0.reason,
            ),
        ),
        A::PolicyRuleUser(Original { content, .. }) => (
            "m.policy.rule.user",
            policy_rule_params(
                &content.0.entity,
                content.0.recommendation.as_str(),
                &content.0.reason,
            ),
        ),
        _ => return None,
    };
    Some(TimelineEvent {
        msg_type: "m.state".to_owned(),
        state_type: state_type.to_owned(),
        state_key: state.state_key().to_owned(),
        state_change: change.to_owned(),
        state_params: params,
        ..base_row(item, room_id)
    })
}

/// The `m.state` removal row for a `FailedToParseState` item of a removable
/// type, or `None` for anything else.
#[cfg(not(test))]
pub(crate) fn failed_state_row(item: &EventTimelineItem, room_id: &str) -> Option<TimelineEvent> {
    let TimelineItemContent::FailedToParseState {
        event_type,
        state_key,
        ..
    } = item.content()
    else {
        return None;
    };
    let r = failed_state_removal(&event_type.to_string(), state_key)?;
    Some(TimelineEvent {
        msg_type: "m.state".to_owned(),
        state_type: r.state_type.to_owned(),
        state_key: r.state_key,
        state_change: r.change.to_owned(),
        state_params: r.params,
        ..base_row(item, room_id)
    })
}

/// A row with the event id, sender, sender profile and timestamp filled in:
/// the shared start of every `m.state` row.
#[cfg(not(test))]
fn base_row(item: &EventTimelineItem, room_id: &str) -> TimelineEvent {
    let SenderProfileFfi {
        sender_name,
        sender_name_ambiguous,
        sender_avatar_url,
        sender_status_emoji,
        sender_status_text,
    } = sender_profile_ffi(item.sender_profile());
    TimelineEvent {
        room_id: room_id.to_owned(),
        event_id: item.event_id().map(|id| id.to_string()).unwrap_or_default(),
        sender: item.sender().to_string(),
        sender_name,
        sender_name_ambiguous,
        sender_avatar_url,
        sender_status_emoji,
        sender_status_text,
        timestamp: item.timestamp().get().into(),
        ..ffi_event_defaults()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tiers() {
        assert_eq!(
            state_tier("m.room.join_rules"),
            Some(StateTier::StateSetting)
        );
        assert_eq!(state_tier("m.room.topic"), Some(StateTier::Always));
        assert_eq!(state_tier("m.call.member"), Some(StateTier::Hidden));
        assert_eq!(state_tier("x.unknown"), None);
    }

    #[test]
    fn new_rows_follow_state_flag() {
        assert!(!state_event_visible(
            "m.state",
            "m.room.join_rules",
            false,
            true
        ));
        assert!(state_event_visible(
            "m.state",
            "m.room.join_rules",
            true,
            false
        ));
        assert!(state_event_visible("m.state", "m.room.topic", false, false));
    }

    #[test]
    fn call_membership_state_never_reaches_the_timeline() {
        for t in [
            "org.matrix.msc3401.call.member",
            "m.call.member",
            "org.matrix.msc4143.rtc.member",
            "m.rtc.member",
            "org.matrix.msc4143.rtc.slot",
            "m.rtc.slot",
        ] {
            assert!(state_event_hidden(t), "{t}");
        }
        assert!(!state_event_hidden("m.room.topic"));
    }

    #[test]
    fn acl_and_known_state_rows_follow_the_state_flag() {
        assert!(state_event_visible(
            "m.state",
            "m.room.server_acl",
            true,
            false
        ));
        assert!(!state_event_visible(
            "m.state",
            "m.room.server_acl",
            false,
            true
        ));
        assert!(state_event_visible(
            "dev.unhandled",
            "m.bridge [a/b]",
            true,
            false
        ));
        assert!(!state_event_visible(
            "dev.unhandled",
            "uk.half-shot.bridge",
            false,
            true
        ));
    }

    #[test]
    fn other_dev_rows_follow_the_unhandled_flag() {
        assert!(state_event_visible(
            "dev.unhandled",
            "com.example.ping",
            false,
            true
        ));
        assert!(!state_event_visible(
            "dev.unhandled",
            "com.example.ping",
            true,
            false
        ));
    }

    #[test]
    fn ordinary_rows_are_always_visible() {
        assert!(state_event_visible("m.text", "", false, false));
    }

    fn v(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| (*s).to_owned()).collect()
    }

    #[test]
    fn join_rules_set_and_changed() {
        assert_eq!(
            join_rules_params(&JoinRule::Invite, Some(&JoinRule::Public)),
            ("changed", v(&["invite", "public"]))
        );
        assert_eq!(
            join_rules_params(&JoinRule::Public, None),
            ("set", v(&["public", ""]))
        );
    }

    #[test]
    fn history_and_guest_params() {
        assert_eq!(
            history_visibility_params(&HistoryVisibility::Shared, Some(&HistoryVisibility::Joined)),
            ("changed", v(&["shared", "joined"]))
        );
        assert_eq!(
            guest_access_params(&GuestAccess::CanJoin, None),
            ("set", v(&["can_join", ""]))
        );
    }

    #[test]
    fn canonical_alias_params_cases() {
        assert_eq!(
            canonical_alias_params(None, Some("#a:s")),
            ("removed", v(&["", "#a:s"]))
        );
        assert_eq!(
            canonical_alias_params(Some("#a:s"), None),
            ("set", v(&["#a:s", ""]))
        );
        assert_eq!(
            canonical_alias_params(Some("#b:s"), Some("#a:s")),
            ("changed", v(&["#b:s", "#a:s"]))
        );
    }

    #[test]
    fn avatar_change_cases() {
        assert_eq!(avatar_change(true, None), "set");
        assert_eq!(avatar_change(true, Some(false)), "set");
        assert_eq!(avatar_change(true, Some(true)), "changed");
        assert_eq!(avatar_change(false, Some(true)), "removed");
    }

    fn pl(ban: i64, users: &[(&str, i64)]) -> PowerLevelsSnapshot {
        PowerLevelsSnapshot {
            fields: [
                ("users_default", 0),
                ("events_default", 0),
                ("state_default", 50),
                ("ban", ban),
                ("kick", 50),
                ("redact", 50),
                ("invite", 0),
            ],
            users: users.iter().map(|(u, l)| ((*u).to_owned(), *l)).collect(),
            events: Default::default(),
        }
    }

    #[test]
    fn power_levels_field_and_user_changes() {
        assert_eq!(
            power_levels_params(&pl(75, &[]), Some(&pl(50, &[]))),
            v(&["field|ban|50|75"])
        );
        assert_eq!(
            power_levels_params(&pl(50, &[("@a:s", 100)]), Some(&pl(50, &[]))),
            v(&["user|@a:s|0|100"])
        );
    }

    #[test]
    fn power_levels_event_added_has_empty_old() {
        let mut new = pl(50, &[]);
        new.events.insert("m.room.name".to_owned(), 100);
        assert_eq!(
            power_levels_params(&new, Some(&pl(50, &[]))),
            v(&["event|m.room.name||100"])
        );
    }

    #[test]
    fn power_levels_first_event_lists_only_non_defaults() {
        assert_eq!(
            power_levels_params(&pl(50, &[("@a:s", 100)]), None),
            v(&["user|@a:s|0|100"])
        );
    }

    #[test]
    fn power_levels_are_capped() {
        let names: Vec<String> = (0..60).map(|i| format!("@u{i:02}:s")).collect();
        let users: Vec<(&str, i64)> = names.iter().map(|n| (n.as_str(), 50)).collect();
        let out = power_levels_params(&pl(50, &users), Some(&pl(50, &[])));
        assert_eq!(out.len(), 51);
        assert_eq!(out[50], "more|||10");
        assert_eq!(out[0], "user|@u00:s|0|50");
    }

    #[test]
    fn server_acl_params_split_lines() {
        assert_eq!(server_acl_params("deny+3\nip-"), v(&["deny+3", "ip-"]));
        assert!(server_acl_params("").is_empty());
    }

    #[test]
    fn space_child_and_policy_changes() {
        assert_eq!(space_link_change(true), "added");
        assert_eq!(space_link_change(false), "removed");
        assert_eq!(
            policy_rule_params("@bad:s", "m.ban", "spam"),
            ("set", v(&["@bad:s", "m.ban", "spam"]))
        );
        assert_eq!(policy_rule_params("", "", "").0, "removed");
    }

    #[test]
    fn visibility_key_picks_body_only_for_dev_unhandled() {
        // A dev.unhandled row has no state_type; its body names the type.
        assert_eq!(
            visibility_key("dev.unhandled", "m.bridge [a]", ""),
            "m.bridge [a]"
        );
        // An m.state row is keyed by state_type, even though its body is empty.
        assert_eq!(
            visibility_key("m.state", "", "m.room.join_rules"),
            "m.room.join_rules"
        );
        // Any other row ignores both.
        assert_eq!(visibility_key("m.text", "hello", ""), "");
    }

    #[test]
    fn dev_unhandled_visibility_agrees_with_the_emit_gate() {
        // A modelled type's redacted form arrives as dev.unhandled: it is a
        // raw row, so it follows show_unhandled.
        assert!(state_event_visible(
            "dev.unhandled",
            "m.room.join_rules",
            false,
            true
        ));
        assert!(!state_event_visible(
            "dev.unhandled",
            "m.room.join_rules",
            true,
            false
        ));
        // Bridge types are raw rows gated by show_state.
        assert!(state_event_visible(
            "dev.unhandled",
            "m.bridge",
            true,
            false
        ));
        assert!(!state_event_visible(
            "dev.unhandled",
            "m.bridge",
            false,
            true
        ));
    }

    #[test]
    fn failed_removals_map_only_removable_types() {
        for t in [
            "m.space.child",
            "m.space.parent",
            "m.policy.rule.room",
            "m.policy.rule.server",
            "m.policy.rule.user",
        ] {
            let r = failed_state_removal(t, "!k:s").unwrap();
            assert_eq!(r.state_type, t);
            assert_eq!(r.state_key, "!k:s");
            assert_eq!(r.change, "removed");
            assert!(r.params.is_empty());
        }
        assert!(failed_state_removal("m.room.join_rules", "").is_none());
        assert!(failed_state_removal("com.example.x", "").is_none());
    }

    #[test]
    fn sender_text_loses_bidi_overrides() {
        let (_, p) = policy_rule_params("@bad:s", "m.ban", "spam\u{202E}gnp.exe");
        assert_eq!(p[2], "spamgnp.exe");
        let (_, p) = policy_rule_params("\u{202E}@bad:s", "m.ban", "");
        assert_eq!(p[0], "@bad:s");
        let (_, p) = canonical_alias_params(Some("#a\u{202E}b:s"), None);
        assert_eq!(p[0], "#ab:s");
    }

    #[test]
    fn only_unmodelled_state_settings_stay_raw() {
        assert!(dev_unhandled_passthrough("m.bridge"));
        assert!(dev_unhandled_passthrough("com.beeper.room_features"));
        assert!(!dev_unhandled_passthrough("m.room.join_rules"));
        assert!(!dev_unhandled_passthrough("m.room.avatar"));
        assert!(!dev_unhandled_passthrough("m.room.create"));
    }
}
