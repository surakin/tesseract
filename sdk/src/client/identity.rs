//! MSC4153 "exclude insecure devices" mode plus the per-room identity-change
//! (pinning) warnings it depends on.
//!
//! The mode is a process-wide flag read when a matrix-sdk `Client` is built
//! (`oauth::build_configured_client`), so toggling it takes effect on the
//! next account start. The identity watcher runs regardless of the flag:
//! warning about a changed identity is useful in either mode, and in strict
//! mode a verified user's identity change blocks sending until it is resolved.

use super::{err, ok, ClientFfi};
use crate::ffi::OpResult;

use matrix_sdk_base::crypto::{CollectStrategy, DecryptionSettings, TrustRequirement};
use std::sync::atomic::{AtomicBool, Ordering};

#[cfg(not(test))]
use super::SendHandler;
#[cfg(not(test))]
use parking_lot::Mutex;
#[cfg(not(test))]
use std::sync::Arc;

static EXCLUDE_INSECURE_DEVICES: AtomicBool = AtomicBool::new(false);

/// Set the process-wide MSC4153 mode. Called by the C++ layer at launch (from
/// the persisted setting) and before each in-place client reload; only
/// clients built afterwards see the new value.
pub fn set_exclude_insecure_devices(enabled: bool) {
    EXCLUDE_INSECURE_DEVICES.store(enabled, Ordering::Relaxed);
}

pub(crate) fn exclude_insecure_devices() -> bool {
    EXCLUDE_INSECURE_DEVICES.load(Ordering::Relaxed)
}

static BLOCK_SENDS_ON_UNVERIFIED_DEVICES: AtomicBool = AtomicBool::new(true);

/// Set whether a send is refused when a verified member's identity changed or
/// they have a device their owner did not cross-sign. Same lifecycle as
/// [`set_exclude_insecure_devices`]: read when a client is built.
pub fn set_block_sends_on_unverified_devices(enabled: bool) {
    BLOCK_SENDS_ON_UNVERIFIED_DEVICES.store(enabled, Ordering::Relaxed);
}

pub(crate) fn block_sends_on_unverified_devices() -> bool {
    BLOCK_SENDS_ON_UNVERIFIED_DEVICES.load(Ordering::Relaxed)
}

/// Who receives room keys (and other Olm-encrypted to-device payloads).
/// `IdentityBasedStrategy` shares only with devices their owner cross-signed
/// and refuses to send while a verified user's identity has changed.
pub(crate) fn room_key_recipient_strategy(
    exclude_insecure: bool,
    block_sends: bool,
) -> CollectStrategy {
    if exclude_insecure {
        CollectStrategy::IdentityBasedStrategy
    } else if block_sends {
        // Every device of a member gets the key, but a send is refused when a
        // verified member's identity has changed or they have an unsigned
        // device, rather than sending on past a warning the user may not read
        // (Element X's default).
        CollectStrategy::ErrorOnVerifiedUserProblem
    } else {
        CollectStrategy::AllDevices
    }
}

/// Which senders' messages decrypt. `CrossSignedOrLegacy` still decrypts
/// Megolm sessions received before matrix-sdk recorded sender trust, so old
/// history doesn't turn into UTDs.
pub(crate) fn decryption_settings(exclude_insecure: bool) -> DecryptionSettings {
    DecryptionSettings {
        sender_device_trust_requirement: if exclude_insecure {
            TrustRequirement::CrossSignedOrLegacy
        } else {
            TrustRequirement::Untrusted
        },
    }
}

/// Wire encoding of an identity-violation kind for `on_identity_status_changed`.
pub(crate) const IDENTITY_PIN_VIOLATION: u8 = 1;
pub(crate) const IDENTITY_VERIFICATION_VIOLATION: u8 = 2;

/// Watch `room`'s members for identity changes and report the room's full
/// current set of violations to the UI: once right away (usually empty, which
/// clears any banner left from before) and then on every change. Rooms that
/// turn out not to be encrypted get that one empty report and nothing more.
#[cfg(not(test))]
pub(super) fn spawn_identity_watcher(
    room: &matrix_sdk::Room,
    room_id: String,
    handler: &Arc<Mutex<SendHandler>>,
    rt: &tokio::runtime::Runtime,
    cancelled: Arc<AtomicBool>,
) -> tokio::task::AbortHandle {
    use futures_util::StreamExt;
    use matrix_sdk::ruma::OwnedUserId;
    use matrix_sdk_base::crypto::IdentityState;
    use std::collections::BTreeMap;

    let room = room.clone();
    let h = Arc::clone(handler);
    let task = rt.spawn(async move {
        // user_id → violation kind; users back in Pinned/Verified are removed.
        let mut violations: BTreeMap<OwnedUserId, u8> = BTreeMap::new();
        // encryption_state() is Unknown until the room's state is loaded
        // (fresh joins, sliding sync); this asks the server when needed.
        let encrypted = match room.latest_encryption_state().await {
            Ok(state) => state.is_encrypted(),
            Err(e) => {
                tracing::warn!("encryption state unavailable for {room_id}: {e}");
                false
            }
        };
        // matrix-sdk's stream only yields its initial state when non-empty,
        // so start from an explicit empty report.
        let sent = report_violations(&room, &room_id, &h, &cancelled, &violations).await;
        if !sent || !encrypted {
            return;
        }
        let stream = match room.subscribe_to_identity_status_changes().await {
            Ok(s) => s,
            Err(e) => {
                tracing::warn!("identity status subscription failed for {room_id}: {e}");
                return;
            }
        };
        let mut stream = std::pin::pin!(stream);
        while let Some(changes) = stream.next().await {
            for change in changes {
                match change.changed_to {
                    IdentityState::PinViolation => {
                        violations.insert(change.user_id, IDENTITY_PIN_VIOLATION);
                    }
                    IdentityState::VerificationViolation => {
                        violations.insert(change.user_id, IDENTITY_VERIFICATION_VIOLATION);
                    }
                    IdentityState::Pinned | IdentityState::Verified => {
                        violations.remove(&change.user_id);
                    }
                }
            }
            if !report_violations(&room, &room_id, &h, &cancelled, &violations).await {
                return;
            }
        }
    });
    task.abort_handle()
}

/// Send `room_id`'s current violations to the UI, named as room members.
/// Returns false once the timeline was torn down (nothing is sent then).
#[cfg(not(test))]
async fn report_violations(
    room: &matrix_sdk::Room,
    room_id: &str,
    h: &Arc<Mutex<SendHandler>>,
    cancelled: &AtomicBool,
    violations: &std::collections::BTreeMap<matrix_sdk::ruma::OwnedUserId, u8>,
) -> bool {
    let mut user_ids = Vec::with_capacity(violations.len());
    let mut names = Vec::with_capacity(violations.len());
    let mut kinds = Vec::with_capacity(violations.len());
    for (uid, kind) in violations {
        names.push(super::member_display_name(room, uid).await);
        user_ids.push(uid.to_string());
        kinds.push(*kind);
    }
    if cancelled.load(Ordering::Acquire) {
        return false;
    }
    let guard = h.lock();
    guard.on_identity_status_changed(room_id, &user_ids, &names, &kinds);
    true
}

/// Wire encoding of `get_user_trust`.
pub(crate) const TRUST_UNKNOWN: u8 = 0; // no cross-signing identity known
pub(crate) const TRUST_NOT_VERIFIED: u8 = 1;
pub(crate) const TRUST_VERIFIED: u8 = 2;
pub(crate) const TRUST_VERIFICATION_VIOLATION: u8 = 3; // was verified, identity reset since

/// Map an identity's flags to the `get_user_trust` code. An unacknowledged
/// reset of an unverified identity isn't distinguished here (matrix-sdk's
/// wrapper doesn't expose it); the room's identity banner covers that.
pub(crate) fn trust_code(verified: bool, verification_violation: bool) -> u8 {
    if verified {
        TRUST_VERIFIED
    } else if verification_violation {
        TRUST_VERIFICATION_VIOLATION
    } else {
        TRUST_NOT_VERIFIED
    }
}

impl ClientFfi {
    /// How far `user_id`'s cross-signing identity is trusted (`TRUST_*`).
    /// Reads the local crypto store only (downloads the keys first when the
    /// user isn't tracked yet). Blocks — worker thread.
    #[cfg(not(test))]
    pub fn get_user_trust(&self, user_id: &str) -> u8 {
        let Some(client) = self.client.clone() else {
            return TRUST_UNKNOWN;
        };
        let Ok(uid) = matrix_sdk::ruma::OwnedUserId::try_from(user_id) else {
            return TRUST_UNKNOWN;
        };
        self.rt.block_on(async move {
            let enc = client.encryption();
            let identity = match enc.get_user_identity(&uid).await {
                Ok(Some(i)) => Some(i),
                // Not tracked yet: ask the server once.
                Ok(None) => enc.request_user_identity(&uid).await.ok().flatten(),
                Err(_) => None,
            };
            match identity {
                None => TRUST_UNKNOWN,
                Some(i) => trust_code(i.is_verified(), i.has_verification_violation()),
            }
        })
    }

    #[cfg(test)]
    pub fn get_user_trust(&self, _user_id: &str) -> u8 {
        TRUST_UNKNOWN
    }

    /// Accept `user_id`'s new identity after a pin violation (the user saw the
    /// "identity was reset" warning and chose to continue). Blocks — worker
    /// thread.
    #[cfg(not(test))]
    pub fn pin_user_identity(&self, user_id: &str) -> OpResult {
        self.with_user_identity(user_id, |identity| async move { identity.pin().await })
    }

    #[cfg(test)]
    pub fn pin_user_identity(&self, _user_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Withdraw verification of `user_id` after their identity changed while
    /// verified. The new identity becomes pinned (unverified), which unblocks
    /// sending in exclude-insecure-devices mode. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn withdraw_user_verification(&self, user_id: &str) -> OpResult {
        self.with_user_identity(user_id, |identity| async move {
            identity.withdraw_verification().await
        })
    }

    /// Look up `user_id`'s cross-signing identity and run `op` on it.
    #[cfg(not(test))]
    fn with_user_identity<F, Fut>(&self, user_id: &str, op: F) -> OpResult
    where
        F: FnOnce(matrix_sdk::encryption::identities::UserIdentity) -> Fut,
        Fut: std::future::Future<Output = Result<(), matrix_sdk::encryption::CryptoStoreError>>,
    {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let uid = match matrix_sdk::ruma::OwnedUserId::try_from(user_id) {
            Ok(u) => u,
            Err(e) => return err(format!("invalid user id: {e}")),
        };
        match self.rt.block_on(async move {
            let identity = client
                .encryption()
                .get_user_identity(&uid)
                .await?
                .ok_or_else(|| anyhow::anyhow!("no identity known for user"))?;
            op(identity).await?;
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn withdraw_user_verification(&self, _user_id: &str) -> OpResult {
        err("not logged in")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_mode_refuses_sends_past_a_verified_user_problem() {
        // Every device still gets the key, but a verified member's identity
        // change or unsigned device fails the send instead of being sent past.
        assert!(matches!(
            room_key_recipient_strategy(false, true),
            CollectStrategy::ErrorOnVerifiedUserProblem
        ));
        assert!(matches!(
            decryption_settings(false).sender_device_trust_requirement,
            TrustRequirement::Untrusted
        ));
    }

    #[test]
    fn blocking_off_shares_with_all_devices() {
        assert!(matches!(
            room_key_recipient_strategy(false, false),
            CollectStrategy::AllDevices
        ));
    }

    #[test]
    fn strict_mode_follows_msc4153() {
        // Strict mode wins over the block setting, whichever way it is set.
        for block in [true, false] {
            assert!(matches!(
                room_key_recipient_strategy(true, block),
                CollectStrategy::IdentityBasedStrategy
            ));
        }
        assert!(matches!(
            decryption_settings(true).sender_device_trust_requirement,
            TrustRequirement::CrossSignedOrLegacy
        ));
    }

    #[test]
    fn trust_code_maps_identity_flags() {
        assert_eq!(trust_code(true, false), TRUST_VERIFIED);
        assert_eq!(trust_code(false, true), TRUST_VERIFICATION_VIOLATION);
        assert_eq!(trust_code(false, false), TRUST_NOT_VERIFIED);
    }

    #[test]
    fn flag_round_trips() {
        set_exclude_insecure_devices(true);
        assert!(exclude_insecure_devices());
        set_exclude_insecure_devices(false);
        assert!(!exclude_insecure_devices());
    }

    #[test]
    fn block_flag_defaults_on_and_round_trips() {
        assert!(block_sends_on_unverified_devices());
        set_block_sends_on_unverified_devices(false);
        assert!(!block_sends_on_unverified_devices());
        set_block_sends_on_unverified_devices(true);
        assert!(block_sends_on_unverified_devices());
    }
}
