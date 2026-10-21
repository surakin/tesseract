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
/// Most keys kept waiting at once; the most prolific sender loses its oldest first.
pub const PARK_CAP: usize = 64;
/// Most keys kept waiting per sending device, so one sender cannot push out the rest.
pub const PER_SENDER_CAP: usize = 8;

/// A frame key whose sender is not a live call member (yet).
#[derive(Clone, Debug)]
pub struct ParkedKey {
    pub user_id: String,
    pub device_id: String,
    pub index: i32,
    pub raw_key: Vec<u8>,
}

/// Keys that arrived before their sender's membership event.
#[derive(Default)]
pub struct ParkedKeys {
    entries: Vec<(std::time::Instant, ParkedKey)>,
}

impl ParkedKeys {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn park(&mut self, key: ParkedKey, now: std::time::Instant) {
        self.entries.retain(|(at, _)| now.saturating_duration_since(*at) <= PARK_TTL);
        let same = |k: &ParkedKey| k.user_id == key.user_id && k.device_id == key.device_id;
        // Per sender: drop its oldest key.
        if self.entries.iter().filter(|(_, k)| same(k)).count() >= PER_SENDER_CAP {
            if let Some(i) = self.entries.iter().position(|(_, k)| same(k)) {
                self.entries.remove(i);
            }
        }
        // Overall: drop the oldest key of whoever holds the most.
        if self.entries.len() >= PARK_CAP {
            let mut counts: std::collections::HashMap<(&str, &str), usize> = std::collections::HashMap::new();
            for (_, k) in &self.entries {
                *counts.entry((k.user_id.as_str(), k.device_id.as_str())).or_default() += 1;
            }
            let worst = counts
                .into_iter()
                .max_by_key(|(_, n)| *n)
                .map(|((u, d), _)| (u.to_owned(), d.to_owned()));
            if let Some((u, d)) = worst {
                if let Some(i) = self.entries.iter().position(|(_, k)| k.user_id == u && k.device_id == d) {
                    self.entries.remove(i);
                }
            }
        }
        self.entries.push((now, key));
    }

    /// Remove and return the parked keys whose sender is in `members`
    /// (`(user_id, device_id)`); expired ones are dropped.
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

/// Send a frame key to one specific Matrix user by their user ID.
///
/// Preferred over `broadcast_key` when the recipient is known via a LiveKit
/// participant identity: it bypasses `room.members()` (which can be stale or
/// incomplete for users who are not in the local member cache) and instead
/// performs a fresh `/keys/query` if the user's devices are not yet known.
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
) -> anyhow::Result<()> {
    use matrix_sdk::encryption::identities::Device;
    use matrix_sdk::ruma::{events::AnyToDeviceEventContent, UserId};

    let uid: &UserId = matrix_user_id
        .try_into()
        .context("invalid matrix user ID from LiveKit identity")?;

    let our_device_id = client
        .device_id()
        .ok_or_else(|| anyhow::anyhow!("device_id not available"))?
        .to_string();

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
    let raw_value =
        serde_json::value::to_raw_value(&content).context("serialize encryption key")?;
    let raw_content: matrix_sdk::ruma::serde::Raw<AnyToDeviceEventContent> =
        matrix_sdk::ruma::serde::Raw::from_json(raw_value);

    // Try the crypto store first; fall back to a fresh /keys/query if empty.
    let mut ud = client
        .encryption()
        .get_user_devices(uid)
        .await
        .context("get_user_devices")?;
    if ud.devices().next().is_none() {
        tracing::info!("e2ee: no cached devices for {matrix_user_id}; querying homeserver");
        let _ = client.encryption().request_user_identity(uid).await;
        ud = client
            .encryption()
            .get_user_devices(uid)
            .await
            .context("get_user_devices after identity request")?;
    }

    let devices: Vec<Device> = ud
        .devices()
        .filter(|d| device_filter.map_or(true, |f| d.device_id().as_str() == f))
        .collect();
    if devices.is_empty() {
        tracing::warn!(
            "e2ee: no matching device for {matrix_user_id} ({device_filter:?}) — key not delivered"
        );
        return Ok(());
    }

    tracing::info!(
        "e2ee: sending frame key index={index} to {matrix_user_id} ({} device(s))",
        devices.len()
    );
    let refs: Vec<&Device> = devices.iter().collect();
    let failures = client
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
        .context("send Olm-encrypted to-device key")?;
    if !failures.is_empty() {
        tracing::warn!(
            "e2ee: key delivery to {matrix_user_id} had {} failure(s): {failures:?}",
            failures.len()
        );
    }
    Ok(())
}

/// Send a frame key to the devices of the given live call members
/// `(user_id, device_id)`. Per MSC4143 keys go only to devices that hold a
/// call membership, not to every device of every room member.
pub async fn broadcast_key_to_members(
    client: &matrix_sdk::Client,
    members: &[(String, String)],
    room_id: &str,
    lk_identity: &str,
    key_b64: &str,
    index: u8,
    mrtc_member_id: Option<&str>,
) {
    tracing::info!(
        "e2ee: sending key index={index} to {} call member device(s)",
        members.len()
    );
    for (user, device) in members {
        if let Err(e) = send_frame_key_to_user(
            client,
            user,
            Some(device),
            room_id,
            lk_identity,
            key_b64,
            index,
            mrtc_member_id,
        )
        .await
        {
            tracing::warn!("e2ee: key delivery to {user}/{device} failed: {e}");
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
        for u in 0..PARK_CAP {
            p.park(parked(&format!("@u{u}:x"), "D", 0), t0);
        }
        // Two keys from u0 make it the most prolific; the next newcomer evicts one of its keys.
        p.park(parked("@u0:x", "D", 1), t0);
        p.park(parked("@new:x", "D", 0), t0);
        let all: Vec<(String, String)> = (0..PARK_CAP)
            .map(|u| (format!("@u{u}:x"), "D".to_owned()))
            .chain(std::iter::once(("@new:x".to_owned(), "D".to_owned())))
            .collect();
        let got = p.take_for(&all, t0);
        assert_eq!(got.len(), PARK_CAP, "the cap holds");
        assert!(got.iter().any(|k| k.user_id == "@new:x"), "the newcomer was kept");
        assert_eq!(got.iter().filter(|k| k.user_id == "@u0:x").count(), 1, "u0 lost one key");
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
}
