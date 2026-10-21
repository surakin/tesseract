//! Sticky MatrixRTC events (MSC4354), read from the SDK's per-room sticky map
//! (`Room::sticky_events()`), which the sliding-sync sticky extension and the
//! timeline both feed. The SDK keeps the live entry per `(sender, type,
//! sticky_key)`, expires them and retries decryption, so all that is left here
//! is turning its entries into the plain [`StickyEntry`] the slot and invite
//! logic works on.

use std::collections::HashSet;

use matrix_sdk_base::sticky::StickyEvent;
use matrix_sdk::{
    ruma::{OwnedRoomId, UInt},
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

/// The live tracked sticky events of a room.
pub fn live_entries(room: &Room) -> Vec<StickyEntry> {
    room.sticky_events()
        .live()
        .iter()
        .filter_map(entry_from_sticky_event)
        .collect()
}

/// Send a sticky message-like event: encrypted in encrypted rooms, with the
/// given sticky duration (clamped to 1 ms ..= 1 h). Fails when the homeserver
/// does not support sticky events, since it would then send an ordinary event
/// that nobody treats as membership.
pub async fn send_sticky(
    room: &Room,
    event_type: &str,
    content: Value,
    duration: std::time::Duration,
) -> anyhow::Result<matrix_sdk::ruma::OwnedEventId> {
    if !room.client().supports_sticky_events().await.unwrap_or(false) {
        anyhow::bail!("the homeserver does not support sticky events");
    }
    let duration = clamp_sticky(duration);
    let sent = room
        .send_raw(event_type, content)
        .with_sticky_duration(duration)
        .await
        .map_err(|e| anyhow::anyhow!("send sticky {event_type}: {e}"))?;
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
        tasks.spawn(async move {
            let mut rx = room.sticky_events().subscribe();
            super::invites::notify_active(&room).await;
            while rx.recv().await.is_ok() || !rx.is_closed() {
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
    fn sticky_durations_are_clamped_to_what_servers_accept() {
        use std::time::Duration;
        assert_eq!(clamp_sticky(Duration::ZERO), Duration::from_millis(1));
        assert_eq!(clamp_sticky(Duration::from_secs(90)), Duration::from_secs(90));
        assert_eq!(clamp_sticky(Duration::from_secs(10 * 3600)), Duration::from_secs(3600));
    }
}
