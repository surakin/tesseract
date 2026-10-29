//! MatrixRTC invites (MSC4075 rewrite): which sticky `m.rtc.invite` events are
//! still "active" for us, telling the UI when one appears or goes away, and
//! declining one. Sending our own invite lives in `mrtc_membership`.

use serde_json::{json, Value};

use super::sticky::StickyEntry;
use super::model::{DeclineContent, InviteContent, MemberContent, DECLINE_TYPE, INVITE_TYPE, MEMBER_TYPE};

/// No invite lives longer than this, whatever it claims (MSC4075).
pub const MAX_INVITE_LIFETIME_MS: u64 = 120_000;
/// How far an event of ours may predate the invite it answers: the two are
/// stamped by different homeservers, whose clocks need not agree to the ms.
const CLOCK_SKEW_MS: u64 = 5_000;
/// The most callers announced for one room at a time. Callers likely to be
/// genuine (the DM peer, someone already in the call, someone allowed to notify
/// the room) take the places first, the earliest first within each group, so a
/// crowd of early senders cannot push a real caller out.
pub const MAX_ACTIVE_INVITES: usize = 8;

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
    /// Whether we ignore this user: their invites never ring.
    pub sender_ignored: &'a dyn Fn(&str) -> bool,
    /// When (ms since the epoch) this invite event was first seen here, if it
    /// was. An invite never counts as older than that, nor as newer than its
    /// stamp: a future-dated `origin_server_ts` cannot make it ring longer.
    pub first_seen: &'a dyn Fn(&str) -> Option<u64>,
    /// Whether this user is the other party of a direct chat the room is.
    pub is_dm_peer: &'a dyn Fn(&str) -> bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ActiveInvite {
    pub event_id: String,
    pub sender: String,
    pub slot_id: String,
    pub remaining_ms: u64,
}

/// Slot ids compare ASCII case-insensitively, as everywhere else in the call
/// code: the reference implementation writes `m.call#ROOM` where the MSC says
/// `m.call#room`, and the docs leave the case unspecified.
fn same_slot(a: &str, b: &str) -> bool {
    a.eq_ignore_ascii_case(b)
}

/// The invites among `entries` (one room's sticky map) that are active for us.
///
/// Each distinct caller rings on its own: for every `(slot, sender)` only the
/// newest live invite counts, so one member's invite cannot hide the real
/// caller's. The result is ordered earliest first and capped at
/// [`MAX_ACTIVE_INVITES`], keeping likely-genuine callers over the rest.
///
/// An invite lasts from the instant its homeserver stamped it, or from when we
/// first saw it if that is earlier (a stamp in the future must not extend it),
/// for the shorter of its `lifetime` (at most two minutes) and the time it stays
/// sticky. The caller's own `sender_ts` is not used: it is the caller's clock,
/// and one running a minute behind ours would otherwise never ring.
pub fn active_invites(entries: &[StickyEntry], ctx: &InviteContext, now_ms: u64) -> Vec<ActiveInvite> {
    let mine = |e: &&StickyEntry| e.sender == ctx.me;
    let usable = |e: &&StickyEntry| !ctx.room_encrypted || e.was_encrypted;
    let member_entries = || {
        entries
            .iter()
            .filter(usable)
            .filter(|e| e.event_type == MEMBER_TYPE)
            .filter_map(|e| Some((e, serde_json::from_value::<MemberContent>(e.content.clone()).ok()?)))
    };
    let my_members: Vec<(&StickyEntry, MemberContent)> = member_entries().filter(|(e, _)| mine(e)).collect();
    // Who is in a call right now, and in which slot: such a sender is a likely
    // genuine caller.
    let in_call: Vec<(&str, String)> = member_entries()
        .filter(|(e, c)| c.is_join() && e.end_ms() > now_ms && !e.is_withdrawal())
        .map(|(e, c)| (e.sender.as_str(), c.slot_id))
        .collect();
    // Declines from anyone else never matter here: only our own answer ends a ring.
    let my_declines: Vec<String> = entries
        .iter()
        .filter(usable)
        .filter(|e| e.event_type == DECLINE_TYPE)
        .filter(mine)
        .filter_map(|e| serde_json::from_value::<DeclineContent>(e.content.clone()).ok())
        .map(|d| d.relates_to.event_id)
        .collect();

    // The newest live invite per (slot, sender).
    let mut newest: Vec<Candidate> = Vec::new();
    for e in entries.iter().filter(usable).filter(|e| e.event_type == INVITE_TYPE) {
        if e.is_withdrawal()
            || e.sender == ctx.me
            || (ctx.sender_ignored)(&e.sender)
            || !(ctx.sender_in_room)(&e.sender)
        {
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
        let start = (ctx.first_seen)(&e.event_id).map_or(e.start_ms, |seen| seen.min(e.start_ms));
        let expiry = start.saturating_add(inv.lifetime.min(MAX_INVITE_LIFETIME_MS)).min(e.end_ms());
        if expiry <= now_ms {
            continue;
        }
        // Answered by a join of ours that is not older than the invite (give or
        // take the two homeservers' clocks; a join left over from an earlier
        // session must not silence a new ring), by a leave of ours strictly
        // newer than it (hanging up and being rung again within seconds is a new
        // ring), or by a decline of exactly this invite.
        let acted = my_members.iter().any(|(m, c)| {
            same_slot(&c.slot_id, &inv.slot_id)
                && if c.is_join() {
                    m.start_ms.saturating_add(CLOCK_SKEW_MS) >= start
                } else {
                    m.start_ms > start
                }
        }) || my_declines.contains(&e.event_id);
        if acted {
            continue;
        }
        let likely_genuine = (ctx.is_dm_peer)(&e.sender)
            || (ctx.can_notify_room)(&e.sender)
            || in_call.iter().any(|(u, slot)| *u == e.sender && same_slot(slot, &inv.slot_id));
        let candidate = Candidate {
            start,
            likely_genuine,
            invite: ActiveInvite {
                event_id: e.event_id.clone(),
                sender: e.sender.clone(),
                slot_id: inv.slot_id,
                remaining_ms: expiry - now_ms,
            },
        };
        match newest
            .iter_mut()
            .find(|c| c.invite.sender == candidate.invite.sender && same_slot(&c.invite.slot_id, &candidate.invite.slot_id))
        {
            Some(held) => {
                if (candidate.start, &candidate.invite.event_id) > (held.start, &held.invite.event_id) {
                    *held = candidate;
                }
            }
            None => newest.push(candidate),
        }
    }
    newest.sort_by(|a, b| (a.start, &a.invite.event_id).cmp(&(b.start, &b.invite.event_id)));
    if newest.len() > MAX_ACTIVE_INVITES {
        // Likely-genuine callers first (stable: still earliest first inside each
        // group), then back to earliest first.
        newest.sort_by_key(|c| !c.likely_genuine);
        newest.truncate(MAX_ACTIVE_INVITES);
        newest.sort_by(|a, b| (a.start, &a.invite.event_id).cmp(&(b.start, &b.invite.event_id)));
    }
    newest.into_iter().map(|c| c.invite).collect()
}

/// An active invite with what ranks it.
struct Candidate {
    start: u64,
    likely_genuine: bool,
    invite: ActiveInvite,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum InviteChange {
    /// A newly active invite to show.
    Announce(ActiveInvite),
    /// An invite we announced is no longer active (withdrawn, answered, expired).
    Cancel { event_id: String },
}

/// What to tell the UI given the invites announced earlier for this room and
/// the invites active now. Returns the changes (cancellations first) and the
/// event ids to remember as announced.
pub fn diff_invites(announced: &[String], active: &[ActiveInvite]) -> (Vec<InviteChange>, Vec<String>) {
    let mut changes: Vec<InviteChange> = announced
        .iter()
        .filter(|old| !active.iter().any(|a| a.event_id == **old))
        .map(|old| InviteChange::Cancel { event_id: old.clone() })
        .collect();
    changes.extend(
        active
            .iter()
            .filter(|a| !announced.contains(&a.event_id))
            .map(|a| InviteChange::Announce(a.clone())),
    );
    (changes, active.iter().map(|a| a.event_id.clone()).collect())
}

/// Event ids of the invites last announced, per account and room.
type AnnouncedInvites = std::collections::HashMap<(String, String), Vec<String>>;

fn announced_store() -> &'static std::sync::Mutex<AnnouncedInvites> {
    static STORE: std::sync::OnceLock<std::sync::Mutex<AnnouncedInvites>> = std::sync::OnceLock::new();
    STORE.get_or_init(Default::default)
}

/// When each invite event still in a room's sticky map was first seen (ms since
/// the epoch), per account and room.
type FirstSeenTimes = std::collections::HashMap<(String, String), std::collections::HashMap<String, u64>>;

fn first_seen_store() -> &'static std::sync::Mutex<FirstSeenTimes> {
    static STORE: std::sync::OnceLock<std::sync::Mutex<FirstSeenTimes>> = std::sync::OnceLock::new();
    STORE.get_or_init(Default::default)
}

/// Note `now` as the first sight of every id in `live` not seen before and
/// forget ids that are no longer live. Returns the times for the live ids.
pub fn note_first_seen(
    seen: &mut std::collections::HashMap<String, u64>,
    live: &[&str],
    now: u64,
) -> std::collections::HashMap<String, u64> {
    seen.retain(|id, _| live.contains(&id.as_str()));
    for id in live {
        seen.entry((*id).to_owned()).or_insert(now);
    }
    seen.clone()
}

/// The event ids of invites already announced to the UI, from either of the two
/// handlers that can see one (the sticky-map watcher and the plain
/// `rtc.notification` event handler): the same event must show up once, under
/// the MatrixRTC slot id the sticky-map watcher knows it by. Bounded, oldest
/// forgotten first.
#[derive(Default)]
pub struct AnnouncedIds {
    order: std::collections::VecDeque<String>,
}

impl AnnouncedIds {
    const CAP: usize = 256;

    /// Remember `id`. True if it was not remembered before.
    pub fn claim(&mut self, id: &str) -> bool {
        if self.order.iter().any(|i| i == id) {
            return false;
        }
        if self.order.len() >= Self::CAP {
            self.order.pop_front();
        }
        self.order.push_back(id.to_owned());
        true
    }
}

fn announced_ids() -> &'static std::sync::Mutex<AnnouncedIds> {
    static IDS: std::sync::OnceLock<std::sync::Mutex<AnnouncedIds>> = std::sync::OnceLock::new();
    IDS.get_or_init(Default::default)
}

/// Whether the plain-event ring handler should announce this event: only if
/// no handler announced it before and the sticky map does not hold it (the
/// sticky-map watcher announces those, with their real slot id). Marks it
/// announced when it returns true.
pub fn claim_legacy_ring(room: &matrix_sdk::Room, event_id: &str) -> bool {
    if super::sticky::live_entries(room).iter().any(|e| e.event_type == INVITE_TYPE && e.event_id == event_id) {
        return false;
    }
    announced_ids().lock().unwrap_or_else(|p| p.into_inner()).claim(event_id)
}

/// Keep the UI's idea of the room's ringing invites in step with the sticky
/// map: announce each newly active invite, cancel each that went away. The UI
/// treats `on_invitation` with a zero lifetime and an event id as the
/// cancellation of that one invite.
pub async fn notify_active(room: &matrix_sdk::Room) {
    use super::slots::read_slot;
    use super::sticky::{live_entries, now_ms};

    let client = room.client();
    let Some(me) = client.user_id().map(|u| u.to_string()) else { return };
    let key = (me.clone(), room.room_id().to_string());
    let previous = announced_store().lock().unwrap_or_else(|p| p.into_inner()).get(&key).cloned().unwrap_or_default();
    let entries = live_entries(room);
    // Most rooms never have an invite: leave before any state lookup.
    if previous.is_empty() && !entries.iter().any(|e| e.event_type == INVITE_TYPE) {
        return;
    }
    if super::signaling::is_call_room(room).await {
        return;
    }
    let now = now_ms();
    let first_seen = {
        let live: Vec<&str> = entries.iter().filter(|e| e.event_type == INVITE_TYPE).map(|e| e.event_id.as_str()).collect();
        let mut store = first_seen_store().lock().unwrap_or_else(|p| p.into_inner());
        let times = note_first_seen(store.entry(key.clone()).or_default(), &live, now);
        if times.is_empty() {
            store.remove(&key);
        }
        times
    };
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
    let ignored = super::sticky::ignored_users(&client).await;
    let dm_peers: Vec<String> = if room.is_direct().await.unwrap_or(false) {
        room.direct_targets().iter().map(|t| t.to_string()).collect()
    } else {
        Vec::new()
    };
    let ctx = InviteContext {
        me: &me,
        room_encrypted: room.encryption_state().is_encrypted(),
        sender_in_room: &|s| senders_in_room.iter().any(|u| u == s),
        slot_open: &|id| slot_is_open && id.eq_ignore_ascii_case(super::model::CALL_SLOT_ID),
        can_notify_room: &|s| may_notify_room.iter().any(|u| u == s),
        sender_ignored: &|s| ignored.contains(s),
        first_seen: &|id| first_seen.get(id).copied(),
        is_dm_peer: &|s| dm_peers.iter().any(|u| u == s),
    };
    let (changes, announced) = diff_invites(&previous, &active_invites(&entries, &ctx, now));
    {
        let mut store = announced_store().lock().unwrap_or_else(|p| p.into_inner());
        if announced.is_empty() {
            store.remove(&key);
        } else {
            store.insert(key, announced);
        }
    }
    let Some(sink) = super::sink_for_client(&client) else { return };
    let room_id = room.room_id().as_str();
    for change in changes {
        match change {
            InviteChange::Announce(inv) => {
                // The plain-event handler must not announce it again under its own slot label.
                announced_ids().lock().unwrap_or_else(|p| p.into_inner()).claim(&inv.event_id);
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
/// encrypted rooms; on a homeserver that cannot take sticky events a plain
/// event, which matrix-js-sdk still counts on devices that receive it live.
/// Only a definite refusal falls back: any other failure may have stored the
/// sticky decline, and a second one would be a duplicate.
pub async fn send_decline(room: &matrix_sdk::Room, invite_event_id: &str) -> anyhow::Result<()> {
    let body = decline_body(invite_event_id);
    let sticky = super::sticky::send_sticky(
        room,
        DECLINE_TYPE,
        body.clone(),
        super::mrtc_membership::INVITE_STICKY_DURATION,
    )
    .await;
    match sticky {
        Ok(_) => return Ok(()),
        Err(e) if !super::sticky::is_sticky_unsupported(&e) => return Err(e),
        Err(_) => {}
    }
    room.send_raw(DECLINE_TYPE, body)
        .await
        .map_err(|e| anyhow::anyhow!("send decline: {e}"))?;
    Ok(())
}

/// Declines waiting to be sent, oldest first, sent one at a time by a single
/// worker: a burst of them (one per invite of a banner) would otherwise hit the
/// homeserver's rate limit all at once, and arrive in no particular order.
pub struct DeclineQueue<T> {
    items: std::collections::VecDeque<T>,
    /// The running worker's lease; it is alive as long as the worker task is.
    worker: Option<std::sync::Weak<()>>,
}

/// Held by the worker task for as long as it exists. The task can end without
/// draining the queue (its runtime was dropped with the client that started it,
/// or it panicked); the lease then dies with it and the next decline starts a
/// new worker instead of waiting for one that is gone.
pub type WorkerLease = std::sync::Arc<()>;

impl<T> Default for DeclineQueue<T> {
    fn default() -> Self {
        Self { items: Default::default(), worker: None }
    }
}

impl<T> DeclineQueue<T> {
    /// Queue `item`. Returns a lease if the caller must start the worker (none
    /// is alive): the worker keeps it until it ends.
    pub fn push(&mut self, item: T) -> Option<WorkerLease> {
        self.items.push_back(item);
        if self.worker.as_ref().is_some_and(|w| w.strong_count() > 0) {
            return None;
        }
        let lease = WorkerLease::new(());
        self.worker = Some(std::sync::Arc::downgrade(&lease));
        Some(lease)
    }

    /// The next item, or `None` once the queue is empty, which also ends the
    /// worker: the next [`push`](Self::push) asks for a new one.
    pub fn pop(&mut self) -> Option<T> {
        let next = self.items.pop_front();
        if next.is_none() {
            self.worker = None;
        }
        next
    }
}

/// Run `attempt`; if it fails, wait `delay` and run it once more.
pub async fn with_one_retry<T, F, Fut>(mut attempt: F, delay: std::time::Duration) -> anyhow::Result<T>
where
    F: FnMut() -> Fut,
    Fut: std::future::Future<Output = anyhow::Result<T>>,
{
    match attempt().await {
        Ok(v) => Ok(v),
        Err(_) => {
            tokio::time::sleep(delay).await;
            attempt().await
        }
    }
}

/// Whether a failure may be logged now, given when one last was (ms since the
/// epoch; 0 for never): at most one warning per [`DECLINE_WARN_EVERY_MS`].
pub fn warn_due(last_ms: u64, now_ms: u64) -> bool {
    last_ms == 0 || now_ms.saturating_sub(last_ms) >= DECLINE_WARN_EVERY_MS
}

const DECLINE_WARN_EVERY_MS: u64 = 30_000;
const DECLINE_RETRY_DELAY: std::time::Duration = std::time::Duration::from_secs(2);

struct DeclineJob {
    client: matrix_sdk::Client,
    room_id: matrix_sdk::ruma::OwnedRoomId,
    invite_event_id: String,
}

fn decline_queue() -> &'static std::sync::Mutex<DeclineQueue<DeclineJob>> {
    static QUEUE: std::sync::OnceLock<std::sync::Mutex<DeclineQueue<DeclineJob>>> = std::sync::OnceLock::new();
    QUEUE.get_or_init(Default::default)
}

/// Decline `invite_event_id` in the background: queued behind earlier declines
/// and sent in order, one at a time, each retried once. Spawns the sending
/// worker on the current tokio runtime when none is running.
pub fn queue_decline(client: matrix_sdk::Client, room_id: matrix_sdk::ruma::OwnedRoomId, invite_event_id: String) {
    let lease = decline_queue()
        .lock()
        .unwrap_or_else(|p| p.into_inner())
        .push(DeclineJob { client, room_id, invite_event_id });
    let Some(lease) = lease else {
        return;
    };
    tokio::spawn(async move {
        // Dropped when the task ends, however it ends.
        let _lease = lease;
        static LAST_WARN: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        loop {
            let Some(job) = decline_queue().lock().unwrap_or_else(|p| p.into_inner()).pop() else {
                return;
            };
            let Some(room) = job.client.get_room(&job.room_id) else { continue };
            let sent = with_one_retry(|| send_decline(&room, &job.invite_event_id), DECLINE_RETRY_DELAY).await;
            if let Err(e) = sent {
                let now = super::sticky::now_ms();
                let last = LAST_WARN.load(std::sync::atomic::Ordering::Relaxed);
                if warn_due(last, now) {
                    LAST_WARN.store(now, std::sync::atomic::Ordering::Relaxed);
                    tracing::warn!("rtc: declining {} failed: {e}", job.invite_event_id);
                }
            }
        }
    });
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
        InviteContext {
            me: ME, room_encrypted: false, slot_open: &|_| true, can_notify_room: &|_| false,
            sender_in_room: &|_| true, sender_ignored: &|_| false,
            first_seen: &|_| None, is_dm_peer: &|_| false,
        }
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
    fn lifetime_is_capped_at_two_minutes_and_by_the_sticky_duration() {
        // Claims a 10 minute lifetime, 110 s ago, and stays sticky 5 minutes: capped to 120 s.
        let mut c = good_content(NOW - 110_000);
        c["lifetime"] = json!(600_000);
        let mut inv = invite("$i", "@a:x", c, NOW - 110_000);
        inv.duration_ms = 300_000;
        assert_eq!(active(&[inv], &ctx())[0].remaining_ms, 10_000);
        // Sticky for only 30 s: it cannot ring after the SDK dropped it.
        let mut short = invite("$j", "@a:x", good_content(NOW - 1000), NOW - 1000);
        short.duration_ms = 30_000;
        assert_eq!(active(&[short], &ctx())[0].remaining_ms, 29_000);
    }

    #[test]
    fn the_callers_clock_does_not_decide_whether_it_rings() {
        // Sent a few minutes "ago" by the caller's slow clock; the homeserver
        // stamped it a second ago.
        let slow = invite("$i", "@a:x", good_content(NOW - 600_000), NOW - 1000);
        assert_eq!(active(&[slow], &ctx())[0].remaining_ms, 89_000);
        // A sender_ts in the future cannot extend it either.
        let fast = invite("$j", "@a:x", good_content(NOW + 1_000_000), NOW - 1000);
        assert_eq!(active(&[fast], &ctx())[0].remaining_ms, 89_000);
    }

    #[test]
    fn every_caller_rings_and_only_the_newest_invite_per_caller_counts() {
        let a = invite("$a", "@a:x", good_content(NOW - 2000), NOW - 2000);
        let b = invite("$b", "@b:x", good_content(NOW - 1000), NOW - 1000);
        let got = active(&[b.clone(), a.clone()], &ctx());
        assert_eq!(got.iter().map(|i| i.event_id.as_str()).collect::<Vec<_>>(), vec!["$a", "$b"], "earliest first");
        // The same caller under two spellings of the slot id: the newer wins.
        let mut upper = invite("$a2", "@a:x", good_content(NOW - 500), NOW - 500);
        upper.content["slot_id"] = json!("m.call#ROOM");
        upper.sticky_key = "m.call#ROOM".into();
        let got = active(&[a, upper, b], &ctx());
        assert_eq!(got.iter().map(|i| i.event_id.as_str()).collect::<Vec<_>>(), vec!["$b", "$a2"]);
    }

    #[test]
    fn a_member_cannot_hide_the_real_caller_with_an_earlier_invite() {
        // A bystander invites us first; the DM peer's invite still rings.
        let spam = invite("$spam", "@spam:x", good_content(NOW - 60_000), NOW - 60_000);
        let real = invite("$real", "@peer:x", good_content(NOW - 1000), NOW - 1000);
        let got = active(&[spam, real], &ctx());
        assert!(got.iter().any(|i| i.event_id == "$real"));
    }

    #[test]
    fn the_number_of_announced_callers_is_capped_keeping_the_earliest() {
        let many: Vec<StickyEntry> = (0..MAX_ACTIVE_INVITES as u64 + 4)
            .map(|n| invite(&format!("$i{n}"), &format!("@u{n}:x"), good_content(NOW - 50_000 + n), NOW - 50_000 + n))
            .collect();
        let got = active(&many, &ctx());
        assert_eq!(got.len(), MAX_ACTIVE_INVITES);
        assert_eq!(got[0].event_id, "$i0");
        assert_eq!(got[MAX_ACTIVE_INVITES - 1].event_id, format!("$i{}", MAX_ACTIVE_INVITES - 1));
    }

    #[test]
    fn invites_from_ignored_users_never_ring() {
        let a = invite("$a", "@ignored:x", good_content(NOW - 1000), NOW - 1000);
        let b = invite("$b", "@fine:x", good_content(NOW - 1000), NOW - 1000);
        let ignoring = InviteContext { sender_ignored: &|s| s == "@ignored:x", ..ctx() };
        let got = active(&[a, b], &ignoring);
        assert_eq!(got.iter().map(|i| i.event_id.as_str()).collect::<Vec<_>>(), vec!["$b"]);
    }

    #[test]
    fn a_join_left_over_from_an_earlier_session_does_not_silence_a_new_invite() {
        let inv = invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        // Our join from a session that crashed three minutes ago is still sticky.
        let stale = member(ME, true, NOW - 180_000);
        assert_eq!(active(&[inv.clone(), stale], &ctx()).len(), 1);
        // A join just after the invite answers it, even if the two homeservers'
        // clocks differ by a couple of seconds.
        assert!(active(&[inv.clone(), member(ME, true, NOW - 3000)], &ctx()).is_empty());
        assert!(active(&[inv, member(ME, true, NOW - 500)], &ctx()).is_empty());
    }

    #[test]
    fn a_future_dated_invite_rings_for_its_lifetime_from_first_sight_not_until_the_sticky_end() {
        // Stamped ten minutes ahead, sticky for the full hour.
        let mut inv = invite("$i", "@a:x", good_content(NOW), NOW + 600_000);
        inv.duration_ms = 3_600_000;
        let seen_now = InviteContext { first_seen: &|_| Some(NOW - 1000), ..ctx() };
        let got = active(&[inv.clone()], &seen_now);
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].remaining_ms, 89_000, "90 s from first sight");
        // Once that has passed it is gone, instead of ringing for up to an hour.
        let later = InviteContext { first_seen: &|_| Some(NOW - 130_000), ..ctx() };
        assert!(active(&[inv.clone()], &later).is_empty());
        // It does not silence an answer-check either: a join of ours right
        // after first sight answers it, an older one does not.
        let joined = member(ME, true, NOW - 500);
        assert!(active(&[inv.clone(), joined], &seen_now).is_empty());
        let stale = member(ME, true, NOW - 180_000);
        assert_eq!(active(&[inv, stale], &seen_now).len(), 1);
    }

    #[test]
    fn first_sight_never_makes_an_old_invite_newer() {
        // Stamped 60 s ago; we only noticed it a moment ago: the stamp rules.
        let inv = invite("$i", "@a:x", good_content(NOW - 60_000), NOW - 60_000);
        let seen = InviteContext { first_seen: &|_| Some(NOW - 1000), ..ctx() };
        assert_eq!(active(&[inv], &seen)[0].remaining_ms, 30_000);
    }

    #[test]
    fn first_sight_times_are_kept_per_live_event_only() {
        let mut seen = std::collections::HashMap::new();
        let t = note_first_seen(&mut seen, &["$a"], 100);
        assert_eq!(t.get("$a"), Some(&100));
        let t = note_first_seen(&mut seen, &["$a", "$b"], 200);
        assert_eq!((t.get("$a"), t.get("$b")), (Some(&100), Some(&200)), "the first sight sticks");
        let t = note_first_seen(&mut seen, &["$b"], 300);
        assert!(!t.contains_key("$a"), "forgotten once no longer live");
        assert_eq!(t.get("$b"), Some(&200));
    }

    #[test]
    fn a_leave_of_ours_just_after_the_invite_does_not_silence_a_new_one() {
        // We hung up at T, the caller invites again 3 s later: a leave inside
        // the clock-skew slack is older than the invite, so the ring shows.
        let inv = invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        assert_eq!(active(&[inv.clone(), member(ME, false, NOW - 4000)], &ctx()).len(), 1);
        assert_eq!(active(&[inv.clone(), member(ME, false, NOW - 1000)], &ctx()).len(), 1, "same ms is not newer");
        // A leave strictly after the invite still answers it.
        assert!(active(&[inv.clone(), member(ME, false, NOW - 999)], &ctx()).is_empty());
        // A join inside the slack still answers (clocks of two homeservers).
        assert!(active(&[inv, member(ME, true, NOW - 4000)], &ctx()).is_empty());
    }

    #[test]
    fn a_late_honest_caller_is_not_crowded_out_by_early_senders() {
        let mut many: Vec<StickyEntry> = (0..MAX_ACTIVE_INVITES as u64 + 4)
            .map(|n| invite(&format!("$s{n}"), &format!("@spam{n}:x"), good_content(NOW - 50_000 + n), NOW - 50_000 + n))
            .collect();
        many.push(invite("$peer", "@peer:x", good_content(NOW - 1000), NOW - 1000));
        // The DM peer ranks first.
        let dm = InviteContext { is_dm_peer: &|s| s == "@peer:x", ..ctx() };
        let got = active(&many, &dm);
        assert_eq!(got.len(), MAX_ACTIVE_INVITES);
        assert!(got.iter().any(|i| i.event_id == "$peer"));
        assert_eq!(got.last().unwrap().event_id, "$peer", "still ordered earliest first");
        assert_eq!(got[0].event_id, "$s0");
        // Without a rank the late sender is capped out, as before.
        assert!(!active(&many, &ctx()).iter().any(|i| i.event_id == "$peer"));
        // Someone who may notify the room ranks too.
        let notify = InviteContext { can_notify_room: &|s| s == "@peer:x", ..ctx() };
        assert!(active(&many, &notify).iter().any(|i| i.event_id == "$peer"));
        // So does a sender already in the call.
        many.push(member("@peer:x", true, NOW - 2000));
        assert!(active(&many, &ctx()).iter().any(|i| i.event_id == "$peer"));
    }

    #[test]
    fn a_member_who_left_or_expired_does_not_rank_as_in_the_call() {
        let mut many: Vec<StickyEntry> = (0..MAX_ACTIVE_INVITES as u64 + 2)
            .map(|n| invite(&format!("$s{n}"), &format!("@spam{n}:x"), good_content(NOW - 50_000 + n), NOW - 50_000 + n))
            .collect();
        many.push(invite("$late", "@late:x", good_content(NOW - 1000), NOW - 1000));
        many.push(member("@late:x", false, NOW - 2000));
        assert!(!active(&many, &ctx()).iter().any(|i| i.event_id == "$late"));
    }

    #[test]
    fn an_event_is_announced_once_across_both_handlers() {
        let mut ids = AnnouncedIds::default();
        assert!(ids.claim("$1"));
        assert!(!ids.claim("$1"), "the second handler to see it stays quiet");
        assert!(ids.claim("$2"));
    }

    #[test]
    fn the_announced_set_is_bounded_forgetting_the_oldest() {
        let mut ids = AnnouncedIds::default();
        for n in 0..AnnouncedIds::CAP + 10 {
            assert!(ids.claim(&format!("${n}")));
        }
        assert!(ids.order.len() <= AnnouncedIds::CAP);
        assert!(ids.claim("$0"), "the oldest was forgotten");
        assert!(!ids.claim(&format!("${}", AnnouncedIds::CAP + 9)), "a recent one is still known");
    }

    #[test]
    fn slot_ids_are_compared_ignoring_ascii_case() {
        let mut inv = invite("$i", "@a:x", good_content(NOW - 1000), NOW - 1000);
        inv.content["slot_id"] = json!("m.call#ROOM");
        // Our join is under the other spelling of the same slot.
        assert!(active(&[inv, member(ME, true, NOW - 500)], &ctx()).is_empty());
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
    fn declines_go_out_in_order_by_a_single_worker() {
        let mut q = DeclineQueue::default();
        let lease = q.push("$inv");
        assert!(lease.is_some(), "the first push starts the worker");
        assert!(q.push("$ring").is_none(), "a running worker takes the rest");
        assert_eq!(q.pop(), Some("$inv"));
        assert_eq!(q.pop(), Some("$ring"));
        assert_eq!(q.pop(), None, "drained: the worker ends");
        assert!(q.push("$later").is_some(), "so the next decline starts a new one");
    }

    #[test]
    fn a_worker_that_died_with_its_runtime_is_replaced_by_the_next_decline() {
        let mut q = DeclineQueue::default();
        let lease = q.push("$a").expect("first push starts the worker");
        assert!(q.push("$b").is_none());
        // The worker's task is gone before it drained the queue.
        drop(lease);
        let lease = q.push("$c");
        assert!(lease.is_some(), "a new worker is started, not waited for");
        // Nothing was lost, and the new worker takes it all in order.
        assert_eq!(
            [q.pop(), q.pop(), q.pop(), q.pop()],
            [Some("$a"), Some("$b"), Some("$c"), None]
        );
    }

    #[test]
    fn a_worker_task_that_panics_or_is_cancelled_gives_up_its_lease() {
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        let mut q = DeclineQueue::default();
        let lease = q.push(1).unwrap();
        let task = rt.spawn(async move {
            let _lease = lease;
            std::future::pending::<()>().await;
        });
        assert!(q.push(2).is_none(), "alive while its task runs");
        // The runtime goes away with the client that started the worker.
        drop(task);
        drop(rt);
        assert!(q.push(3).is_some());
    }

    #[tokio::test]
    async fn a_failed_decline_is_tried_once_more() {
        let calls = std::sync::atomic::AtomicU32::new(0);
        let flaky = || async {
            if calls.fetch_add(1, std::sync::atomic::Ordering::SeqCst) == 0 {
                Err(anyhow::anyhow!("timed out"))
            } else {
                Ok(())
            }
        };
        assert!(with_one_retry(flaky, std::time::Duration::ZERO).await.is_ok());
        assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst), 2);

        let calls = std::sync::atomic::AtomicU32::new(0);
        let broken = || async {
            calls.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            Err::<(), _>(anyhow::anyhow!("still down"))
        };
        assert!(with_one_retry(broken, std::time::Duration::ZERO).await.is_err());
        assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst), 2, "never a third try");

        let calls = std::sync::atomic::AtomicU32::new(0);
        let fine = || async {
            calls.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            Ok(())
        };
        assert!(with_one_retry(fine, std::time::Duration::ZERO).await.is_ok());
        assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst), 1);
    }

    #[test]
    fn decline_failures_are_warned_about_at_most_every_half_minute() {
        assert!(warn_due(0, 1_000), "never warned before");
        assert!(!warn_due(1_000, 20_000));
        assert!(warn_due(1_000, 31_000));
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

    fn ids(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| (*s).to_owned()).collect()
    }

    #[test]
    fn a_new_invite_is_announced_once() {
        let (changes, state) = diff_invites(&[], &[act("$1")]);
        assert_eq!(changes, vec![InviteChange::Announce(act("$1"))]);
        assert_eq!(state, ids(&["$1"]));
        let (again, state) = diff_invites(&state, &[act("$1")]);
        assert!(again.is_empty(), "no repeat while it stays active");
        assert_eq!(state, ids(&["$1"]));
    }

    #[test]
    fn a_withdrawn_or_answered_invite_is_cancelled() {
        let (changes, state) = diff_invites(&ids(&["$1"]), &[]);
        assert_eq!(changes, vec![InviteChange::Cancel { event_id: "$1".into() }]);
        assert!(state.is_empty());
        let (nothing, _) = diff_invites(&[], &[]);
        assert!(nothing.is_empty());
    }

    #[test]
    fn a_replacing_invite_cancels_the_old_one_first() {
        let (changes, state) = diff_invites(&ids(&["$1"]), &[act("$2")]);
        assert_eq!(
            changes,
            vec![InviteChange::Cancel { event_id: "$1".into() }, InviteChange::Announce(act("$2"))]
        );
        assert_eq!(state, ids(&["$2"]));
    }

    #[test]
    fn several_callers_are_tracked_and_cancelled_one_by_one() {
        let (changes, state) = diff_invites(&[], &[act("$1"), act("$2")]);
        assert_eq!(changes, vec![InviteChange::Announce(act("$1")), InviteChange::Announce(act("$2"))]);
        // One of them goes away; the other is neither re-announced nor cancelled.
        let (changes, state) = diff_invites(&state, &[act("$2")]);
        assert_eq!(changes, vec![InviteChange::Cancel { event_id: "$1".into() }]);
        assert_eq!(state, ids(&["$2"]));
    }
}
