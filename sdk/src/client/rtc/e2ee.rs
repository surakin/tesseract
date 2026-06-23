#![cfg(feature = "calls")]

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

    /// Broadcast our current key to all joined room members via unencrypted
    /// to-device m.rtc.encryption_key events.
    pub async fn broadcast_own_key(
        &self,
        client: &matrix_sdk::Client,
        room: &matrix_sdk::Room,
        member_id: &str,
    ) -> anyhow::Result<()> {
        use matrix_sdk::ruma::{
            api::client::to_device::send_event_to_device::v3 as to_device_api,
            events::ToDeviceEventType,
            to_device::DeviceIdOrAllDevices,
            TransactionId,
        };
        use std::collections::BTreeMap;

        let content = serde_json::json!({
            "member_id": member_id,
            "media_key": {
                "key": self.own_key_b64(),
                "index": self.own_index,
            }
        });

        let members = room
            .members(matrix_sdk::RoomMemberships::JOIN)
            .await
            .context("get room members for key broadcast")?;
        let own_user = client
            .user_id()
            .ok_or_else(|| anyhow::anyhow!("not logged in"))?;

        let mut messages: BTreeMap<
            matrix_sdk::ruma::OwnedUserId,
            BTreeMap<
                DeviceIdOrAllDevices,
                matrix_sdk::ruma::serde::Raw<
                    matrix_sdk::ruma::events::AnyToDeviceEventContent,
                >,
            >,
        > = BTreeMap::new();

        for member in members {
            let uid = member.user_id().to_owned();
            if uid == own_user {
                continue;
            }
            let raw_value = serde_json::value::to_raw_value(&content)
                .context("serialize encryption key")?;
            let raw = matrix_sdk::ruma::serde::Raw::from_json(raw_value);
            messages
                .entry(uid)
                .or_default()
                .insert(DeviceIdOrAllDevices::AllDevices, raw);
        }

        if messages.is_empty() {
            return Ok(());
        }

        let event_type = ToDeviceEventType::from("org.matrix.msc4143.rtc.encryption_key");
        let txn = TransactionId::new();
        let request = to_device_api::Request::new_raw(event_type, txn, messages);
        client.send(request).await.context("send to-device encryption keys")?;
        Ok(())
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
