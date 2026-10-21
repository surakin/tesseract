//! MatrixRTC invites (MSC4075 rewrite): which sticky `m.rtc.invite` events are
//! still "active" for us, telling the UI when one appears or goes away, and
//! declining one. Sending our own invite lives in `mrtc_membership`.

use serde_json::{json, Value};

use super::sticky::StickyEntry;
use super::model::{DeclineContent, InviteContent, MemberContent, DECLINE_TYPE, INVITE_TYPE, MEMBER_TYPE};

/// No invite lives longer than this, whatever it claims (MSC4075).
pub const MAX_INVITE_LIFETIME_MS: u64 = 120_000;

/// What the caller knows that the sticky entries do not.
pub struct InviteContext<'a> {
    /// Our user id.
    pub me: &'a str,
    /// In an encrypted room only events that arrived encrypted count (MSC4075
    /// requires invites and declines to be encrypted there, MSC4143 member events too).
    pub room_encrypted: bool,
    /// Whether this user is currently joined to the room (a sticky event can
    /// outlive its sender's membership).
    pub sender_in_room: &'a dyn Fn(&str) -> bool,
    /// Whether the room has an open slot with this slot id.
    pub slot_open: &'a dyn Fn(&str) -> bool,
    /// Whether this sender may trigger a room notification (`@room` mention).
    pub can_notify_room: &'a dyn Fn(&str) -> bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ActiveInvite {
    pub event_id: String,
    pub sender: String,
    pub slot_id: String,
    pub remaining_ms: u64,
}

/// The invites among `entries` (one room's sticky map) that are active for us.
pub fn active_invites(entries: &[StickyEntry], ctx: &InviteContext, now_ms: u64) -> Vec<ActiveInvite> {
    let mine = |e: &&StickyEntry| e.sender == ctx.me;
    let usable = |e: &&StickyEntry| !ctx.room_encrypted || e.was_encrypted;
    let my_members: Vec<(&StickyEntry, MemberContent)> = entries
        .iter()
        .filter(usable)
        .filter(|e| e.event_type == MEMBER_TYPE)
        .filter(mine)
        .filter_map(|e| Some((e, serde_json::from_value(e.content.clone()).ok()?)))
        .collect();
    let my_declines: Vec<String> = entries
        .iter()
        .filter(usable)
        .filter(|e| e.event_type == DECLINE_TYPE)
        .filter(mine)
        .filter_map(|e| serde_json::from_value::<DeclineContent>(e.content.clone()).ok())
        .map(|d| d.relates_to.event_id)
        .collect();

    let mut out: Vec<(u64, ActiveInvite)> = Vec::new();
    for e in entries.iter().filter(usable).filter(|e| e.event_type == INVITE_TYPE) {
        if e.is_withdrawal() || e.sender == ctx.me || !(ctx.sender_in_room)(&e.sender) {
            continue;
        }
        let Ok(inv) = serde_json::from_value::<InviteContent>(e.content.clone()) else { continue };
        if inv.validate().is_err() {
            continue;
        }
        let targeted = inv.mentions.user_ids.iter().any(|u| u == ctx.me)
            || (inv.mentions.room && (ctx.can_notify_room)(&e.sender));
        if !targeted || !(ctx.slot_open)(&inv.slot_id) {
            continue;
        }
        let base = inv.sender_ts.min(e.start_ms);
        let expiry = base.saturating_add(inv.lifetime.min(MAX_INVITE_LIFETIME_MS));
        if expiry <= now_ms {
            continue;
        }
        let acted = my_members.iter().any(|(m, c)| {
            c.slot_id == inv.slot_id && (c.is_join() || m.start_ms >= e.start_ms)
        }) || my_declines.iter().any(|id| *id == e.event_id);
        if acted {
            continue;
        }
        out.push((
            e.start_ms,
            ActiveInvite {
                event_id: e.event_id.clone(),
                sender: e.sender.clone(),
                slot_id: inv.slot_id,
                remaining_ms: expiry - now_ms,
            },
        ));
    }
    // One ring per slot: the earliest invite wins.
    out.sort_by(|a, b| (a.0, &a.1.event_id).cmp(&(b.0, &b.1.event_id)));
    let mut seen_slots: Vec<String> = Vec::new();
    out.into_iter()
        .filter(|(_, i)| {
            if seen_slots.contains(&i.slot_id) {
                false
            } else {
                seen_slots.push(i.slot_id.clone());
                true
            }
        })
        .map(|(_, i)| i)
        .collect()
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum InviteChange {
    /// A newly active invite to show.
    Announce(ActiveInvite),
    /// An invite we announced is no longer active (withdrawn, answered, expired).
    Cancel { event_id: String },
}

/// What to tell the UI given the invite announced earlier for this room and
/// the invites active now (already one per slot). Returns the changes and the
/// event id to remember as announced.
pub fn diff_invites(announced: Option<&str>, active: &[ActiveInvite]) -> (Vec<InviteChange>, Option<String>) {
    let now_active = active.first();
    let mut changes = Vec::new();
    if let Some(old) = announced {
        if now_active.map(|a| a.event_id.as_str()) == Some(old) {
            return (changes, Some(old.to_owned()));
        }
        changes.push(InviteChange::Cancel { event_id: old.to_owned() });
    }
    match now_active {
        Some(a) => {
            changes.push(InviteChange::Announce(a.clone()));
            (changes, Some(a.event_id.clone()))
        }
        None => (changes, None),
    }
}

/// The invite last announced per account and room.
fn announced_store() -> &'static std::sync::Mutex<std::collections::HashMap<(String, String), String>> {
    static STORE: std::sync::OnceLock<std::sync::Mutex<std::collections::HashMap<(String, String), String>>> =
        std::sync::OnceLock::new();
    STORE.get_or_init(Default::default)
}

/// Keep the UI's idea of the room's ringing invite in step with the sticky
/// map: announce a new active invite, cancel one that went away. The UI treats
/// `on_invitation` with a zero lifetime and an event id as a cancellation.
pub async fn notify_active(room: &matrix_sdk::Room) {
    use super::slots::read_slot;
    use super::sticky::{live_entries, now_ms};

    let client = room.client();
    let Some(me) = client.user_id().map(|u| u.to_string()) else { return };
    let key = (me.clone(), room.room_id().to_string());
    let previous = announced_store().lock().unwrap_or_else(|p| p.into_inner()).get(&key).cloned();
    let entries = live_entries(room);
    // Most rooms never have an invite: leave before any state lookup.
    if previous.is_none() && !entries.iter().any(|e| e.event_type == INVITE_TYPE) {
        return;
    }
    if super::signaling::is_call_room(room).await {
        return;
    }
    let now = now_ms();
    let slot = read_slot(room).await;
    let slot_is_open = slot.as_ref().is_some_and(|s| s.is_open());
    let mut senders_in_room: Vec<String> = Vec::new();
    for e in entries.iter().filter(|e| e.event_type == INVITE_TYPE) {
        if senders_in_room.contains(&e.sender) {
            continue;
        }
        if let Ok(u) = matrix_sdk::ruma::UserId::parse(&e.sender) {
            if matches!(room.get_member(&u).await, Ok(Some(m)) if *m.membership() == matrix_sdk::ruma::events::room::member::MembershipState::Join) {
                senders_in_room.push(e.sender.clone());
            }
        }
    }
    let mut may_notify_room: Vec<String> = Vec::new();
    if let Ok(pl) = room.power_levels().await {
        for e in entries.iter().filter(|e| e.event_type == INVITE_TYPE) {
            if let Ok(u) = matrix_sdk::ruma::UserId::parse(&e.sender) {
                if pl.user_can_trigger_room_notification(&u) {
                    may_notify_room.push(e.sender.clone());
                }
            }
        }
    }
    let ctx = InviteContext {
        me: &me,
        room_encrypted: room.encryption_state().is_encrypted(),
        sender_in_room: &|s| senders_in_room.iter().any(|u| u == s),
        slot_open: &|id| slot_is_open && id.eq_ignore_ascii_case(super::model::CALL_SLOT_ID),
        can_notify_room: &|s| may_notify_room.iter().any(|u| u == s),
    };
    let (changes, announced) = diff_invites(previous.as_deref(), &active_invites(&entries, &ctx, now));
    {
        let mut store = announced_store().lock().unwrap_or_else(|p| p.into_inner());
        match announced {
            Some(id) => store.insert(key, id),
            None => store.remove(&key),
        };
    }
    let Some(sink) = super::sink_for_client(&client) else { return };
    let room_id = room.room_id().as_str();
    for change in changes {
        match change {
            InviteChange::Announce(inv) => {
                sink.on_invitation(room_id, &inv.slot_id, &inv.sender, "", inv.remaining_ms, &inv.event_id)
            }
            InviteChange::Cancel { event_id } => sink.on_invitation(room_id, "", "", "", 0, &event_id),
        }
    }
}

/// Content of an `m.rtc.decline` for `invite_event_id`.
pub fn decline_body(invite_event_id: &str) -> Value {
    json!({
        "m.relates_to": { "rel_type": "m.reference", "event_id": invite_event_id },
        "sticky_key": invite_event_id,
        "msc4354_sticky_key": invite_event_id,
    })
}

/// Decline an invite: sticky (so it survives a gappy sync) and encrypted in
/// encrypted rooms; on a homeserver without sticky events a plain event, which
/// matrix-js-sdk still counts on devices that receive it live.
pub async fn send_decline(room: &matrix_sdk::Room, invite_event_id: &str) -> anyhow::Result<()> {
    let body = decline_body(invite_event_id);
    let sticky = super::sticky::send_sticky(
        room,
        DECLINE_TYPE,
        body.clone(),
        super::mrtc_membership::INVITE_STICKY_DURATION,
    )
    .await;
    if sticky.is_ok() {
        return Ok(());
    }
    room.send_raw(DECLINE_TYPE, body)
        .await
        .map_err(|e| anyhow::anyhow!("send decline: {e}"))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::client::rtc::sticky::StickyEntry;
    use crate::client::rtc::model::{DECLINE_TYPE, INVITE_TYPE, MEMBER_TYPE};
    use serde_json::json;

    const NOW: u64 = 5_000_000;
    const ME: &str = "@me:x";

    fn invite(id: &str, sender: &str, content: serde_json::Value, start: u64) -> StickyEntry {
        StickyEntry {
            event_id: id.into(), sender: sender.into(), event_type: INVITE_TYPE.into(),
            sticky_key: "m.call#room".into(), start_ms: start, duration_ms: 90_000,
            was_encrypted: true, sender_device: None, content,
        }
    }

    fn good_content(sender_ts: u64) -> serde_json::Value {
        json!({"slot_id": "m.call#room", "sender_ts": sender_ts, "lifetime": 90_000,
               "m.mentions": {"user_ids": [ME]}, "sticky_key": "m.call#room"})
    }

    fn member(user: &str, join: bool, start: u64) -> StickyEntry {
        StickyEntry {
            event_id: format!("$m-{user}-{start}"), sender: user.into(), event_type: MEMBER_TYPE.into(),
            sticky_key: "mid".into(), start_ms: start, duration_ms: 3_600_000, was_encrypted: true,
            sender_device: None,
            content: json!({"slot_id": "m.call#room",
                "member": {"id": "mid", "membership": if join {"join"} else {"leave"}},
                "sticky_key": "mid"}),
        }
    }

    fn decline(user: &str, invite_id: &str) -> StickyEntry {
        StickyEntry {
            event_id: "$d".into(), sender: user.into(), event_type: DECLINE_TYPE.into(),
            sticky_key: invite_id.into(), start_ms: NOW - 1000, duration_ms: 90_000,
            was_encrypted: true, sender_device: None,
            content: json!({"m.relates_to": {"rel_type": "m.reference", "event_id": invite_id},
                            "sticky_key": invite_id}),
        }
    }

    fn ctx() -> InviteContext<'static> {
        InviteContext { me: ME, room_encrypted: false, slot_open: &|_| true, can_notify_room: &|_| false, sender_in_room: &|_| true }
    }

    fn active(entries: &[StickyEntry], ctx: &InviteContext) -> Vec<ActiveInvite> {
        active_invites(entries, ctx, NOW)
    }

    #[test]
    fn a_targeted_fresh_invite_rings() {
        let got = active(&[invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000)], &ctx());
        assert_eq!(got.len(), 1);
        assert_eq!((got[0].event_id.as_str(), got[0].sender.as_str()), ("$i", "@a:x"));
        assert_eq!(got[0].remaining_ms, 89_000);
    }

    #[test]
    fn inactive_invites_never_ring() {
        let base = || invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        // Own invite.
        assert!(active(&[invite("$i", ME, good_content(NOW - 1000), NOW - 1000)], &ctx()).is_empty());
        // Not targeted at me.
        let mut other = base();
        other.content["m.mentions"] = json!({"user_ids": ["@someone:x"]});
        assert!(active(&[other], &ctx()).is_empty());
        // Slot closed.
        let closed = InviteContext { slot_open: &|_| false, ..ctx() };
        assert!(active(&[base()], &closed).is_empty());
        // Expired.
        let old = invite("$i", "@a:x", good_content(NOW - 200_000), NOW - 200_000);
        assert!(active(&[old], &ctx()).is_empty());
        // Withdrawn (content only holds the sticky key).
        let mut w = base();
        w.content = json!({"sticky_key": "m.call#room"});
        assert!(active(&[w], &ctx()).is_empty());
        // I already joined (from any device).
        assert!(active(&[base(), member(ME, true, NOW - 500)], &ctx()).is_empty());
        // I declined it.
        assert!(active(&[base(), decline(ME, "$i")], &ctx()).is_empty());
        // I joined and left again after the invite: it must not reactivate.
        assert!(active(&[base(), member(ME, false, NOW - 100)], &ctx()).is_empty());
    }

    #[test]
    fn a_leave_older_than_the_invite_does_not_suppress_it() {
        let inv = invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        assert_eq!(active(&[inv, member(ME, false, NOW - 50_000)], &ctx()).len(), 1);
    }

    #[test]
    fn someone_else_joining_or_declining_does_not_matter() {
        let inv = invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        let got = active(&[inv, member("@b:x", true, NOW - 500), decline("@b:x", "$i")], &ctx());
        assert_eq!(got.len(), 1);
    }

    #[test]
    fn room_mentions_need_the_senders_right_to_notify() {
        let mut inv = invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        inv.content["m.mentions"] = json!({"room": true});
        assert!(active(&[inv.clone()], &ctx()).is_empty());
        let allowed = InviteContext { can_notify_room: &|s| s == "@a:x", ..ctx() };
        assert_eq!(active(&[inv], &allowed).len(), 1);
    }

    #[test]
    fn lifetime_is_capped_at_two_minutes_and_measured_from_the_earlier_time() {
        // Claims a 10 minute lifetime, 110 s ago: capped to 120 s total.
        let mut c = good_content(NOW - 110_000);
        c["lifetime"] = json!(600_000);
        let inv = invite("$i", "@a:x", c, NOW - 110_000);
        assert_eq!(active(&[inv], &ctx())[0].remaining_ms, 10_000);
        // A sender_ts in the future cannot extend it: start (receipt) wins.
        let future = invite("$j", "@a:x", good_content(NOW + 1_000_000), NOW - 1000);
        assert_eq!(active(&[future], &ctx())[0].remaining_ms, 89_000);
    }

    #[test]
    fn duplicate_invites_from_two_senders_coalesce() {
        let a = invite("$a", "@a:x", good_content(NOW - 2000), NOW - 2000);
        let b = invite("$b", "@b:x", good_content(NOW - 1000), NOW - 1000);
        let got = active(&[a, b], &ctx());
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].event_id, "$a", "the earliest wins");
    }

    #[test]
    fn cleartext_events_are_ignored_in_an_encrypted_room() {
        let mut inv = invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        inv.was_encrypted = false;
        let enc = InviteContext { room_encrypted: true, ..ctx() };
        assert!(active(&[inv.clone()], &enc).is_empty(), "a cleartext invite cannot ring");
        assert_eq!(active(&[inv.clone()], &ctx()).len(), 1, "fine in an unencrypted room");
        // A cleartext decline or join of ours does not silence a real invite either.
        let mut real = invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        real.was_encrypted = true;
        let mut fake_join = member(ME, true, NOW - 500);
        fake_join.was_encrypted = false;
        let mut fake_decline = decline(ME, "$i");
        fake_decline.was_encrypted = false;
        assert_eq!(active(&[real, fake_join, fake_decline], &enc).len(), 1);
    }

    #[test]
    fn an_invite_from_someone_outside_the_room_never_rings() {
        let inv = invite("$i", "@gone:x", good_content(NOW - 1000), NOW - 1000);
        let outside = InviteContext { sender_in_room: &|s| s != "@gone:x", ..ctx() };
        assert!(active(&[inv.clone()], &outside).is_empty());
        assert_eq!(active(&[inv], &ctx()).len(), 1);
    }

    #[test]
    fn legacy_shaped_notifications_are_not_invites() {
        let mut legacy = invite("$l", "@a:x", json!({"notification_type": "ring", "sender_ts": NOW - 1000,
            "lifetime": 30_000, "m.mentions": {"room": true}}), NOW - 1000);
        legacy.sticky_key = "x".into();
        assert!(active(&[legacy], &ctx()).is_empty());
    }

    #[test]
    fn decline_body_matches_msc() {
        assert_eq!(
            decline_body("$1"),
            json!({
                "m.relates_to": {"rel_type": "m.reference", "event_id": "$1"},
                "sticky_key": "$1",
                "msc4354_sticky_key": "$1",
            })
        );
    }

    // ── announcing and cancelling ───────────────────────────────────────────

    fn act(id: &str) -> ActiveInvite {
        ActiveInvite { event_id: id.into(), sender: "@a:x".into(), slot_id: "m.call#room".into(), remaining_ms: 1000 }
    }

    #[test]
    fn a_new_invite_is_announced_once() {
        let (changes, state) = diff_invites(None, &[act("$1")]);
        assert_eq!(changes, vec![InviteChange::Announce(act("$1"))]);
        assert_eq!(state.as_deref(), Some("$1"));
        let (again, state) = diff_invites(state.as_deref(), &[act("$1")]);
        assert!(again.is_empty(), "no repeat while it stays active");
        assert_eq!(state.as_deref(), Some("$1"));
    }

    #[test]
    fn a_withdrawn_or_answered_invite_is_cancelled() {
        let (changes, state) = diff_invites(Some("$1"), &[]);
        assert_eq!(changes, vec![InviteChange::Cancel { event_id: "$1".into() }]);
        assert_eq!(state, None);
        let (nothing, _) = diff_invites(None, &[]);
        assert!(nothing.is_empty());
    }

    #[test]
    fn a_replacing_invite_cancels_the_old_one_first() {
        let (changes, state) = diff_invites(Some("$1"), &[act("$2")]);
        assert_eq!(
            changes,
            vec![InviteChange::Cancel { event_id: "$1".into() }, InviteChange::Announce(act("$2"))]
        );
        assert_eq!(state.as_deref(), Some("$2"));
    }
}
