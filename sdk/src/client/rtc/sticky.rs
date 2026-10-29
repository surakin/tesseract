//! Sticky MatrixRTC events (MSC4354), read from the SDK's per-room sticky map
//! (`Room::sticky_events()`), which the sliding-sync sticky extension and the
//! timeline both feed. The SDK keeps the live entry per `(sender, type,
//! sticky_key)`, expires them and retries decryption, so all that is left here
//! is turning its entries into the plain [`StickyEntry`] the slot and invite
//! logic works on.

use std::collections::{HashMap, HashSet};
use std::hash::Hash;
use std::time::{Duration, Instant};

use matrix_sdk_base::sticky::{StickyEvent, StickyKey};
use matrix_sdk::{
    ruma::{api::error::ErrorKind, events::ignored_user_list::IgnoredUserListEventContent, OwnedRoomId, UInt},
    Client, Room,
};
use serde_json::Value;
use tracing::debug;

use super::model::{sticky_key_of, DECLINE_TYPE, INVITE_TYPE, MEMBER_TYPE};

/// Servers cap stickiness at one hour; so do we.
pub const MAX_STICKY_MS: u64 = 3_600_000;

const TRACKED: [&str; 3] = [MEMBER_TYPE, INVITE_TYPE, DECLINE_TYPE];

pub fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

/// One live sticky event, in the shape the slot and invite logic reads.
#[derive(Clone, Debug)]
pub struct StickyEntry {
    pub event_id: String,
    pub sender: String,
    pub event_type: String,
    pub sticky_key: String,
    /// The event's `origin_server_ts`, in ms since the epoch.
    pub start_ms: u64,
    pub duration_ms: u64,
    pub was_encrypted: bool,
    /// The device the event was sent from, when decryption attributed one.
    pub sender_device: Option<String>,
    pub content: Value,
}

impl StickyEntry {
    pub fn end_ms(&self) -> u64 {
        self.start_ms.saturating_add(self.duration_ms.min(MAX_STICKY_MS))
    }

    /// A sticky event whose content holds only the sticky key removes the
    /// entry it replaces (MSC4354 "removing entries").
    pub fn is_withdrawal(&self) -> bool {
        self.content
            .as_object()
            .is_some_and(|o| o.keys().all(|k| k == "sticky_key" || k == "msc4354_sticky_key"))
    }
}

/// Build an entry from a (decrypted) event's JSON and the instant the SDK says
/// it stops being sticky. `None` unless it is one of the tracked types with a
/// sticky key.
pub fn entry_from_parts(
    ev: &Value,
    expires_ms: u64,
    was_encrypted: bool,
    sender_device: Option<String>,
) -> Option<StickyEntry> {
    let event_type = ev.get("type")?.as_str()?;
    if !TRACKED.contains(&event_type) {
        return None;
    }
    let content = ev.get("content")?;
    let sticky_key = sticky_key_of(content)?;
    let origin_ts = ev.get("origin_server_ts")?.as_u64()?;
    // The entry lasts until `expires_ms`, whatever the event claimed.
    let start_ms = origin_ts.min(expires_ms);
    Some(StickyEntry {
        event_id: ev.get("event_id")?.as_str()?.to_owned(),
        sender: ev.get("sender")?.as_str()?.to_owned(),
        event_type: event_type.to_owned(),
        sticky_key,
        start_ms,
        duration_ms: expires_ms - start_ms,
        was_encrypted,
        sender_device,
        content: content.clone(),
    })
}

fn entry_from_sticky_event(e: &StickyEvent) -> Option<StickyEntry> {
    let json: Value = serde_json::from_str(e.raw().json().get()).ok()?;
    let expires_ms = u64::from(UInt::from(e.expires_at.0));
    let info = e.encryption_info();
    entry_from_parts(
        &json,
        expires_ms,
        info.is_some(),
        info.and_then(|i| i.sender_device.as_ref()).map(|d| d.to_string()),
    )
}

/// Parsed entries of one room, kept between reads. A room's sticky map holds up
/// to 5000 events and is read on every update, so a member sending many
/// distinct keys must cost one parse each, not one per read. An entry is reused
/// while its key still maps to the same event with the same expiry; whatever
/// left the live set is dropped from the cache on the next read.
pub struct EntryCache<K> {
    map: HashMap<K, Cached>,
}

struct Cached {
    event_id: String,
    expires_ms: u64,
    /// `None` for a tracked event that did not parse: remembered too.
    entry: Option<StickyEntry>,
}

impl<K> Default for EntryCache<K> {
    fn default() -> Self {
        Self { map: HashMap::new() }
    }
}

impl<K: Hash + Eq> EntryCache<K> {
    /// The entries of `live` (`key`, `event_id`, `expires_ms`, payload),
    /// parsing a payload with `parse` only when its key is new or now maps to
    /// another event.
    pub fn collect<T>(
        &mut self,
        live: impl IntoIterator<Item = (K, String, u64, T)>,
        parse: impl Fn(&T) -> Option<StickyEntry>,
    ) -> Vec<StickyEntry> {
        let mut next = HashMap::new();
        let mut out = Vec::new();
        for (key, event_id, expires_ms, payload) in live {
            let cached = match self.map.remove(&key) {
                Some(c) if c.event_id == event_id && c.expires_ms == expires_ms => c,
                _ => Cached { entry: parse(&payload), event_id, expires_ms },
            };
            out.extend(cached.entry.clone());
            next.insert(key, cached);
        }
        self.map = next;
        out
    }

    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

/// One cache per (account, room): the same event decrypts to different
/// attribution on another account.
fn entry_caches() -> &'static parking_lot::Mutex<HashMap<(String, String), EntryCache<StickyKey>>> {
    static CACHES: std::sync::OnceLock<
        parking_lot::Mutex<HashMap<(String, String), EntryCache<StickyKey>>>,
    > = std::sync::OnceLock::new();
    CACHES.get_or_init(Default::default)
}

/// The live tracked sticky events of a room. Events of other types are skipped
/// before anything is parsed.
pub fn live_entries(room: &Room) -> Vec<StickyEntry> {
    let live = room.sticky_events().live();
    let tracked = live
        .into_iter()
        .filter(|e| TRACKED.contains(&e.key.event_type.to_string().as_str()))
        .map(|e| {
            let expires_ms = u64::from(UInt::from(e.expires_at.0));
            (e.key.clone(), e.event_id.to_string(), expires_ms, e)
        });
    let id = (
        room.client().user_id().map(|u| u.to_string()).unwrap_or_default(),
        room.room_id().to_string(),
    );
    let mut caches = entry_caches().lock();
    let cache = caches.entry(id.clone()).or_default();
    let out = cache.collect(tracked, entry_from_sticky_event);
    if cache.is_empty() {
        caches.remove(&id);
    }
    out
}

/// Spacing for work triggered by every update of a map that a hostile member
/// can make change thousands of times: at most one pass per `min_interval`,
/// and an update that arrives sooner is answered by one pass at the end of the
/// interval (the trailing pass).
pub struct PassGate {
    min_interval: Duration,
    last_pass: Option<Instant>,
}

impl PassGate {
    pub fn new(min_interval: Duration) -> Self {
        Self { min_interval, last_pass: None }
    }

    /// How long to wait, from `now`, before the pass an update asks for.
    pub fn delay(&self, now: Instant) -> Duration {
        match self.last_pass {
            Some(last) => self.min_interval.saturating_sub(now.saturating_duration_since(last)),
            None => Duration::ZERO,
        }
    }

    /// Record that a pass started at `now`.
    pub fn ran(&mut self, now: Instant) {
        self.last_pass = Some(now);
    }
}

/// How often the invite watcher re-reads a room's sticky map at most.
const INVITE_PASS_INTERVAL: Duration = Duration::from_millis(250);

/// Whether `room` has any call member (sticky or legacy), by the local store
/// alone; cheap enough to ask of every room.
pub async fn room_has_call_members(room: &Room) -> bool {
    !live_entries(room).is_empty()
        || !super::members::read_call_members(room, None, super::members::JoinCheck::Cached).await.is_empty()
}

/// The homeserver definitely did not take a sticky event (it does not support
/// MSC4354, or refused the request as unrecognised or forbidden), so nothing
/// was stored. Any other failure may have landed.
#[derive(Debug)]
pub struct StickyUnsupported(pub String);

impl std::fmt::Display for StickyUnsupported {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for StickyUnsupported {}

/// Whether an error from a send is a definite "not accepted" from the server.
pub fn is_sticky_unsupported(e: &anyhow::Error) -> bool {
    e.downcast_ref::<StickyUnsupported>().is_some()
}

/// Whether a homeserver error kind means the sticky request was refused outright.
fn refuses_sticky(kind: Option<&ErrorKind>) -> bool {
    matches!(kind, Some(ErrorKind::Unrecognized | ErrorKind::Forbidden))
}

/// The user ids the account ignores, read from the local store (no request).
pub async fn ignored_users(client: &Client) -> HashSet<String> {
    match client.account().account_data::<IgnoredUserListEventContent>().await {
        Ok(Some(raw)) => raw
            .deserialize()
            .map(|c| c.ignored_users.keys().map(|u| u.to_string()).collect())
            .unwrap_or_default(),
        _ => HashSet::new(),
    }
}

/// Send a sticky message-like event: encrypted in encrypted rooms, with the
/// given sticky duration (clamped to 1 ms ..= 1 h). Fails when the homeserver
/// does not support sticky events, since it would then send an ordinary event
/// that nobody treats as membership; that failure is a [`StickyUnsupported`].
pub async fn send_sticky(
    room: &Room,
    event_type: &str,
    content: Value,
    duration: std::time::Duration,
) -> anyhow::Result<matrix_sdk::ruma::OwnedEventId> {
    if !room.client().supports_sticky_events().await.unwrap_or(false) {
        return Err(StickyUnsupported("the homeserver does not support sticky events".to_owned()).into());
    }
    let duration = clamp_sticky(duration);
    let sent = room
        .send_raw(event_type, content)
        .with_sticky_duration(duration)
        .await
        .map_err(|e| {
            let msg = format!("send sticky {event_type}: {e}");
            if refuses_sticky(e.client_api_error_kind()) {
                anyhow::Error::new(StickyUnsupported(msg))
            } else {
                anyhow::anyhow!(msg)
            }
        })?;
    Ok(sent.response.event_id)
}

/// A sticky duration the server will accept: at least 1 ms, at most one hour.
pub fn clamp_sticky(d: std::time::Duration) -> std::time::Duration {
    d.clamp(std::time::Duration::from_millis(1), std::time::Duration::from_millis(MAX_STICKY_MS))
}

/// Announce active invites as the SDK's sticky maps change: one task per
/// joined room, started when the room first shows up. Runs until aborted.
pub async fn watch_invites(client: Client) {
    let mut watched: HashSet<OwnedRoomId> = HashSet::new();
    let mut tasks = tokio::task::JoinSet::new();
    let mut updates = client.subscribe_to_all_room_updates();
    let mut start = |room: Room, watched: &mut HashSet<OwnedRoomId>| {
        if !watched.insert(room.room_id().to_owned()) {
            return;
        }
        let mut ignores = client.subscribe_to_ignore_user_list_changes();
        tasks.spawn(async move {
            use futures_util::StreamExt;
            let mut rx = room.sticky_events().subscribe();
            let mut gate = PassGate::new(INVITE_PASS_INTERVAL);
            gate.ran(Instant::now());
            super::invites::notify_active(&room).await;
            loop {
                tokio::select! {
                    got = rx.recv() => {
                        if got.is_err() && rx.is_closed() {
                            break;
                        }
                    }
                    // Invites from a newly ignored user stop ringing, and a
                    // newly unignored one's start.
                    Some(_) = ignores.next() => {}
                }
                // A burst of updates is answered by one pass at the end of
                // the interval; what piled up meanwhile is already in the map.
                let wait = gate.delay(Instant::now());
                if !wait.is_zero() {
                    tokio::time::sleep(wait).await;
                    while rx.try_recv().is_ok() {}
                }
                gate.ran(Instant::now());
                super::invites::notify_active(&room).await;
            }
        });
    };
    for room in client.joined_rooms() {
        start(room, &mut watched);
    }
    loop {
        match updates.recv().await {
            Ok(u) => {
                for id in u.joined.keys() {
                    if let Some(room) = client.get_room(id) {
                        start(room, &mut watched);
                    }
                }
            }
            Err(tokio::sync::broadcast::error::RecvError::Lagged(n)) => {
                debug!("rtc: invite watcher lagged by {n} room updates");
                for room in client.joined_rooms() {
                    start(room, &mut watched);
                }
            }
            Err(_) => break,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn member_event() -> Value {
        json!({
            "type": "org.matrix.msc4143.rtc.member",
            "event_id": "$m1",
            "sender": "@a:x",
            "origin_server_ts": 1_000_000u64,
            "content": { "sticky_key": "mid", "slot_id": "m.call#room" }
        })
    }

    #[test]
    fn the_sdk_expiry_decides_how_long_the_entry_lives() {
        let e = entry_from_parts(&member_event(), 1_300_000, true, Some("DEV".into())).unwrap();
        assert_eq!((e.start_ms, e.duration_ms, e.end_ms()), (1_000_000, 300_000, 1_300_000));
        assert!(e.was_encrypted);
        assert_eq!(e.sender_device.as_deref(), Some("DEV"));
        assert_eq!((e.sender.as_str(), e.sticky_key.as_str()), ("@a:x", "mid"));
    }

    #[test]
    fn a_future_dated_event_cannot_outlive_its_expiry() {
        let mut ev = member_event();
        ev["origin_server_ts"] = json!(9_000_000u64);
        let e = entry_from_parts(&ev, 1_300_000, false, None).unwrap();
        assert_eq!(e.end_ms(), 1_300_000);
    }

    #[test]
    fn other_event_types_are_ignored() {
        let mut ev = member_event();
        ev["type"] = json!("m.room.message");
        assert!(entry_from_parts(&ev, 2_000_000, false, None).is_none());
    }

    #[test]
    fn an_event_without_a_sticky_key_is_ignored() {
        let mut ev = member_event();
        ev["content"] = json!({"slot_id": "m.call#room"});
        assert!(entry_from_parts(&ev, 2_000_000, false, None).is_none());
    }

    #[test]
    fn key_only_content_is_a_withdrawal() {
        let mut ev = member_event();
        ev["content"] = json!({"sticky_key": "mid"});
        assert!(entry_from_parts(&ev, 2_000_000, false, None).unwrap().is_withdrawal());
        assert!(!entry_from_parts(&member_event(), 2_000_000, false, None).unwrap().is_withdrawal());
    }

    fn entry(id: &str) -> StickyEntry {
        StickyEntry {
            event_id: id.into(), sender: "@a:x".into(), event_type: MEMBER_TYPE.into(),
            sticky_key: "k".into(), start_ms: 0, duration_ms: 1000,
            was_encrypted: false, sender_device: None, content: json!({}),
        }
    }

    #[test]
    fn an_unchanged_event_is_parsed_once_across_reads() {
        let parses = std::cell::Cell::new(0);
        let parse = |id: &String| {
            parses.set(parses.get() + 1);
            Some(entry(id))
        };
        let mut cache = EntryCache::<String>::default();
        for _ in 0..3 {
            let live = (0..100).map(|i| (format!("k{i}"), format!("$e{i}"), 5_000u64, format!("$e{i}")));
            assert_eq!(cache.collect(live, parse).len(), 100);
        }
        assert_eq!(parses.get(), 100, "later reads come from the cache");
    }

    #[test]
    fn a_replaced_or_re_expired_event_is_parsed_again() {
        let parses = std::cell::Cell::new(0);
        let parse = |id: &String| {
            parses.set(parses.get() + 1);
            Some(entry(id))
        };
        let mut cache = EntryCache::<&str>::default();
        let read = |cache: &mut EntryCache<&str>, id: &str, exp: u64| {
            cache.collect([("k", id.to_owned(), exp, id.to_owned())], &parse)
        };
        assert_eq!(read(&mut cache, "$a", 10)[0].event_id, "$a");
        assert_eq!(read(&mut cache, "$a", 10)[0].event_id, "$a");
        assert_eq!(parses.get(), 1);
        // Same key, newer event: not the cached one.
        assert_eq!(read(&mut cache, "$b", 10)[0].event_id, "$b");
        assert_eq!(parses.get(), 2);
        // Same event, different expiry: parsed again.
        read(&mut cache, "$b", 20);
        assert_eq!(parses.get(), 3);
    }

    #[test]
    fn entries_that_left_the_live_set_leave_the_cache() {
        let mut cache = EntryCache::<&str>::default();
        let parse = |id: &String| Some(entry(id));
        cache.collect([("a", "$a".to_owned(), 1, "$a".to_owned()), ("b", "$b".to_owned(), 1, "$b".to_owned())], parse);
        assert!(!cache.is_empty());
        let out = cache.collect(std::iter::empty::<(&str, String, u64, String)>(), parse);
        assert!(out.is_empty());
        assert!(cache.is_empty());
    }

    #[test]
    fn an_unparsable_event_is_remembered_not_retried() {
        let parses = std::cell::Cell::new(0);
        let parse = |_: &()| {
            parses.set(parses.get() + 1);
            None
        };
        let mut cache = EntryCache::<&str>::default();
        for _ in 0..3 {
            assert!(cache.collect([("k", "$a".to_owned(), 1, ())], parse).is_empty());
        }
        assert_eq!(parses.get(), 1);
    }

    #[test]
    fn a_burst_of_updates_gets_one_pass_per_interval() {
        let mut gate = PassGate::new(Duration::from_millis(250));
        let t0 = Instant::now();
        assert_eq!(gate.delay(t0), Duration::ZERO, "the first pass is not held back");
        gate.ran(t0);
        // An update 100 ms later waits out the rest of the interval...
        let t1 = t0 + Duration::from_millis(100);
        assert_eq!(gate.delay(t1), Duration::from_millis(150));
        // ...and one that arrives once it has passed runs at once.
        assert_eq!(gate.delay(t0 + Duration::from_millis(250)), Duration::ZERO);
        assert_eq!(gate.delay(t0 + Duration::from_secs(5)), Duration::ZERO);
        gate.ran(t0 + Duration::from_millis(250));
        assert_eq!(gate.delay(t0 + Duration::from_millis(300)), Duration::from_millis(200));
    }

    #[test]
    fn duration_is_clamped_to_one_hour() {
        let e = StickyEntry {
            event_id: "$a".into(), sender: "@a:x".into(), event_type: MEMBER_TYPE.into(),
            sticky_key: "k".into(), start_ms: 0, duration_ms: 10 * MAX_STICKY_MS,
            was_encrypted: false, sender_device: None, content: json!({}),
        };
        assert_eq!(e.end_ms(), MAX_STICKY_MS);
    }

    #[test]
    fn only_a_definite_refusal_counts_as_sticky_unsupported() {
        let refused = anyhow::Error::new(StickyUnsupported("no".into()));
        assert!(is_sticky_unsupported(&refused));
        // Context added on the way up does not hide it.
        assert!(is_sticky_unsupported(&refused.context("decline")));
        // A timeout or a server error may have stored the event: not a refusal.
        assert!(!is_sticky_unsupported(&anyhow::anyhow!("send sticky x: timed out")));
        assert!(refuses_sticky(Some(&ErrorKind::Unrecognized)));
        assert!(refuses_sticky(Some(&ErrorKind::Forbidden)));
        assert!(!refuses_sticky(Some(&ErrorKind::Unknown)));
        assert!(!refuses_sticky(None), "no homeserver answer at all");
    }

    #[test]
    fn sticky_durations_are_clamped_to_what_servers_accept() {
        use std::time::Duration;
        assert_eq!(clamp_sticky(Duration::ZERO), Duration::from_millis(1));
        assert_eq!(clamp_sticky(Duration::from_secs(90)), Duration::from_secs(90));
        assert_eq!(clamp_sticky(Duration::from_secs(10 * 3600)), Duration::from_secs(3600));
    }
}
