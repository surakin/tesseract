//! Recovery / key backup / Megolm room-key file export+import.
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

use std::sync::Arc;

use super::{err, ok, ClientFfi};

#[cfg(test)]
use super::backup_progress_default;

use crate::ffi::{BackupProgress, OpResult};

#[cfg(not(test))]
use std::sync::atomic::Ordering;

impl ClientFfi {
    // -----------------------------------------------------------------------
    // Recovery / key backup (Step 6)
    // -----------------------------------------------------------------------

    /// Returns true when this device is missing the cross-signing / backup
    /// secrets that are already present in server-side secret storage. The
    /// UI surfaces a "Verify this device" banner when this is true.
    #[cfg(not(test))]
    pub fn needs_recovery(&self) -> bool {
        let Some(client) = self.client.as_ref() else {
            return false;
        };
        // `Recovery::state()` is a synchronous observable read; no runtime
        // block_on needed (the previous block_on of an already-ready future
        // added no value and ran on the UI thread).
        use matrix_sdk::encryption::recovery::RecoveryState;
        matches!(
            client.encryption().recovery().state(),
            RecoveryState::Incomplete
        )
    }

    #[cfg(test)]
    pub fn needs_recovery(&self) -> bool {
        false
    }

    /// Returns the current recovery state as a u8:
    /// 0 = Unknown, 1 = Disabled, 2 = Enabled, 3 = Incomplete.
    /// Cheap synchronous read; no block_on needed.
    #[cfg(not(test))]
    pub fn recovery_state(&self) -> u8 {
        let Some(client) = self.client.as_ref() else {
            return 0;
        };
        use matrix_sdk::encryption::recovery::RecoveryState;
        match client.encryption().recovery().state() {
            RecoveryState::Unknown    => 0,
            RecoveryState::Disabled   => 1,
            RecoveryState::Enabled    => 2,
            RecoveryState::Incomplete => 3,
        }
    }

    #[cfg(test)]
    pub fn recovery_state(&self) -> u8 {
        0
    }

    /// Whether a cross-signing identity already exists for our own user.
    /// Read from the local crypto store only (no network request — see
    /// `Encryption::get_user_identity`). Lets the UI distinguish a truly fresh
    /// account (no identity → safe to bootstrap a new one) from one where
    /// cross-signing was set up on another device and this one can't bootstrap
    /// over the top — it must be verified against the existing identity.
    #[cfg(not(test))]
    pub fn own_identity_exists(&self) -> bool {
        let Some(client) = self.client.clone() else {
            return false;
        };
        self.rt.block_on(async move {
            let Some(uid) = client.user_id().map(|u| u.to_owned()) else {
                return false;
            };
            matches!(
                client.encryption().get_user_identity(&uid).await,
                Ok(Some(_))
            )
        })
    }

    #[cfg(test)]
    pub fn own_identity_exists(&self) -> bool {
        false
    }

    /// Whether this device is currently cross-signed / verified. Synchronous
    /// local read of the matrix-sdk verification state.
    #[cfg(not(test))]
    pub fn device_verified(&self) -> bool {
        let Some(client) = self.client.as_ref() else {
            return false;
        };
        use matrix_sdk::encryption::VerificationState;
        matches!(
            client.encryption().verification_state().next_now(),
            VerificationState::Verified
        )
    }

    #[cfg(test)]
    pub fn device_verified(&self) -> bool {
        false
    }

    /// Whether the cross-signing PRIVATE keys are present in the local store
    /// (i.e. this device created the identity at login, or has since recovered
    /// it). Distinct from `own_identity_exists()`, which only checks the PUBLIC
    /// identity synced from the server. This lets the UI tell a freshly
    /// bootstrapped first device (keys present → Fresh setup) from a foreign
    /// identity created on another device (keys absent → Recover), without
    /// depending on the timing of `verification_state()` flipping to Verified.
    #[cfg(not(test))]
    pub fn have_cross_signing_keys(&self) -> bool {
        let Some(client) = self.client.clone() else {
            return false;
        };
        self.rt.block_on(async move {
            client
                .encryption()
                .cross_signing_status()
                .await
                .is_some_and(|s| s.is_complete())
        })
    }

    #[cfg(test)]
    pub fn have_cross_signing_keys(&self) -> bool {
        false
    }

    /// Unlock the server-side secret storage with the supplied recovery key
    /// (or passphrase), importing the cross-signing private keys and the
    /// backup decryption key into this device. The actual backup download
    /// runs asynchronously; observe `on_backup_progress` for progress.
    #[cfg(not(test))]
    pub fn recover(&self, key_or_passphrase: &str) -> OpResult {
        use matrix_sdk::encryption::{
            recovery::RecoveryError, secret_storage::SecretStorageError,
        };
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        if key_or_passphrase.trim().is_empty() {
            return err("recovery key or passphrase is empty");
        }
        let key = key_or_passphrase.to_owned();
        match self
            .rt
            .block_on(async move { client.encryption().recovery().recover(&key).await })
        {
            Ok(()) => {
                // Unlike enable_recovery(), recover() has no progress stream, so
                // drive the encryption-setup overlay to its terminal Done step
                // explicitly. Without this the overlay sits on the Progress
                // spinner forever after a successful key import (the shells only
                // wire the error branch). step=4 maps to EnableProgress::Done.
                if let Some(h) = self.handler.as_ref() {
                    {
                        let g = h.lock();
                        g.on_enable_recovery_progress(4, "", 0, 0);
                    }
                }
                ok("")
            }
            Err(RecoveryError::SecretStorage(SecretStorageError::MissingKeyInfo { .. })) => err(
                "No recovery key is configured for this account. \
                 Please verify this device using another signed-in device instead.",
            ),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn recover(&self, _key_or_passphrase: &str) -> OpResult {
        err("not logged in")
    }

    /// Bootstrap cross-signing + key backup for a fresh account.
    /// `passphrase`: empty = generate a random recovery key; non-empty = derive
    /// the key from this passphrase (the raw key is NOT reported via the callback
    /// in that case — `recovery_key` in the Done callback will be empty).
    /// Progress is reported via `on_enable_recovery_progress` before this returns.
    #[cfg(not(test))]
    pub fn enable_recovery(&self, passphrase: &str) -> OpResult {
        use matrix_sdk::encryption::recovery::{EnableProgress, RecoveryError};
        use futures_util::StreamExt as _;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(handler) = self.handler.as_ref().map(Arc::clone) else {
            return err("sync not started");
        };
        let passphrase = passphrase.to_owned();

        self.rt.block_on(async move {
            // Make the Fresh path self-contained. The login-time auto-bootstrap
            // of cross-signing can be cancelled (e.g. the session is invalidated
            // server-side while the SQLite write is in flight, surfacing as
            // SQLITE_ABORT), leaving this device without a cross-signing
            // identity — so recovery().enable() would store nothing and the
            // device would never verify. Re-run the bootstrap here, at a stable
            // user-driven moment. It is a no-op when an identity already exists
            // (incl. one set up on another device — that case is routed to the
            // Recover flow, not here). OAuth/OIDC servers permit the initial
            // upload without extra UIA.
            if let Err(e) =
                client.encryption().bootstrap_cross_signing_if_needed(None).await
            {
                // This only does work (and can fail) when no cross-signing
                // identity exists yet — it is a no-op when one is already
                // present. So a failure here means this device has no identity
                // and recovery().enable() would store nothing and report a
                // false success with an unverified device. Abort with the error
                // instead of silently continuing.
                tracing::warn!("enable_recovery: bootstrap_cross_signing failed: {e:?}");
                {
                    let g = handler.lock();
                    g.on_enable_recovery_progress(5, &e.to_string(), 0, 0);
                }
                return err(e.to_string());
            }

            let recovery = client.encryption().recovery();

            // `recovery().enable()` refuses to run when a key backup already
            // exists on the server but is not enabled on this device, returning
            // BackupExistsOnServer. That happens with an orphaned backup left by
            // a prior reset; it is unreachable from this device (no secret
            // storage / backup key), so delete it and retry once.
            let mut deleted_stale_backup = false;

            loop {
                let enable = if passphrase.is_empty() {
                    recovery.enable().wait_for_backups_to_upload()
                } else {
                    recovery
                        .enable()
                        .wait_for_backups_to_upload()
                        .with_passphrase(&passphrase)
                };

                let mut progress_stream = enable.subscribe_to_progress();
                let handler2 = Arc::clone(&handler);
                let progress_task = tokio::spawn(async move {
                    while let Some(update) = progress_stream.next().await {
                        let Ok(progress) = update else { break };
                        let (step, key, bu, tot): (u8, String, u32, u32) = match progress {
                            EnableProgress::Starting => (0, String::new(), 0, 0),
                            EnableProgress::CreatingBackup => (1, String::new(), 0, 0),
                            EnableProgress::CreatingRecoveryKey => (2, String::new(), 0, 0),
                            EnableProgress::BackingUp(ref counts) => {
                                (3, String::new(), counts.backed_up as u32, counts.total as u32)
                            }
                            EnableProgress::RoomKeyUploadError => (6, String::new(), 0, 0),
                            EnableProgress::Done { ref recovery_key } => {
                                (4, recovery_key.clone(), 0, 0)
                            }
                        };
                        {
                            let g = handler2.lock();
                            g.on_enable_recovery_progress(step, &key, bu, tot);
                        }
                    }
                });

                match enable.await {
                    Ok(_recovery_key) => {
                        let _ = progress_task.await;
                        return ok(""); // key delivered via progress step=4
                    }
                    Err(RecoveryError::BackupExistsOnServer) if !deleted_stale_backup => {
                        progress_task.abort();
                        let _ = progress_task.await;
                        // Delete the orphaned server backup, then retry enable().
                        if let Err(e) =
                            client.encryption().backups().disable_and_delete().await
                        {
                            {
                                let g = handler.lock();
                                g.on_enable_recovery_progress(5, &e.to_string(), 0, 0);
                            }
                            return err(e.to_string());
                        }
                        deleted_stale_backup = true;
                        // loop and retry once
                    }
                    Err(e) => {
                        progress_task.abort();
                        let _ = progress_task.await;
                        {
                            let g = handler.lock();
                            g.on_enable_recovery_progress(5, &e.to_string(), 0, 0);
                        }
                        return err(e.to_string());
                    }
                }
            }
        })
    }

    #[cfg(test)]
    pub fn enable_recovery(&self, _passphrase: &str) -> OpResult {
        err("not logged in")
    }

    /// Snapshot of the current backup state plus the running imported-key
    /// counter. Cheap; reads atomic state populated by the watcher task.
    #[cfg(not(test))]
    pub fn backup_state(&self) -> BackupProgress {
        BackupProgress {
            state: self.backup_state_code.load(Ordering::Relaxed),
            imported_keys: self.imported_keys.load(Ordering::Relaxed),
            total_keys: 0,
        }
    }

    #[cfg(test)]
    pub fn backup_state(&self) -> BackupProgress {
        backup_progress_default()
    }

    // -----------------------------------------------------------------------
    // Room key file export / import (Megolm export format)
    // -----------------------------------------------------------------------

    /// Export all Megolm room keys to the passphrase-encrypted file at
    /// `path`. The output uses the standard Matrix key-export format so the
    /// file can be imported by any compatible Matrix client.
    #[cfg(not(test))]
    pub fn export_room_keys(&self, path: &str, passphrase: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        if passphrase.is_empty() {
            return err("passphrase must not be empty");
        }
        let path_buf = std::path::PathBuf::from(path);
        let pass = passphrase.to_owned();
        match self.rt.block_on(async move {
            client.encryption().export_room_keys(path_buf, &pass, |_| true).await
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn export_room_keys(&self, _path: &str, _passphrase: &str) -> OpResult {
        err("not logged in")
    }

    /// Import Megolm room keys from the passphrase-encrypted file at `path`.
    /// Returns an error string on failure (wrong passphrase, bad file, etc.).
    #[cfg(not(test))]
    pub fn import_room_keys(&self, path: &str, passphrase: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let path_buf = std::path::PathBuf::from(path);
        let pass = passphrase.to_owned();
        match self.rt.block_on(async move {
            client.encryption().import_room_keys(path_buf, &pass).await
        }) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn import_room_keys(&self, _path: &str, _passphrase: &str) -> OpResult {
        err("not logged in")
    }
}
