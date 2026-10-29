use anyhow::Context;
use base64ct::{Base64, Encoding};
use hkdf::Hkdf;
use rand::RngCore;
use sha2::Sha256;
use std::collections::HashMap;
use zeroize::Zeroize;

/// 32-byte raw key material (pre-HKDF).
pub type KeyMaterial = [u8; 32];

/// 16-byte AES-128-GCM frame key derived via HKDF.
pub type FrameKey = [u8; 16];

/// E2EE key state for one active call session.
pub struct E2eeManager {
    own_key: KeyMaterial,
    own_index: u8,
    /// member_id → (rotation_index → frame_key)
    peer_keys: HashMap<String, HashMap<u8, FrameKey>>,
}

// The stored key is wiped when the manager goes away. Copies handed out by
// `own_raw_key` / `own_key_b64` are the caller's, and are not wiped here.
impl Drop for E2eeManager {
    fn drop(&mut self) {
        self.own_key.zeroize();
    }
}

impl E2eeManager {
    pub fn new() -> Self {
        Self {
            own_key: Self::generate_key(),
            own_index: 0,
            peer_keys: HashMap::new(),
        }
    }

    fn generate_key() -> KeyMaterial {
        let mut key = [0u8; 32];
        rand::thread_rng().fill_bytes(&mut key);
        key
    }

    pub fn own_index(&self) -> u8 {
        self.own_index
    }

    pub fn own_key_b64(&self) -> String {
        Base64::encode_string(&self.own_key)
    }

    pub fn own_raw_key(&self) -> &KeyMaterial {
        &self.own_key
    }

    /// Derive the 16-byte frame key for our own outgoing track.
    pub fn own_frame_key(&self, member_id: &str) -> FrameKey {
        Self::derive_frame_key(&self.own_key, member_id, self.own_index)
    }

    /// Generate a new key and increment the rotation index.
    /// Returns `(new_index, base64_key_material)`.
    /// The caller is responsible for the 5-second delay before activating the
    /// new key in the FrameCryptor (coalesced by the session's debounce timer).
    pub fn rotate(&mut self) -> (u8, String) {
        self.own_key.zeroize();
        self.own_key = Self::generate_key();
        self.own_index = self.own_index.wrapping_add(1);
        (self.own_index, self.own_key_b64())
    }

    /// Store a peer key received via m.rtc.encryption_key to-device.
    /// Returns the derived frame key for immediate FrameCryptor registration.
    pub fn receive_peer_key(
        &mut self,
        member_id: &str,
        index: u8,
        key_b64: &str,
    ) -> anyhow::Result<FrameKey> {
        let raw = Base64::decode_vec(key_b64).context("decode peer key base64")?;
        let material: KeyMaterial = raw
            .try_into()
            .map_err(|_| anyhow::anyhow!("peer key must be exactly 32 bytes"))?;
        let frame_key = Self::derive_frame_key(&material, member_id, index);
        self.peer_keys
            .entry(member_id.to_owned())
            .or_default()
            .insert(index, frame_key);
        Ok(frame_key)
    }

    /// HKDF-SHA256: ikm=key_material, salt=b"livekit_frame", info=member_id||[index].
    fn derive_frame_key(material: &KeyMaterial, member_id: &str, index: u8) -> FrameKey {
        let hk = Hkdf::<Sha256>::new(Some(b"livekit_frame"), material);
        let mut info = member_id.as_bytes().to_vec();
        info.push(index);
        let mut okm = [0u8; 16];
        hk.expand(&info, &mut okm)
            .expect("HKDF expand to 16 bytes always succeeds");
        okm
    }
}

/// How long a key from a not-yet-known call member is kept waiting for its
/// membership event (MSC4143: clients SHOULD store keys that arrive first).
pub const PARK_TTL: std::time::Duration = std::time::Duration::from_secs(60);
/// Most keys kept waiting for a membership event at once; the most prolific
/// sender loses its oldest first.
pub const PARK_CAP: usize = 64;
/// Most keys kept waiting per sending device, so one sender cannot push out the rest.
pub const PER_SENDER_CAP: usize = 8;
/// Most events kept waiting for their sending device to become known, in a
/// budget of their own: they can never push out a key waiting for a membership.
pub const RECHECK_CAP: usize = 32;
/// Most of those per user. A joined member can mint any number of Olm identities
/// that never upload device keys, so the count is per user, not per device.
pub const PER_USER_RECHECK_CAP: usize = 8;

/// A frame key whose sender is not a live call member (yet), or whose sending
/// device is not known (yet).
#[derive(Clone, Debug)]
pub struct ParkedKey {
    pub user_id: String,
    pub device_id: String,
    pub index: i32,
    pub raw_key: Vec<u8>,
    /// Set for a key whose sending device is not known yet: the event as it
    /// arrived, judged again once the device is. `device_id` is then
    /// [`recheck_device_id`] and `index` / `raw_key` are unused.
    pub recheck: Option<Recheck>,
}

/// A key event that could not be judged yet, kept whole.
#[derive(Clone, Debug)]
pub struct Recheck {
    pub content: super::model::MediaKeyContent,
    pub info: matrix_sdk::deserialized_responses::EncryptionInfo,
}

/// Stands in for the device id of a key whose sending device is unknown. It only
/// tells such a key from a member's key; the caps count rechecks per user.
pub fn recheck_device_id(curve25519_key_b64: &str) -> String {
    format!("?curve25519:{curve25519_key_b64}")
}

type Parked = Vec<(std::time::Instant, ParkedKey)>;

/// Index of the oldest entry of `group` (ties: the first in the list).
fn oldest_of(entries: &Parked, group: impl Fn(&ParkedKey) -> bool) -> Option<usize> {
    entries
        .iter()
        .enumerate()
        .filter(|(_, (_, k))| group(k))
        .min_by_key(|(i, (at, _))| (*at, *i))
        .map(|(i, _)| i)
}

/// Drop the oldest entry of whoever holds the most (`owner` names the holder).
/// Ties between holders go to the one whose entry is the oldest, so the same
/// input always evicts the same entry.
fn evict_from_biggest_holder(entries: &mut Parked, owner: impl Fn(&ParkedKey) -> (&str, &str)) {
    let mut counts: std::collections::BTreeMap<(&str, &str), usize> = Default::default();
    for (_, k) in entries.iter() {
        *counts.entry(owner(k)).or_default() += 1;
    }
    let Some(most) = counts.values().max().copied() else { return };
    let victim = entries
        .iter()
        .enumerate()
        .filter(|(_, (_, k))| counts[&owner(k)] == most)
        .min_by_key(|(i, (at, _))| (*at, *i))
        .map(|(i, _)| i);
    if let Some(i) = victim {
        entries.remove(i);
    }
}

/// Keys that arrived before their sender's membership event, and events whose
/// sending device was not known yet. The two have separate budgets.
#[derive(Default)]
pub struct ParkedKeys {
    entries: Parked,
    rechecks: Parked,
}

impl ParkedKeys {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn park(&mut self, key: ParkedKey, now: std::time::Instant) {
        if key.recheck.is_some() {
            self.park_recheck(key, now);
            return;
        }
        self.entries.retain(|(at, _)| now.saturating_duration_since(*at) <= PARK_TTL);
        let same = |k: &ParkedKey| k.user_id == key.user_id && k.device_id == key.device_id;
        // Per sender: drop its oldest key.
        if self.entries.iter().filter(|(_, k)| same(k)).count() >= PER_SENDER_CAP {
            if let Some(i) = oldest_of(&self.entries, same) {
                self.entries.remove(i);
            }
        }
        // Overall: drop the oldest key of whoever holds the most.
        if self.entries.len() >= PARK_CAP {
            evict_from_biggest_holder(&mut self.entries, |k| (k.user_id.as_str(), k.device_id.as_str()));
        }
        self.entries.push((now, key));
    }

    /// `at` is when the event first arrived (a re-parked one keeps its time).
    fn park_recheck(&mut self, key: ParkedKey, at: std::time::Instant) {
        self.rechecks.retain(|(t, _)| at.saturating_duration_since(*t) <= PARK_TTL);
        let same_user = |k: &ParkedKey| k.user_id == key.user_id;
        if self.rechecks.iter().filter(|(_, k)| same_user(k)).count() >= PER_USER_RECHECK_CAP {
            if let Some(i) = oldest_of(&self.rechecks, same_user) {
                self.rechecks.remove(i);
            }
        }
        if self.rechecks.len() >= RECHECK_CAP {
            evict_from_biggest_holder(&mut self.rechecks, |k| (k.user_id.as_str(), ""));
        }
        self.rechecks.push((at, key));
    }

    /// Remove and return the parked keys whose sender is in `members`
    /// (`(user_id, device_id)`); expired ones are dropped. Keys waiting for
    /// their sending device to become known are not members' keys yet and stay.
    pub fn take_for(&mut self, members: &[(String, String)], now: std::time::Instant) -> Vec<ParkedKey> {
        self.entries.retain(|(at, _)| now.saturating_duration_since(*at) <= PARK_TTL);
        let mut released = Vec::new();
        self.entries.retain(|(_, k)| {
            let member = members.iter().any(|(u, d)| *u == k.user_id && *d == k.device_id);
            if member {
                released.push(k.clone());
            }
            !member
        });
        released
    }

    /// Remove and return the keys waiting for their sending device to become
    /// known, each with the time it was parked; expired ones are dropped. A key
    /// that is still not decided goes back with [`Self::park`] and that time, so
    /// waiting does not extend its life.
    pub fn take_rechecks(&mut self, now: std::time::Instant) -> Vec<(std::time::Instant, ParkedKey)> {
        let mut out = std::mem::take(&mut self.rechecks);
        out.retain(|(at, _)| now.saturating_duration_since(*at) <= PARK_TTL);
        out
    }
}

/// At most one `true` per key and interval: for log lines and queries that
/// anyone who can send us an event could otherwise trigger at will.
pub struct RateLimiter {
    interval: std::time::Duration,
    cap: usize,
    last: std::collections::HashMap<String, std::time::Instant>,
}

impl RateLimiter {
    /// `cap` bounds the keys remembered; past it, new keys are refused until
    /// old ones have aged out.
    pub fn new(interval: std::time::Duration, cap: usize) -> Self {
        Self { interval, cap, last: Default::default() }
    }

    pub fn allow(&mut self, key: &str, now: std::time::Instant) -> bool {
        match self.last.get(key) {
            Some(t) if now.saturating_duration_since(*t) < self.interval => return false,
            Some(_) => {}
            None if self.last.len() >= self.cap => {
                let interval = self.interval;
                self.last.retain(|_, t| now.saturating_duration_since(*t) < interval);
                if self.last.len() >= self.cap {
                    return false;
                }
            }
            None => {}
        }
        self.last.insert(key.to_owned(), now);
        true
    }
}

/// The to-device content of a frame key: Element's `keys`/`member`/`session`
/// shape, plus MSC4143's `member_id`/`media_key` when we have a MatrixRTC
/// member id (the reference implementation sends both).
pub fn key_event_content(
    room_id: &str,
    lk_identity: &str,
    our_device_id: &str,
    key_b64: &str,
    index: u8,
    mrtc_member_id: Option<&str>,
    now_ms: u64,
) -> serde_json::Value {
    let mut content = serde_json::json!({
        "keys": { "key": key_b64, "index": index },
        "room_id": room_id,
        "member": { "claimed_device_id": our_device_id, "id": lk_identity },
        "session": { "call_id": "", "application": "m.call", "scope": "m.room" },
        "sent_ts": now_ms,
    });
    if let Some(id) = mrtc_member_id {
        content["member_id"] = serde_json::Value::String(id.to_owned());
        content["media_key"] = serde_json::json!({ "key": key_b64, "index": index });
    }
    content
}

/// How one send of a key to one recipient device ended.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Delivery {
    Delivered,
    /// We know no such device of the user yet (after asking the homeserver);
    /// it may show up, so this is worth another try.
    NoDevice,
    /// Our policy keeps the key from this device (it is not cross-signed by its
    /// owner and we exclude those); trying again changes nothing.
    Withheld(String),
    Failed(String),
}

/// The outcome of a send to `matching` devices of which `failed` did not get
/// the message (withheld, or the request failed) and `withheld` of those never
/// will, by our own policy.
pub fn delivery_outcome(matching: usize, failed: usize, withheld: usize) -> Delivery {
    if matching == 0 {
        Delivery::NoDevice
    } else if failed == 0 || (failed == withheld && matching > failed) {
        // The devices that can get the key did; the rest never will.
        Delivery::Delivered
    } else if failed == withheld {
        Delivery::Withheld(format!("{withheld} of {matching} device(s) are not trusted to get the key"))
    } else {
        Delivery::Failed(format!("{failed} of {matching} device(s) did not get the key"))
    }
}

/// Whether a device that did not get the key is one our policy keeps it from for
/// good: with insecure devices excluded, one its owner did not cross-sign.
pub fn withheld_by_policy(exclude_insecure: bool, cross_signed_by_owner: bool) -> bool {
    exclude_insecure && !cross_signed_by_owner
}

/// Send a frame key to one device of a Matrix user (or to all of them when
/// `device_filter` is `None`) and report how it went.
///
/// A device the crypto store does not know is looked up with a `/keys/query`
/// for the user first (the SDK's identity request, which also stores the
/// devices it learns about) when `may_query` allows it; when it is still
/// missing the answer is [`Delivery::NoDevice`], not success.
///
/// Uses the `io.element.call.encryption_keys` event format (Element Call wire
/// protocol), validated against Element Call 0.20.1.
pub async fn send_frame_key_to_user(
    client: &matrix_sdk::Client,
    matrix_user_id: &str,
    device_filter: Option<&str>,
    room_id: &str,
    lk_identity: &str,
    key_b64: &str,
    index: u8,
    mrtc_member_id: Option<&str>,
    may_query: &(dyn Fn() -> bool + Sync),
) -> Delivery {
    use matrix_sdk::encryption::identities::Device;
    use matrix_sdk::ruma::{events::AnyToDeviceEventContent, UserId};

    let Ok(uid) = <&UserId>::try_from(matrix_user_id) else {
        return Delivery::Failed("invalid matrix user ID".to_owned());
    };
    let Some(our_device_id) = client.device_id().map(|d| d.to_string()) else {
        return Delivery::Failed("device_id not available".to_owned());
    };

    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64;

    let content = key_event_content(
        room_id,
        lk_identity,
        &our_device_id,
        key_b64,
        index,
        mrtc_member_id,
        now_ms,
    );
    let raw_content: matrix_sdk::ruma::serde::Raw<AnyToDeviceEventContent> =
        match serde_json::value::to_raw_value(&content) {
            Ok(raw) => matrix_sdk::ruma::serde::Raw::from_json(raw),
            Err(e) => return Delivery::Failed(format!("serialize encryption key: {e}")),
        };

    // Try the crypto store first; ask the homeserver when the device is not in it.
    let matching = |ud: matrix_sdk::encryption::identities::UserDevices| -> Vec<Device> {
        ud.devices()
            .filter(|d| device_filter.map_or(true, |f| d.device_id().as_str() == f))
            .collect()
    };
    let mut devices = match client.encryption().get_user_devices(uid).await {
        Ok(ud) => matching(ud),
        Err(e) => return Delivery::Failed(format!("get_user_devices: {e}")),
    };
    if devices.is_empty() {
        // A member whose device is gone costs one query per interval, not one per attempt.
        if !may_query() {
            tracing::debug!("e2ee: no cached device for {matrix_user_id} ({device_filter:?}); asked recently");
            return delivery_outcome(0, 0, 0);
        }
        tracing::info!("e2ee: no cached device for {matrix_user_id} ({device_filter:?}); querying homeserver");
        if let Err(e) = client.encryption().request_user_identity(uid).await {
            tracing::debug!("e2ee: /keys/query for {matrix_user_id} failed: {e}");
        }
        devices = match client.encryption().get_user_devices(uid).await {
            Ok(ud) => matching(ud),
            Err(e) => return Delivery::Failed(format!("get_user_devices after query: {e}")),
        };
    }
    if devices.is_empty() {
        tracing::debug!("e2ee: no matching device for {matrix_user_id} ({device_filter:?}) yet");
        return delivery_outcome(0, 0, 0);
    }

    tracing::info!(
        "e2ee: sending frame key index={index} to {matrix_user_id} ({} device(s))",
        devices.len()
    );
    let refs: Vec<&Device> = devices.iter().collect();
    let failures = match client
        .encryption()
        .encrypt_and_send_raw_to_device(
            refs,
            "io.element.call.encryption_keys",
            raw_content,
            crate::client::identity::room_key_recipient_strategy(
                crate::client::identity::exclude_insecure_devices(),
                crate::client::identity::block_sends_on_unverified_devices(),
            ),
        )
        .await
    {
        Ok(f) => f,
        Err(e) => return Delivery::Failed(format!("send Olm-encrypted to-device key: {e}")),
    };
    if !failures.is_empty() {
        tracing::warn!(
            "e2ee: key delivery to {matrix_user_id} had {} failure(s): {failures:?}",
            failures.len()
        );
    }
    let exclude_insecure = crate::client::identity::exclude_insecure_devices();
    let withheld = failures
        .iter()
        .filter(|(_, id)| {
            devices
                .iter()
                .find(|d| d.device_id() == id)
                .is_some_and(|d| withheld_by_policy(exclude_insecure, d.is_cross_signed_by_owner()))
        })
        .count();
    delivery_outcome(devices.len(), failures.len(), withheld)
}

// ---------------------------------------------------------------------------
// Key delivery: who still has to get which key
// ---------------------------------------------------------------------------

/// A `(user_id, device_id)` of a live call member.
pub type Recipient = (String, String);

/// Waits before the 2nd, 3rd, ... attempt to deliver a key. After the last one
/// fails the recipient is called unreachable and the key is tried again, with
/// this schedule from the start, every [`GIVEN_UP_RETRY`] for as long as the
/// recipient is a live call member.
pub const RETRY_BACKOFF: [std::time::Duration; 4] = [
    std::time::Duration::from_secs(2),
    std::time::Duration::from_secs(5),
    std::time::Duration::from_secs(15),
    std::time::Duration::from_secs(30),
];
/// How long a recipient that did not take the key through the whole backoff is
/// left alone before the next round of attempts (a sender whose network was down
/// for a minute must not lose its keys for good).
pub const GIVEN_UP_RETRY: std::time::Duration = std::time::Duration::from_secs(60);
/// One send (device lookup, Olm session set-up, request) may take this long.
pub const SEND_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(10);
/// Recipients served at once.
pub const MAX_CONCURRENT_SENDS: usize = 8;

/// One key owed to one recipient.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeliveryJob {
    pub to: Recipient,
    pub index: u8,
    pub key_b64: String,
    /// Which enqueue this belongs to; a result for a replaced entry is stale.
    seq: u64,
}

#[derive(Debug)]
struct Owed {
    key_b64: String,
    seq: u64,
    /// Attempts that did not deliver in the current round.
    failures: usize,
    /// Rounds of attempts that all failed.
    rounds: u32,
    next_try: std::time::Instant,
}

/// What [`PendingDeliveries::record`] did with a result.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Recorded {
    Done,
    RetryIn(std::time::Duration),
    /// A round of attempts ended without success (`rounds` of them so far); the
    /// key stays owed and the next round starts after [`GIVEN_UP_RETRY`].
    GaveUp { rounds: u32 },
    /// It will never be delivered (withheld by our policy): no more tries.
    Final,
    /// The entry was replaced or dropped meanwhile.
    Stale,
}

/// The keys that are not delivered yet, with their retry schedule. Pure state:
/// callers pass the clock in and run the sends it hands out.
///
/// Sends to one user never overlap: a user with a send in flight is handed
/// nothing new until it ends, and what is due for a user goes out oldest key
/// first, so a retry of key N and a send of key N+1 cannot arrive swapped.
#[derive(Default)]
pub struct PendingDeliveries {
    owed: std::collections::HashMap<(Recipient, u8), Owed>,
    seq: u64,
    /// Jobs handed out and not yet recorded, per user.
    busy: std::collections::HashMap<String, usize>,
    /// Recipients whose last round of attempts failed completely, until one
    /// key reaches them. A key owed to them does not hold anything back.
    unreachable: std::collections::HashSet<Recipient>,
}

impl PendingDeliveries {
    pub fn new() -> Self {
        Self::default()
    }

    /// Owe `index` to `to`, due now. A key already owed for the same recipient
    /// and index is replaced (a new session starts from scratch); a send of the
    /// replaced one that is still running is stale when it ends.
    pub fn enqueue(&mut self, to: &[Recipient], index: u8, key_b64: &str, now: std::time::Instant) {
        for r in to {
            self.seq += 1;
            self.owed.insert(
                (r.clone(), index),
                Owed { key_b64: key_b64.to_owned(), seq: self.seq, failures: 0, rounds: 0, next_try: now },
            );
        }
    }

    /// The sends that are due, for users with nothing in flight; they count as
    /// in flight until recorded. A user's jobs come together, oldest key first.
    pub fn take_due(&mut self, now: std::time::Instant) -> Vec<DeliveryJob> {
        let busy = &self.busy;
        let mut jobs: Vec<DeliveryJob> = self
            .owed
            .iter()
            .filter(|((to, _), o)| o.next_try <= now && !busy.contains_key(&to.0))
            .map(|((to, index), o)| DeliveryJob { to: to.clone(), index: *index, key_b64: o.key_b64.clone(), seq: o.seq })
            .collect();
        jobs.sort_by(|a, b| (&a.to.0, a.seq, &a.to.1).cmp(&(&b.to.0, b.seq, &b.to.1)));
        for job in &jobs {
            *self.busy.entry(job.to.0.clone()).or_default() += 1;
        }
        jobs
    }

    /// Book the outcome of a send.
    pub fn record(&mut self, job: &DeliveryJob, outcome: &Delivery, now: std::time::Instant) -> Recorded {
        if let Some(n) = self.busy.get_mut(&job.to.0) {
            *n = n.saturating_sub(1);
            if *n == 0 {
                self.busy.remove(&job.to.0);
            }
        }
        let key = (job.to.clone(), job.index);
        let Some(o) = self.owed.get_mut(&key).filter(|o| o.seq == job.seq) else {
            return Recorded::Stale;
        };
        match outcome {
            Delivery::Delivered => {
                self.owed.remove(&key);
                self.unreachable.remove(&job.to);
                Recorded::Done
            }
            Delivery::Withheld(_) => {
                self.owed.remove(&key);
                Recorded::Final
            }
            Delivery::NoDevice | Delivery::Failed(_) => {
                o.failures += 1;
                match RETRY_BACKOFF.get(o.failures - 1) {
                    Some(wait) => {
                        o.next_try = now + *wait;
                        Recorded::RetryIn(*wait)
                    }
                    None => {
                        o.rounds += 1;
                        o.failures = 0;
                        o.next_try = now + GIVEN_UP_RETRY;
                        let rounds = o.rounds;
                        self.unreachable.insert(job.to.clone());
                        Recorded::GaveUp { rounds }
                    }
                }
            }
        }
    }

    /// Stop owing anything to devices that are no longer call members.
    pub fn retain_recipients(&mut self, live: &std::collections::HashSet<Recipient>) {
        self.owed.retain(|(to, _), _| live.contains(to));
        self.unreachable.retain(|to| live.contains(to));
    }

    /// Stop owing keys that are no longer in use or coming (`live` are the
    /// indices of the key in use and of the one about to be).
    pub fn retain_indices(&mut self, live: &[u8]) {
        self.owed.retain(|(_, index), _| live.contains(index));
    }

    /// When the next retry falls due (in-flight sends report on their own, and
    /// a user with one is served when it ends).
    pub fn next_due(&self) -> Option<std::time::Instant> {
        self.owed
            .iter()
            .filter(|((to, _), _)| !self.busy.contains_key(&to.0))
            .map(|(_, o)| o.next_try)
            .min()
    }

    /// Whether key `index` is settled: delivered to every recipient, or owed
    /// only to those that cannot be reached (or never get it by our policy). A
    /// key is not put to use before.
    pub fn settled(&self, index: u8) -> bool {
        self.owed.keys().all(|(to, i)| *i != index || self.unreachable.contains(to))
    }

    pub fn is_empty(&self) -> bool {
        self.owed.is_empty()
    }
}

/// How a key reaches a recipient; the client in production, a fake in tests.
#[async_trait::async_trait]
pub trait KeySender: Send + Sync + 'static {
    async fn send(&self, to: &Recipient, index: u8, key_b64: &str) -> Delivery;
}

/// Users whose unknown devices we ask the homeserver about at most this often.
const DEVICE_QUERY_INTERVAL: std::time::Duration = std::time::Duration::from_secs(30);
const DEVICE_QUERY_USERS: usize = 256;

/// Sends through the SDK, on the call's room and LiveKit identity.
pub struct ClientKeySender {
    pub client: matrix_sdk::Client,
    pub room_id: String,
    pub lk_identity: String,
    pub mrtc_member_id: Option<String>,
    device_queries: parking_lot::Mutex<RateLimiter>,
}

impl ClientKeySender {
    pub fn new(client: matrix_sdk::Client, room_id: String, lk_identity: String, mrtc_member_id: Option<String>) -> Self {
        Self {
            client,
            room_id,
            lk_identity,
            mrtc_member_id,
            device_queries: parking_lot::Mutex::new(RateLimiter::new(DEVICE_QUERY_INTERVAL, DEVICE_QUERY_USERS)),
        }
    }
}

#[async_trait::async_trait]
impl KeySender for ClientKeySender {
    async fn send(&self, to: &Recipient, index: u8, key_b64: &str) -> Delivery {
        send_frame_key_to_user(
            &self.client,
            &to.0,
            Some(&to.1),
            &self.room_id,
            &self.lk_identity,
            key_b64,
            index,
            self.mrtc_member_id.as_deref(),
            &|| self.device_queries.lock().allow(&to.0, clock_now()),
        )
        .await
    }
}

/// The tokio clock as a std instant, so tests can pause it.
fn clock_now() -> std::time::Instant {
    tokio::time::Instant::now().into_std()
}

struct DeliveryShared {
    pending: parking_lot::Mutex<PendingDeliveries>,
    /// Something was enqueued (or dropped): the worker looks again.
    wake: tokio::sync::Notify,
    /// A send ended: whoever waits on [`PendingDeliveries::settled`] looks again.
    progress: tokio::sync::Notify,
}

/// Key delivery off the caller's path: [`Self::enqueue`] returns at once, a
/// worker ([`Self::spawn`]) sends to the recipients concurrently, each send
/// under a timeout, and retries what did not arrive on a backoff.
#[derive(Clone)]
pub struct KeyDelivery {
    shared: std::sync::Arc<DeliveryShared>,
}

impl KeyDelivery {
    pub fn new() -> Self {
        Self {
            shared: std::sync::Arc::new(DeliveryShared {
                pending: parking_lot::Mutex::new(PendingDeliveries::new()),
                wake: tokio::sync::Notify::new(),
                progress: tokio::sync::Notify::new(),
            }),
        }
    }

    pub fn enqueue(&self, to: &[Recipient], index: u8, key_b64: &str) {
        if to.is_empty() {
            return;
        }
        self.shared.pending.lock().enqueue(to, index, key_b64, clock_now());
        self.shared.wake.notify_one();
    }

    pub fn retain_recipients(&self, live: &std::collections::HashSet<Recipient>) {
        self.shared.pending.lock().retain_recipients(live);
    }

    pub fn retain_indices(&self, live: &[u8]) {
        self.shared.pending.lock().retain_indices(live);
    }

    pub fn settled(&self, index: u8) -> bool {
        self.shared.pending.lock().settled(index)
    }

    /// Resolves when a send has ended since the last call.
    pub async fn progressed(&self) {
        self.shared.progress.notified().await;
    }

    /// Start the worker; it stops (with every send in progress) when the
    /// returned task is aborted.
    pub fn spawn(&self, sender: std::sync::Arc<dyn KeySender>) -> tokio::task::AbortHandle {
        tokio::spawn(delivery_worker(std::sync::Arc::clone(&self.shared), sender)).abort_handle()
    }
}

/// One user's jobs while their task runs. Whatever the task did not book when
/// it ends, by a panic or by being dropped, is booked as failed, so the user is
/// served again and no entry stays in flight for good.
struct Outstanding {
    shared: std::sync::Arc<DeliveryShared>,
    jobs: Vec<DeliveryJob>,
    done: usize,
}

impl Drop for Outstanding {
    fn drop(&mut self) {
        if self.done == self.jobs.len() {
            return;
        }
        let mut pending = self.shared.pending.lock();
        for job in &self.jobs[self.done..] {
            pending.record(job, &Delivery::Failed("send interrupted".to_owned()), clock_now());
        }
        drop(pending);
        self.shared.progress.notify_one();
        self.shared.wake.notify_one();
    }
}

async fn delivery_worker(shared: std::sync::Arc<DeliveryShared>, sender: std::sync::Arc<dyn KeySender>) {
    use std::sync::Arc;
    // Dropped with this task, which aborts the sends still running.
    let mut sends = tokio::task::JoinSet::new();
    let permits = Arc::new(tokio::sync::Semaphore::new(MAX_CONCURRENT_SENDS));
    loop {
        let (jobs, next) = {
            let mut p = shared.pending.lock();
            let jobs = p.take_due(clock_now());
            (jobs, p.next_due())
        };
        // One user's devices go one after the other (they share an Olm
        // session set-up), oldest key first; different users go concurrently.
        let mut by_user: std::collections::BTreeMap<String, Vec<DeliveryJob>> = Default::default();
        for job in jobs {
            by_user.entry(job.to.0.clone()).or_default().push(job);
        }
        for (_, group) in by_user {
            let shared = Arc::clone(&shared);
            let sender = Arc::clone(&sender);
            let permits = Arc::clone(&permits);
            sends.spawn(async move {
                let mut group = Outstanding { shared, jobs: group, done: 0 };
                let Ok(_permit) = permits.acquire().await else { return };
                while let Some(job) = group.jobs.get(group.done).cloned() {
                    let outcome = tokio::time::timeout(SEND_TIMEOUT, sender.send(&job.to, job.index, &job.key_b64))
                        .await
                        .unwrap_or_else(|_| Delivery::Failed("timed out".to_owned()));
                    let recorded = group.shared.pending.lock().record(&job, &outcome, clock_now());
                    group.done += 1;
                    match recorded {
                        Recorded::GaveUp { rounds: 1 } => tracing::warn!(
                            "e2ee: key index={} for {}/{} not delivered after {} attempts ({outcome:?}); trying again every {GIVEN_UP_RETRY:?}",
                            job.index, job.to.0, job.to.1, RETRY_BACKOFF.len() + 1
                        ),
                        Recorded::GaveUp { .. } => tracing::debug!(
                            "e2ee: key index={} for {}/{} still not delivered ({outcome:?})",
                            job.index, job.to.0, job.to.1
                        ),
                        Recorded::Final => tracing::warn!(
                            "e2ee: key index={} for {}/{} is withheld: {outcome:?}",
                            job.index, job.to.0, job.to.1
                        ),
                        Recorded::RetryIn(wait) => tracing::info!(
                            "e2ee: key index={} for {}/{} not delivered ({outcome:?}); retrying in {wait:?}",
                            job.index, job.to.0, job.to.1
                        ),
                        Recorded::Done | Recorded::Stale => {}
                    }
                    group.shared.progress.notify_one();
                    // A retry may now be the next thing due, and so may this
                    // user's next key.
                    group.shared.wake.notify_one();
                }
            });
        }
        let next = next.map(tokio::time::Instant::from_std);
        tokio::select! {
            _ = shared.wake.notified() => {}
            Some(_) = sends.join_next(), if !sends.is_empty() => {}
            _ = tokio::time::sleep_until(next.unwrap_or_else(tokio::time::Instant::now)), if next.is_some() => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_derivation_is_deterministic() {
        let material = [42u8; 32];
        let k1 = E2eeManager::derive_frame_key(&material, "member-abc", 0);
        let k2 = E2eeManager::derive_frame_key(&material, "member-abc", 0);
        assert_eq!(k1, k2);
    }

    #[test]
    fn key_derivation_differs_by_index() {
        let material = [42u8; 32];
        let k0 = E2eeManager::derive_frame_key(&material, "member-abc", 0);
        let k1 = E2eeManager::derive_frame_key(&material, "member-abc", 1);
        assert_ne!(k0, k1);
    }

    #[test]
    fn key_derivation_differs_by_member_id() {
        let material = [42u8; 32];
        let ka = E2eeManager::derive_frame_key(&material, "member-abc", 0);
        let kb = E2eeManager::derive_frame_key(&material, "member-xyz", 0);
        assert_ne!(ka, kb);
    }

    #[test]
    fn rotate_increments_index() {
        let mut mgr = E2eeManager::new();
        assert_eq!(mgr.own_index(), 0);
        let (idx, _) = mgr.rotate();
        assert_eq!(idx, 1);
        assert_eq!(mgr.own_index(), 1);
    }

    #[test]
    fn rotate_wraps_at_255() {
        let mut mgr = E2eeManager::new();
        mgr.own_index = 255;
        let (idx, _) = mgr.rotate();
        assert_eq!(idx, 0);
    }

    #[test]
    fn receive_peer_key_roundtrip() {
        let mut mgr = E2eeManager::new();
        // Generate a key and base64-encode it
        let raw = [7u8; 32];
        let b64 = Base64::encode_string(&raw);
        let fk = mgr.receive_peer_key("peer-1", 0, &b64).unwrap();
        let expected = E2eeManager::derive_frame_key(&raw, "peer-1", 0);
        assert_eq!(fk, expected);
    }

    #[test]
    fn derive_frame_key_matches_independent_hkdf() {
        let material = [9u8; 32];
        let hk = Hkdf::<Sha256>::new(Some(b"livekit_frame"), &material);
        let mut expected = [0u8; 16];
        hk.expand(b"@a:x:DEV\x03", &mut expected).unwrap();
        assert_eq!(
            E2eeManager::derive_frame_key(&material, "@a:x:DEV", 3),
            expected
        );
    }

    #[test]
    fn own_key_b64_decodes_to_raw_key() {
        let mgr = E2eeManager::new();
        let decoded = Base64::decode_vec(&mgr.own_key_b64()).unwrap();
        assert_eq!(decoded.as_slice(), mgr.own_raw_key().as_slice());
    }

    #[test]
    fn own_frame_key_follows_own_key_and_index() {
        let mut mgr = E2eeManager::new();
        let before = mgr.own_frame_key("me");
        assert_eq!(
            before,
            E2eeManager::derive_frame_key(mgr.own_raw_key(), "me", 0)
        );
        let (idx, b64) = mgr.rotate();
        assert_eq!(b64, mgr.own_key_b64());
        assert_eq!(
            mgr.own_frame_key("me"),
            E2eeManager::derive_frame_key(mgr.own_raw_key(), "me", idx)
        );
        assert_ne!(mgr.own_frame_key("me"), before);
    }

    #[test]
    fn rotate_replaces_key_material() {
        let mut mgr = E2eeManager::new();
        let old = *mgr.own_raw_key();
        mgr.rotate();
        assert_ne!(&old, mgr.own_raw_key());
    }

    #[test]
    fn receive_peer_key_rejects_bad_base64() {
        let mut mgr = E2eeManager::new();
        assert!(mgr.receive_peer_key("p", 0, "not base64 !!!").is_err());
        assert!(!mgr.peer_keys.contains_key("p"));
    }

    #[test]
    fn receive_peer_key_rejects_wrong_length() {
        let mut mgr = E2eeManager::new();
        for len in [0usize, 16, 31, 33] {
            let b64 = Base64::encode_string(&vec![1u8; len]);
            let err = mgr.receive_peer_key("p", 0, &b64).unwrap_err();
            assert!(err.to_string().contains("32 bytes"), "len {len}: {err}");
        }
        assert!(!mgr.peer_keys.contains_key("p"));
    }

    #[test]
    fn receive_peer_key_same_index_overwrites() {
        let mut mgr = E2eeManager::new();
        mgr.receive_peer_key("p", 0, &Base64::encode_string(&[1u8; 32]))
            .unwrap();
        let second = mgr
            .receive_peer_key("p", 0, &Base64::encode_string(&[2u8; 32]))
            .unwrap();
        assert_eq!(mgr.peer_keys["p"][&0], second);
        assert_eq!(mgr.peer_keys["p"].len(), 1);
    }

    fn parked(user: &str, device: &str, index: i32) -> ParkedKey {
        ParkedKey {
            user_id: user.into(),
            device_id: device.into(),
            index,
            raw_key: vec![index as u8; 32],
            recheck: None,
        }
    }

    #[test]
    fn key_before_member_event_is_stored() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        p.park(parked("@a:x", "DEV", 0), t0);
        // Not a member yet: nothing is released and the key is kept.
        assert!(p.take_for(&[("@b:x".into(), "DEV".into())], t0).is_empty());
        // The member event arrives: the key is released exactly once.
        let got = p.take_for(&[("@a:x".into(), "DEV".into())], t0);
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].index, 0);
        assert!(p.take_for(&[("@a:x".into(), "DEV".into())], t0).is_empty());
    }

    #[test]
    fn parked_keys_expire_after_the_ttl() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        p.park(parked("@a:x", "DEV", 0), t0);
        let late = t0 + PARK_TTL + std::time::Duration::from_secs(1);
        assert!(p.take_for(&[("@a:x".into(), "DEV".into())], late).is_empty());
    }

    #[test]
    fn one_sender_is_capped_to_its_newest_keys() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        for i in 0..(PER_SENDER_CAP as i32 + 5) {
            p.park(parked("@a:x", "DEV", i), t0);
        }
        let got = p.take_for(&[("@a:x".into(), "DEV".into())], t0);
        assert_eq!(got.len(), PER_SENDER_CAP);
        assert_eq!(got[0].index, 5, "the oldest made way");
    }

    #[test]
    fn one_sender_cannot_evict_another_senders_key() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        p.park(parked("@honest:x", "DEV", 0), t0);
        for i in 0..500 {
            p.park(parked("@spam:x", "DEV", i), t0);
        }
        let got = p.take_for(&[("@honest:x".into(), "DEV".into())], t0);
        assert_eq!(got.len(), 1);
    }

    #[test]
    fn the_global_cap_drops_the_most_prolific_sender_first() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        for u in 0..(PARK_CAP - 1) {
            p.park(parked(&format!("@u{u}:x"), "D", 0), t0);
        }
        // Two keys from u0 make it the most prolific; the next newcomer evicts one of its keys.
        p.park(parked("@u0:x", "D", 1), t0);
        p.park(parked("@new:x", "D", 0), t0);
        let all: Vec<(String, String)> = (0..PARK_CAP - 1)
            .map(|u| (format!("@u{u}:x"), "D".to_owned()))
            .chain(std::iter::once(("@new:x".to_owned(), "D".to_owned())))
            .collect();
        let got = p.take_for(&all, t0);
        assert_eq!(got.len(), PARK_CAP, "the cap holds");
        assert!(got.iter().any(|k| k.user_id == "@new:x"), "the newcomer was kept");
        assert_eq!(got.iter().filter(|k| k.user_id == "@u0:x").count(), 1, "u0 lost one key");
        assert_eq!(got.iter().find(|k| k.user_id == "@u0:x").unwrap().index, 1, "its oldest went");
    }

    #[test]
    fn the_global_cap_evicts_the_oldest_key_when_everyone_holds_the_same() {
        let t0 = std::time::Instant::now();
        let s = std::time::Duration::from_millis;
        let mut p = ParkedKeys::new();
        // Parked in reverse alphabetical order, so a map order could not give this.
        for (n, u) in (0..PARK_CAP).rev().enumerate() {
            p.park(parked(&format!("@u{u:02}:x"), "D", 0), t0 + s(n as u64));
        }
        p.park(parked("@new:x", "D", 0), t0 + s(1000));
        let all: Vec<(String, String)> = (0..PARK_CAP)
            .map(|u| (format!("@u{u:02}:x"), "D".to_owned()))
            .chain(std::iter::once(("@new:x".to_owned(), "D".to_owned())))
            .collect();
        let got = p.take_for(&all, t0 + s(1000));
        assert_eq!(got.len(), PARK_CAP);
        let last = format!("@u{:02}:x", PARK_CAP - 1);
        assert!(!got.iter().any(|k| k.user_id == last), "the oldest one made way");
    }

    #[test]
    fn legacy_key_body_is_unchanged_without_a_matrixrtc_member() {
        let v = key_event_content("!r:x", "@a:x:DEV", "DEV", "S0VZ", 3, None, 1234);
        assert_eq!(v["keys"], serde_json::json!({"key": "S0VZ", "index": 3}));
        assert_eq!(v["member"], serde_json::json!({"claimed_device_id": "DEV", "id": "@a:x:DEV"}));
        assert_eq!(v["session"]["application"], "m.call");
        assert!(v.get("member_id").is_none() && v.get("media_key").is_none());
    }

    #[test]
    fn a_matrixrtc_member_adds_the_stable_fields_next_to_the_legacy_ones() {
        let v = key_event_content("!r:x", "@a:x:DEV", "DEV", "S0VZ", 3, Some("mid1"), 1234);
        assert_eq!(v["member_id"], "mid1");
        assert_eq!(v["media_key"], serde_json::json!({"key": "S0VZ", "index": 3}));
        assert_eq!(v["keys"]["index"], 3, "reference-implementation clients still read keys/member");
        assert_eq!(v["room_id"], "!r:x");
    }

    // ── Parked keys waiting for their sending device ───────────────────────────

    fn recheck_key(user: &str, curve: &str) -> ParkedKey {
        use matrix_sdk::deserialized_responses::{
            AlgorithmInfo, DeviceLinkProblem, EncryptionInfo, VerificationLevel, VerificationState,
        };
        ParkedKey {
            user_id: user.into(),
            device_id: recheck_device_id(curve),
            index: 0,
            raw_key: Vec::new(),
            recheck: Some(Recheck {
                content: serde_json::from_value(serde_json::json!({
                    "room_id": "!r:x", "member_id": "m1", "media_key": { "index": 1, "key": "AAAA" }
                }))
                .unwrap(),
                info: EncryptionInfo {
                    sender: user.try_into().unwrap(),
                    sender_device: None,
                    forwarder: None,
                    algorithm_info: AlgorithmInfo::OlmV1Curve25519AesSha2 {
                        curve25519_public_key_base64: curve.to_owned(),
                    },
                    verification_state: VerificationState::Unverified(VerificationLevel::None(
                        DeviceLinkProblem::MissingDevice,
                    )),
                },
            }),
        }
    }

    #[test]
    fn a_key_from_an_unknown_device_is_not_released_as_a_members_key() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        p.park(recheck_key("@a:x", "CURVE"), t0);
        p.park(parked("@a:x", "DEV", 2), t0);
        // Even a member whose device id equals the stand-in does not get it.
        let lookalike = (String::from("@a:x"), recheck_device_id("CURVE"));
        let got = p.take_for(&[("@a:x".into(), "DEV".into()), lookalike], t0);
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].index, 2);
        assert_eq!(p.take_rechecks(t0).len(), 1, "it is still waiting");
    }

    #[test]
    fn rechecks_come_out_with_their_park_time_and_expire_on_that_clock() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        p.park(recheck_key("@a:x", "C1"), t0);
        p.park(parked("@b:x", "DEV", 0), t0);
        let later = t0 + std::time::Duration::from_secs(30);
        let mut out = p.take_rechecks(later);
        assert_eq!(out.len(), 1);
        let (at, key) = out.remove(0);
        assert_eq!(at, t0);
        // Undecided: back it goes with its original time.
        p.park(key, at);
        let (at, key) = p.take_rechecks(later).remove(0);
        p.park(key, at);
        let late = t0 + PARK_TTL + std::time::Duration::from_secs(1);
        assert!(p.take_rechecks(late).is_empty(), "waiting did not extend its life");
    }

    #[test]
    fn unknown_devices_are_capped_per_user_not_per_olm_key() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        // One member with a fresh Olm identity per event, none of which ever uploads device keys.
        for i in 0..200 {
            p.park(recheck_key("@spam:x", &format!("CURVE{i}")), t0 + std::time::Duration::from_millis(i));
        }
        p.park(recheck_key("@a:x", "A"), t0 + std::time::Duration::from_secs(1));
        let out = p.take_rechecks(t0 + std::time::Duration::from_secs(1));
        assert_eq!(out.iter().filter(|(_, k)| k.user_id == "@spam:x").count(), PER_USER_RECHECK_CAP);
        assert_eq!(out.iter().filter(|(_, k)| k.user_id == "@a:x").count(), 1);
        // The newest of the spammer's identities are the ones kept.
        let kept: Vec<_> = out.iter().filter(|(_, k)| k.user_id == "@spam:x").map(|(at, _)| *at).collect();
        assert!(kept.iter().all(|at| *at >= t0 + std::time::Duration::from_millis(200 - PER_USER_RECHECK_CAP as u64)));
    }

    #[test]
    fn waiting_for_a_device_can_never_push_out_a_key_waiting_for_a_membership() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        for i in 0..(PARK_CAP - 1) {
            p.park(parked(&format!("@m{i}:x"), "DEV", 0), t0);
        }
        // Many joined members, each with several unknown devices: far past every rechecks cap.
        for u in 0..100 {
            for i in 0..4 {
                p.park(recheck_key(&format!("@r{u}:x"), &format!("C{u}-{i}")), t0);
            }
        }
        let members: Vec<(String, String)> =
            (0..(PARK_CAP - 1)).map(|i| (format!("@m{i}:x"), "DEV".to_owned())).collect();
        assert_eq!(p.take_for(&members, t0).len(), PARK_CAP - 1, "every membership key is still there");
        assert!(p.take_rechecks(t0).len() <= RECHECK_CAP, "rechecks have a budget of their own");
    }

    #[test]
    fn a_full_recheck_budget_drops_from_the_user_with_the_most_waiting() {
        let t0 = std::time::Instant::now();
        let mut p = ParkedKeys::new();
        // Four users with 2 each, then one user filling the rest of the budget.
        for u in 0..4 {
            for i in 0..2 {
                p.park(recheck_key(&format!("@honest{u}:x"), &format!("H{u}-{i}")), t0);
            }
        }
        for i in 0..(RECHECK_CAP + 10) {
            p.park(recheck_key("@spam:x", &format!("S{i}")), t0 + std::time::Duration::from_millis(i as u64));
        }
        let out = p.take_rechecks(t0 + std::time::Duration::from_secs(1));
        for u in 0..4 {
            assert_eq!(out.iter().filter(|(_, k)| k.user_id == format!("@honest{u}:x")).count(), 2);
        }
        assert!(out.len() <= RECHECK_CAP);
    }

    // ── Rate limiter ───────────────────────────────────────────────────────────

    #[test]
    fn the_limiter_lets_one_through_per_key_and_interval() {
        let t0 = std::time::Instant::now();
        let s = std::time::Duration::from_secs;
        let mut l = RateLimiter::new(s(10), 8);
        assert!(l.allow("@a:x", t0));
        assert!(!l.allow("@a:x", t0 + s(9)));
        assert!(l.allow("@b:x", t0 + s(9)), "another sender has its own budget");
        assert!(l.allow("@a:x", t0 + s(10)));
        assert!(!l.allow("@a:x", t0 + s(11)));
    }

    #[test]
    fn the_limiter_stays_bounded() {
        let t0 = std::time::Instant::now();
        let s = std::time::Duration::from_secs;
        let mut l = RateLimiter::new(s(10), 3);
        for i in 0..3 {
            assert!(l.allow(&format!("k{i}"), t0));
        }
        assert!(!l.allow("new", t0 + s(1)), "full: a new key is refused");
        assert!(l.allow("new", t0 + s(11)), "old ones aged out");
    }

    // ── Key delivery ───────────────────────────────────────────────────────────

    fn to(user: &str) -> Recipient {
        (user.to_owned(), "DEV".to_owned())
    }

    fn secs(n: u64) -> std::time::Duration {
        std::time::Duration::from_secs(n)
    }

    #[test]
    fn outcomes_tell_a_missing_device_from_a_failed_send() {
        assert_eq!(delivery_outcome(1, 0, 0), Delivery::Delivered);
        assert_eq!(delivery_outcome(0, 0, 0), Delivery::NoDevice, "no device is not success");
        assert!(matches!(delivery_outcome(2, 1, 0), Delivery::Failed(_)));
    }

    #[test]
    fn a_device_withheld_by_policy_is_final_not_a_failure() {
        assert!(matches!(delivery_outcome(1, 1, 1), Delivery::Withheld(_)));
        // One device got it, the other never will: nothing left to do.
        assert_eq!(delivery_outcome(2, 1, 1), Delivery::Delivered);
        // A real failure next to a withheld device is still worth another try.
        assert!(matches!(delivery_outcome(3, 2, 1), Delivery::Failed(_)));
        assert!(withheld_by_policy(true, false));
        assert!(!withheld_by_policy(true, true));
        assert!(!withheld_by_policy(false, false), "nothing is excluded when the policy is off");
    }

    #[test]
    fn a_delivered_key_is_not_owed_any_more() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x"), to("@b:x")], 3, "KEY", t0);
        let jobs = p.take_due(t0);
        assert_eq!(jobs.len(), 2);
        assert!(p.take_due(t0).is_empty(), "in flight sends are not handed out twice");
        assert_eq!(p.record(&jobs[0], &Delivery::Delivered, t0), Recorded::Done);
        assert!(!p.is_empty());
        assert_eq!(p.record(&jobs[1], &Delivery::Delivered, t0), Recorded::Done);
        assert!(p.is_empty());
    }

    #[test]
    fn an_undelivered_key_is_retried_on_the_backoff_then_again_every_minute() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "KEY", t0);
        let mut now = t0;
        for wait in RETRY_BACKOFF {
            let job = p.take_due(now).pop().expect("due");
            assert_eq!(p.record(&job, &Delivery::NoDevice, now), Recorded::RetryIn(wait));
            assert!(p.take_due(now + wait - secs(1)).is_empty(), "not before the backoff");
            assert_eq!(p.next_due(), Some(now + wait));
            now += wait;
        }
        let last = p.take_due(now).pop().expect("the final attempt");
        assert_eq!(
            p.record(&last, &Delivery::Failed("offline".into()), now),
            Recorded::GaveUp { rounds: 1 }
        );
        assert!(!p.is_empty(), "the key is still owed");
        assert_eq!(p.next_due(), Some(now + GIVEN_UP_RETRY));
        assert!(p.take_due(now + GIVEN_UP_RETRY - secs(1)).is_empty());
        // The next round starts the schedule over.
        now += GIVEN_UP_RETRY;
        let again = p.take_due(now).pop().expect("a new round");
        assert_eq!(
            p.record(&again, &Delivery::Failed("offline".into()), now),
            Recorded::RetryIn(RETRY_BACKOFF[0])
        );
    }

    #[test]
    fn a_recipient_that_comes_back_gets_the_key_after_the_give_up() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "KEY", t0);
        let mut now = t0;
        for _ in 0..=RETRY_BACKOFF.len() {
            let job = p.take_due(now).pop().expect("due");
            p.record(&job, &Delivery::Failed("offline".into()), now);
            now = p.next_due().unwrap();
        }
        let job = p.take_due(now).pop().expect("the key is tried again");
        assert_eq!(p.record(&job, &Delivery::Delivered, now), Recorded::Done);
        assert!(p.is_empty());
    }

    #[test]
    fn a_withheld_key_is_not_tried_again() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "KEY", t0);
        let job = p.take_due(t0).pop().unwrap();
        assert_eq!(p.record(&job, &Delivery::Withheld("untrusted".into()), t0), Recorded::Final);
        assert!(p.is_empty());
        assert!(p.settled(1));
    }

    #[test]
    fn keys_no_longer_in_use_are_not_owed_any_more() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "OLD", t0);
        p.enqueue(&[to("@a:x")], 2, "NEW", t0);
        p.retain_indices(&[2]);
        let jobs = p.take_due(t0);
        assert_eq!(jobs.len(), 1);
        assert_eq!(jobs[0].index, 2);
    }

    #[test]
    fn a_retry_that_delivers_ends_the_schedule() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "KEY", t0);
        let job = p.take_due(t0).pop().unwrap();
        p.record(&job, &Delivery::NoDevice, t0);
        let retry = p.take_due(t0 + secs(2)).pop().unwrap();
        assert_eq!(p.record(&retry, &Delivery::Delivered, t0 + secs(2)), Recorded::Done);
        assert!(p.is_empty());
    }

    #[test]
    fn a_new_enqueue_restarts_the_schedule_and_makes_the_old_result_stale() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "OLD", t0);
        let old = p.take_due(t0).pop().unwrap();
        p.enqueue(&[to("@a:x")], 1, "NEW", t0 + secs(1));
        assert_eq!(p.record(&old, &Delivery::Delivered, t0 + secs(2)), Recorded::Stale);
        let new = p.take_due(t0 + secs(1)).pop().unwrap();
        assert_eq!(new.key_b64, "NEW");
        assert_eq!(p.record(&new, &Delivery::Delivered, t0 + secs(2)), Recorded::Done);
    }

    #[test]
    fn leavers_are_no_longer_owed_anything() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x"), to("@b:x")], 1, "KEY", t0);
        let jobs = p.take_due(t0);
        p.retain_recipients(&[to("@a:x")].into_iter().collect());
        let b = jobs.iter().find(|j| j.to == to("@b:x")).unwrap();
        assert_eq!(p.record(b, &Delivery::Delivered, t0), Recorded::Stale);
        assert!(!p.is_empty(), "@a is still owed");
    }

    #[test]
    fn a_key_is_settled_when_every_recipient_has_it_or_cannot_be_reached() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        assert!(p.settled(1), "nothing owed");
        p.enqueue(&[to("@a:x"), to("@b:x")], 1, "KEY", t0);
        p.enqueue(&[to("@a:x")], 2, "OTHER", t0);
        assert!(!p.settled(1));
        let jobs = p.take_due(t0);
        let job = |u: &str, i: u8| jobs.iter().find(|j| j.to == to(u) && j.index == i).unwrap().clone();
        p.record(&job("@a:x", 1), &Delivery::Delivered, t0);
        assert!(!p.settled(1), "@b has not got it");
        p.record(&job("@b:x", 1), &Delivery::NoDevice, t0);
        assert!(!p.settled(1), "tried once is not delivered: a retry is pending");
        assert!(!p.settled(2), "another key's round is separate");
    }

    #[test]
    fn an_unreachable_recipient_does_not_hold_a_new_key_back() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@ghost:x")], 1, "K1", t0);
        let mut now = t0;
        for _ in 0..=RETRY_BACKOFF.len() {
            let job = p.take_due(now).pop().unwrap();
            p.record(&job, &Delivery::NoDevice, now);
            now = p.next_due().unwrap();
        }
        // The next key is owed to the ghost too, but it is known not to answer.
        p.enqueue(&[to("@ghost:x"), to("@live:x")], 2, "K2", now);
        assert!(!p.settled(2), "@live has not got it yet");
        let jobs = p.take_due(now);
        let job = |u: &str, i: u8| jobs.iter().find(|j| j.to == to(u) && j.index == i).unwrap().clone();
        p.record(&job("@live:x", 2), &Delivery::Delivered, now);
        assert!(p.settled(2), "only the unreachable ghost is left");
        // Reaching it again makes it count again.
        p.record(&job("@ghost:x", 1), &Delivery::Delivered, now);
        p.enqueue(&[to("@ghost:x")], 3, "K3", now);
        assert!(!p.settled(3));
    }

    // ── One send per user at a time ────────────────────────────────────────────

    #[test]
    fn a_user_with_a_send_in_flight_is_not_handed_a_second() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "K1", t0);
        let first = p.take_due(t0).pop().unwrap();
        // The new key replaces nothing and is due, but @a is busy.
        p.enqueue(&[to("@a:x")], 2, "K2", t0);
        assert!(p.take_due(t0 + secs(1)).is_empty());
        assert_eq!(p.next_due(), None, "nothing is due while @a is busy; its send reports back");
        p.record(&first, &Delivery::Delivered, t0 + secs(1));
        let second = p.take_due(t0 + secs(1)).pop().unwrap();
        assert_eq!(second.index, 2);
    }

    #[test]
    fn a_replaced_entry_does_not_start_a_second_send_while_the_first_runs() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "OLD", t0);
        let old = p.take_due(t0).pop().unwrap();
        p.enqueue(&[to("@a:x")], 1, "NEW", t0);
        assert!(p.take_due(t0).is_empty(), "the old send still runs");
        assert_eq!(p.record(&old, &Delivery::Delivered, t0), Recorded::Stale);
        assert_eq!(p.take_due(t0).pop().unwrap().key_b64, "NEW");
    }

    #[test]
    fn what_a_user_is_owed_goes_out_oldest_key_first() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        // Index 255 is older than index 0 (the index wrapped): the order is the enqueue order.
        p.enqueue(&[to("@a:x")], 255, "OLD", t0);
        p.enqueue(&[to("@a:x")], 0, "NEW", t0);
        let jobs = p.take_due(t0);
        assert_eq!(jobs.iter().map(|j| j.index).collect::<Vec<_>>(), vec![255, 0]);
    }

    #[test]
    fn other_users_are_served_while_one_is_busy() {
        let t0 = std::time::Instant::now();
        let mut p = PendingDeliveries::new();
        p.enqueue(&[to("@a:x")], 1, "K", t0);
        p.take_due(t0);
        p.enqueue(&[to("@a:x"), to("@b:x")], 2, "K2", t0);
        let jobs = p.take_due(t0);
        assert_eq!(jobs.len(), 1);
        assert_eq!(jobs[0].to, to("@b:x"));
    }

    /// Answers from a script and notes when each send started.
    struct FakeSender {
        started: parking_lot::Mutex<Vec<(String, u8, tokio::time::Instant)>>,
        /// Outcomes per user, consumed in order; `Delivered` once exhausted.
        script: parking_lot::Mutex<std::collections::HashMap<String, Vec<Delivery>>>,
        /// Users whose send never finishes.
        hang: Vec<String>,
        /// Users whose first send panics.
        panic_once: parking_lot::Mutex<Vec<String>>,
        running: std::sync::atomic::AtomicUsize,
        most_running: std::sync::atomic::AtomicUsize,
        busy_for: std::time::Duration,
    }

    impl FakeSender {
        fn new() -> Self {
            Self {
                started: Default::default(),
                script: Default::default(),
                hang: Vec::new(),
                panic_once: Default::default(),
                running: Default::default(),
                most_running: Default::default(),
                busy_for: std::time::Duration::ZERO,
            }
        }

        fn starts_for(&self, user: &str) -> Vec<std::time::Duration> {
            let t0 = self.started.lock().iter().map(|s| s.2).min().unwrap();
            self.started.lock().iter().filter(|s| s.0 == user).map(|s| s.2 - t0).collect()
        }
    }

    #[async_trait::async_trait]
    impl KeySender for FakeSender {
        async fn send(&self, to: &Recipient, index: u8, _key_b64: &str) -> Delivery {
            use std::sync::atomic::Ordering::SeqCst;
            self.started.lock().push((to.0.clone(), index, tokio::time::Instant::now()));
            if self.hang.contains(&to.0) {
                std::future::pending::<()>().await;
            }
            let blow_up = {
                let mut users = self.panic_once.lock();
                users.iter().position(|u| *u == to.0).map(|i| users.remove(i)).is_some()
            };
            if blow_up {
                panic!("the sender blew up");
            }
            let now = self.running.fetch_add(1, SeqCst) + 1;
            self.most_running.fetch_max(now, SeqCst);
            tokio::time::sleep(self.busy_for).await;
            self.running.fetch_sub(1, SeqCst);
            let mut script = self.script.lock();
            match script.get_mut(&to.0) {
                Some(v) if !v.is_empty() => v.remove(0),
                _ => Delivery::Delivered,
            }
        }
    }

    #[tokio::test(start_paused = true)]
    async fn the_worker_retries_a_missing_device_until_it_shows_up() {
        let sender = std::sync::Arc::new(FakeSender::new());
        sender.script.lock().insert("@a:x".into(), vec![Delivery::NoDevice, Delivery::NoDevice]);
        let d = KeyDelivery::new();
        let _worker = d.spawn(sender.clone());
        d.enqueue(&[to("@a:x")], 5, "KEY");
        assert!(!d.settled(5));
        tokio::time::sleep(secs(60)).await;
        assert_eq!(sender.starts_for("@a:x"), vec![secs(0), secs(2), secs(7)], "2 s, then 5 s");
        assert!(d.settled(5));
        assert!(d.shared.pending.lock().is_empty());
    }

    #[tokio::test(start_paused = true)]
    async fn the_worker_tries_again_after_a_minute_when_the_backoff_is_spent() {
        let sender = std::sync::Arc::new(FakeSender::new());
        // The sender was offline for a while: five failures, then it works.
        sender.script.lock().insert("@a:x".into(), vec![Delivery::Failed("offline".into()); 5]);
        let d = KeyDelivery::new();
        let _worker = d.spawn(sender.clone());
        d.enqueue(&[to("@a:x")], 1, "KEY");
        tokio::time::sleep(secs(100)).await;
        assert_eq!(
            sender.starts_for("@a:x"),
            vec![secs(0), secs(2), secs(7), secs(22), secs(52)],
            "first try plus four retries"
        );
        assert!(!d.shared.pending.lock().is_empty(), "still owed");
        assert!(d.settled(1), "an unreachable recipient does not hold the key back");
        tokio::time::sleep(secs(60)).await;
        assert_eq!(sender.starts_for("@a:x").len(), 6, "one more try a minute after the last");
        assert!(d.shared.pending.lock().is_empty(), "delivered");
    }

    #[tokio::test(start_paused = true)]
    async fn a_member_who_never_answers_is_tried_every_minute_until_they_leave() {
        let sender = std::sync::Arc::new(FakeSender::new());
        sender.script.lock().insert("@a:x".into(), vec![Delivery::NoDevice; 100]);
        let d = KeyDelivery::new();
        let _worker = d.spawn(sender.clone());
        d.enqueue(&[to("@a:x")], 1, "KEY");
        tokio::time::sleep(secs(400)).await;
        assert!(sender.starts_for("@a:x").len() > 8, "tries went on after the first round");
        d.retain_recipients(&Default::default());
        let before = sender.starts_for("@a:x").len();
        tokio::time::sleep(secs(300)).await;
        assert_eq!(sender.starts_for("@a:x").len(), before, "nothing after they left");
    }

    #[tokio::test(start_paused = true)]
    async fn sends_to_one_user_never_overlap_and_keep_their_order() {
        let mut fake = FakeSender::new();
        fake.busy_for = secs(1);
        let sender = std::sync::Arc::new(fake);
        let d = KeyDelivery::new();
        let _worker = d.spawn(sender.clone());
        d.enqueue(&[to("@a:x")], 1, "K1");
        tokio::time::sleep(std::time::Duration::from_millis(100)).await;
        d.enqueue(&[to("@a:x")], 1, "K1 again");
        d.enqueue(&[to("@a:x")], 2, "K2");
        tokio::time::sleep(secs(20)).await;
        assert_eq!(sender.most_running.load(std::sync::atomic::Ordering::SeqCst), 1);
        let order: Vec<u8> = sender.started.lock().iter().map(|s| s.1).collect();
        assert_eq!(order, vec![1, 1, 2], "the replaced key is sent again, then the newer one");
        assert!(d.shared.pending.lock().is_empty());
    }

    #[tokio::test(start_paused = true)]
    async fn a_panicking_send_does_not_leave_the_user_stuck() {
        let sender = std::sync::Arc::new(FakeSender::new());
        sender.panic_once.lock().push("@a:x".into());
        let d = KeyDelivery::new();
        let _worker = d.spawn(sender.clone());
        d.enqueue(&[to("@a:x")], 1, "KEY");
        tokio::time::sleep(secs(30)).await;
        assert_eq!(sender.starts_for("@a:x").len(), 2, "the failed send was retried");
        assert!(d.shared.pending.lock().is_empty(), "and delivered");
    }

    #[tokio::test(start_paused = true)]
    async fn sends_run_concurrently_up_to_the_cap() {
        let mut fake = FakeSender::new();
        fake.busy_for = secs(1);
        let sender = std::sync::Arc::new(fake);
        let d = KeyDelivery::new();
        let _worker = d.spawn(sender.clone());
        let users: Vec<Recipient> = (0..20).map(|i| to(&format!("@u{i}:x"))).collect();
        d.enqueue(&users, 1, "KEY");
        tokio::time::sleep(secs(10)).await;
        assert!(d.settled(1));
        assert_eq!(
            sender.most_running.load(std::sync::atomic::Ordering::SeqCst),
            MAX_CONCURRENT_SENDS
        );
        // 20 one-second sends, 8 at a time: three waves, not twenty seconds.
        let last = sender.started.lock().iter().map(|s| s.2).max().unwrap();
        let first = sender.started.lock().iter().map(|s| s.2).min().unwrap();
        assert_eq!(last - first, secs(2));
    }

    #[tokio::test(start_paused = true)]
    async fn a_hung_send_times_out_and_does_not_hold_up_the_others() {
        let mut fake = FakeSender::new();
        fake.hang = vec!["@stuck:x".into()];
        let sender = std::sync::Arc::new(fake);
        let d = KeyDelivery::new();
        let _worker = d.spawn(sender.clone());
        d.enqueue(&[to("@stuck:x"), to("@fine:x")], 1, "KEY");
        tokio::time::sleep(secs(1)).await;
        assert!(!d.settled(1), "the stuck recipient has not been tried out yet");
        tokio::time::sleep(SEND_TIMEOUT * 3).await;
        assert!(!d.settled(1), "a timeout is not a delivery: the key is still owed");
        assert_eq!(sender.starts_for("@fine:x"), vec![secs(0)], "delivered at once, once");
    }

    #[tokio::test(start_paused = true)]
    async fn a_member_who_left_is_not_retried() {
        let sender = std::sync::Arc::new(FakeSender::new());
        sender.script.lock().insert("@a:x".into(), vec![Delivery::NoDevice; 10]);
        let d = KeyDelivery::new();
        let _worker = d.spawn(sender.clone());
        d.enqueue(&[to("@a:x")], 1, "KEY");
        tokio::time::sleep(secs(1)).await;
        d.retain_recipients(&Default::default());
        tokio::time::sleep(secs(120)).await;
        assert_eq!(sender.starts_for("@a:x").len(), 1);
    }
}
