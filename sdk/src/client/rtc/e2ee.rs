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

    /// Broadcast our current key to all joined room members via Olm-encrypted
    /// to-device `org.matrix.msc4143.rtc.encryption_key` events.
    ///
    /// Olm-wrapping ensures the homeserver cannot read the frame key material.
    /// Without this an eavesdropper with homeserver access could decrypt all
    /// call media even though LiveKit AES-GCM frame encryption is enabled.
    pub async fn broadcast_own_key(
        &self,
        client: &matrix_sdk::Client,
        room: &matrix_sdk::Room,
        member_id: &str,
    ) -> anyhow::Result<()> {
        broadcast_key(client, room, member_id, &self.own_key_b64(), self.own_index).await
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

    let devices: Vec<Device> = ud.devices().collect();
    if devices.is_empty() {
        tracing::warn!(
            "e2ee: still no devices for {matrix_user_id} after key query — key not delivered"
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

/// Broadcast a frame key by value to all joined room members via Olm-encrypted
/// to-device events.  Exposed as a free function so callers that snapshot key
/// material before an `.await` can call it without holding the `E2eeManager`
/// mutex across the async boundary (`parking_lot::MutexGuard` is not `Send`).
pub async fn broadcast_key(
    client: &matrix_sdk::Client,
    room: &matrix_sdk::Room,
    member_id: &str,
    key_b64: &str,
    index: u8,
) -> anyhow::Result<()> {
    use matrix_sdk::encryption::identities::Device;
    use matrix_sdk::ruma::events::AnyToDeviceEventContent;
    use matrix_sdk_base::crypto::CollectStrategy;

    let our_device_id = client
        .device_id()
        .ok_or_else(|| anyhow::anyhow!("device id not available"))?
        .to_string();
    let room_id = room.room_id().to_string();
    let now_ms = {
        use std::time::{SystemTime, UNIX_EPOCH};
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64
    };
    let content = serde_json::json!({
        "keys": { "key": key_b64, "index": index },
        "room_id": room_id,
        "member": { "claimed_device_id": our_device_id, "id": member_id },
        "session": { "call_id": "", "application": "m.call", "scope": "m.room" },
        "sent_ts": now_ms,
    });
    let raw_value =
        serde_json::value::to_raw_value(&content).context("serialize encryption key")?;
    let raw_content: matrix_sdk::ruma::serde::Raw<AnyToDeviceEventContent> =
        matrix_sdk::ruma::serde::Raw::from_json(raw_value);

    let own_user = client
        .user_id()
        .ok_or_else(|| anyhow::anyhow!("not logged in"))?;

    let members = room
        .members(matrix_sdk::RoomMemberships::JOIN)
        .await
        .context("get room members for key broadcast")?;

    let other_members: Vec<_> = members.iter().filter(|m| m.user_id() != own_user).collect();
    tracing::info!(
        "e2ee: broadcasting key index={index} to {} other member(s)",
        other_members.len()
    );

    // Collect all UserDevices first so Device references outlive the call to
    // encrypt_and_send_raw_to_device.
    let mut all_user_devices = Vec::new();
    for member in &other_members {
        let uid = member.user_id();
        match client.encryption().get_user_devices(uid).await {
            Ok(ud) => {
                let n = ud.devices().count();
                tracing::info!("e2ee:   {uid} → {n} device(s)");
                all_user_devices.push(ud);
            }
            Err(e) => {
                tracing::warn!("e2ee: failed to get devices for {uid}: {e}");
            }
        }
    }

    let recipient_devices: Vec<Device> = all_user_devices
        .iter()
        .flat_map(|ud| ud.devices())
        .collect();

    if recipient_devices.is_empty() {
        tracing::warn!("e2ee: no recipient devices found — key broadcast skipped (0 devices across {} user(s))", other_members.len());
        return Ok(());
    }

    tracing::info!("e2ee: sending key to {} device(s)", recipient_devices.len());
    let refs: Vec<&Device> = recipient_devices.iter().collect();
    let failures = client
        .encryption()
        .encrypt_and_send_raw_to_device(
            refs,
            "io.element.call.encryption_keys",
            raw_content,
            CollectStrategy::AllDevices,
        )
        .await
        .context("send Olm-encrypted to-device encryption keys")?;
    if failures.is_empty() {
        tracing::info!("e2ee: key broadcast complete (all devices reached)");
    } else {
        tracing::warn!(
            "e2ee: key broadcast had {} device failure(s): {failures:?}",
            failures.len()
        );
    }
    Ok(())
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
