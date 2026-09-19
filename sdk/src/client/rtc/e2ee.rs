use anyhow::Context;
use base64ct::{Base64, Encoding};
use hkdf::Hkdf;
use rand::RngCore;
use sha2::Sha256;
use std::collections::HashMap;

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

    pub fn latest_peer_frame_key(&self, member_id: &str) -> Option<FrameKey> {
        self.peer_keys.get(member_id)?.values().last().copied()
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
) -> anyhow::Result<()> {
    use matrix_sdk::encryption::identities::Device;
    use matrix_sdk::ruma::{events::AnyToDeviceEventContent, UserId};
    use matrix_sdk_base::crypto::CollectStrategy;

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

    let content = serde_json::json!({
        "keys": { "key": key_b64, "index": index },
        "room_id": room_id,
        "member": { "claimed_device_id": our_device_id, "id": lk_identity },
        "session": { "call_id": "", "application": "m.call", "scope": "m.room" },
        "sent_ts": now_ms,
    });
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
            CollectStrategy::AllDevices,
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
}
