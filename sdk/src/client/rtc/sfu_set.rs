//! The set of LiveKit SFU connections of one call.
//!
//! In multi-SFU mode (Element Call's `compatibility`/`matrix_2_0` modes) each
//! member publishes on its own homeserver's SFU. We publish on ours (the
//! primary connection) and additionally connect, subscribe-only, to the SFU of
//! every other member so we receive their media.

use std::{
    collections::{BTreeSet, HashMap},
    sync::Arc,
    time::{Duration, Instant},
};

use parking_lot::Mutex as PLMutex;
use tracing::{info, warn};

use super::{
    livekit_room::{LiveKitRoom, MemberTransports, RemoteSfuRoom, SharedKeys},
    session::get_openid_token,
    transport::fetch_livekit_jwt,
    RtcEventSink,
};

/// How long a remote SFU stays connected after its last member left, so a
/// member rejoining (or a briefly stale state read) doesn't cause churn.
const DROP_GRACE: Duration = Duration::from_secs(10);

/// What is needed to obtain a token and connect to another SFU.
pub struct SfuContext {
    pub client: matrix_sdk::Client,
    pub http: reqwest::Client,
    pub room_id: String,
    pub slot_id: String,
    pub member_id: String,
    pub device_id: String,
    pub user_id: String,
    pub session_id: u64,
    pub sink: Option<Arc<dyn RtcEventSink>>,
    pub use_e2ee: bool,
    pub members: MemberTransports,
}

struct Remote {
    room: Arc<RemoteSfuRoom>,
    unused_since: Option<Instant>,
}

struct Retry {
    failures: u32,
    next_attempt: Instant,
}

pub struct SfuSet {
    ctx: SfuContext,
    pub primary: Arc<LiveKitRoom>,
    keys: SharedKeys,
    remotes: PLMutex<HashMap<String, Remote>>,
    retries: PLMutex<HashMap<String, Retry>>,
}

/// SFUs to connect to and SFUs no longer wanted.
pub fn diff_sfus(
    current: &BTreeSet<String>,
    desired: &BTreeSet<String>,
) -> (Vec<String>, Vec<String>) {
    (
        desired.difference(current).cloned().collect(),
        current.difference(desired).cloned().collect(),
    )
}

/// Wait before the next connection attempt after `failures` consecutive failures.
pub fn backoff(failures: u32) -> Duration {
    Duration::from_secs(match failures {
        0 | 1 => 5,
        2 => 15,
        _ => 60,
    })
}

impl SfuSet {
    pub fn new(ctx: SfuContext, primary: Arc<LiveKitRoom>, keys: SharedKeys) -> Self {
        Self {
            ctx,
            primary,
            keys,
            remotes: PLMutex::new(HashMap::new()),
            retries: PLMutex::new(HashMap::new()),
        }
    }

    /// Store a peer's frame key and apply it to every connection.
    pub fn apply_key(&self, sender_user_id: &str, index: i32, raw_key: Vec<u8>) {
        self.keys.store(sender_user_id, index, raw_key.clone());
        let mut applied = self.primary.apply_peer_key(sender_user_id, index, &raw_key);
        for remote in self.remotes.lock().values() {
            applied += remote.room.apply_peer_key(sender_user_id, index, &raw_key);
        }
        if applied == 0 {
            info!(
                "e2ee: queued key for {sender_user_id} index={index} (participant not yet connected)"
            );
        }
    }

    /// Apply every stored key to the participants of every connection.
    pub fn apply_all_keys(&self) {
        self.primary.apply_all_peer_keys();
        for remote in self.remotes.lock().values() {
            remote.room.apply_all_peer_keys();
        }
    }

    /// Connect to newly needed SFUs and drop the ones nobody publishes on.
    pub async fn reconcile(&self, desired: &BTreeSet<String>) {
        let (to_connect, to_drop) = {
            let mut remotes = self.remotes.lock();
            let current: BTreeSet<String> = remotes.keys().cloned().collect();
            let (to_connect, to_drop) = diff_sfus(&current, desired);
            for (url, remote) in remotes.iter_mut() {
                if desired.contains(url) {
                    remote.unused_since = None;
                }
            }
            (to_connect, to_drop)
        };

        let mut dropped = Vec::new();
        {
            let mut remotes = self.remotes.lock();
            for url in to_drop {
                let Some(remote) = remotes.get_mut(&url) else { continue };
                let since = *remote.unused_since.get_or_insert_with(Instant::now);
                if since.elapsed() >= DROP_GRACE {
                    if let Some(r) = remotes.remove(&url) {
                        dropped.push((url, r.room));
                    }
                }
            }
        }
        for (url, room) in dropped {
            info!("rtc: dropping SFU {url}: no members publish there any more");
            room.disconnect().await;
        }

        for url in to_connect {
            self.connect_remote(&url).await;
        }
    }

    async fn connect_remote(&self, service_url: &str) {
        {
            let retries = self.retries.lock();
            if retries.get(service_url).is_some_and(|r| Instant::now() < r.next_attempt) {
                return;
            }
        }
        info!("rtc: connecting to remote SFU {service_url}");
        match self.try_connect_remote(service_url).await {
            Ok(room) => {
                self.retries.lock().remove(service_url);
                room.apply_all_peer_keys();
                self.remotes.lock().insert(
                    service_url.to_owned(),
                    Remote { room: Arc::new(room), unused_since: None },
                );
            }
            Err(e) => {
                let mut retries = self.retries.lock();
                let failures = retries.get(service_url).map_or(0, |r| r.failures) + 1;
                let wait = backoff(failures);
                warn!(
                    "rtc: connecting to remote SFU {service_url} failed (attempt {failures}, \
                     retry in {wait:?}): {e:#}"
                );
                retries.insert(
                    service_url.to_owned(),
                    Retry { failures, next_attempt: Instant::now() + wait },
                );
            }
        }
    }

    async fn try_connect_remote(&self, service_url: &str) -> anyhow::Result<RemoteSfuRoom> {
        let c = &self.ctx;
        let openid = get_openid_token(&c.client).await?;
        let transport = fetch_livekit_jwt(
            &c.http,
            service_url,
            &c.room_id,
            &c.slot_id,
            &openid.access_token,
            openid.expires_in,
            &openid.matrix_server_name,
            &c.member_id,
            &c.device_id,
            &c.user_id,
        )
        .await?;
        RemoteSfuRoom::connect(
            &transport.server_url,
            &transport.jwt,
            c.session_id,
            c.sink.clone(),
            c.use_e2ee,
            self.keys.clone(),
            service_url,
            c.members.clone(),
        )
        .await
    }

    /// Leave every SFU, concurrently, without waiting longer than a few
    /// seconds (this runs on the UI thread's hang-up call).
    pub async fn disconnect_all(&self) {
        let remotes: Vec<Arc<RemoteSfuRoom>> = self
            .remotes
            .lock()
            .drain()
            .map(|(_, r)| r.room)
            .collect();
        let primary = Arc::clone(&self.primary);
        let all = async move {
            let mut tasks = vec![tokio::spawn(async move { primary.disconnect().await })];
            for r in remotes {
                tasks.push(tokio::spawn(async move { r.disconnect().await }));
            }
            for t in tasks {
                let _ = t.await;
            }
        };
        if tokio::time::timeout(Duration::from_secs(3), all).await.is_err() {
            warn!("rtc: timed out disconnecting from SFUs");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set(items: &[&str]) -> BTreeSet<String> {
        items.iter().map(|s| (*s).to_owned()).collect()
    }

    #[test]
    fn diff_connects_new_and_drops_stale() {
        let (c, d) = diff_sfus(&set(&["a", "b"]), &set(&["b", "c"]));
        assert_eq!(c, vec!["c".to_owned()]);
        assert_eq!(d, vec!["a".to_owned()]);
    }

    #[test]
    fn diff_of_equal_sets_is_empty() {
        let (c, d) = diff_sfus(&set(&["a"]), &set(&["a"]));
        assert!(c.is_empty() && d.is_empty());
    }

    #[test]
    fn backoff_grows_then_caps() {
        assert_eq!(backoff(1), Duration::from_secs(5));
        assert_eq!(backoff(2), Duration::from_secs(15));
        assert_eq!(backoff(3), Duration::from_secs(60));
        assert_eq!(backoff(50), Duration::from_secs(60));
    }
}
