//! Recovery / key backup / Megolm room-key file export+import.
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

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

    /// Unlock the server-side secret storage with the supplied recovery key
    /// (or passphrase), importing the cross-signing private keys and the
    /// backup decryption key into this device. The actual backup download
    /// runs asynchronously; observe `on_backup_progress` for progress.
    #[cfg(not(test))]
    pub fn recover(&mut self, key_or_passphrase: &str) -> OpResult {
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
            Ok(()) => ok(""),
            Err(RecoveryError::SecretStorage(SecretStorageError::MissingKeyInfo { .. })) => err(
                "No recovery key is configured for this account. \
                 Please verify this device using another signed-in device instead.",
            ),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn recover(&mut self, _key_or_passphrase: &str) -> OpResult {
        err("not logged in")
    }

    /// Bootstrap cross-signing + key backup for a fresh account.
    /// `passphrase`: empty = generate a random recovery key; non-empty = derive
    /// the key from this passphrase (the raw key is NOT reported via the callback
    /// in that case — `recovery_key` in the Done callback will be empty).
    /// Progress is reported via `on_enable_recovery_progress` before this returns.
    #[cfg(not(test))]
    pub fn enable_recovery(&mut self, passphrase: &str) -> OpResult {
        use matrix_sdk::encryption::recovery::EnableProgress;
        use futures_util::StreamExt as _;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(handler) = self.handler.as_ref().map(Arc::clone) else {
            return err("sync not started");
        };
        let passphrase = passphrase.to_owned();

        self.rt.block_on(async move {
            let recovery = client.encryption().recovery();
            let enable = if passphrase.is_empty() {
                recovery.enable()
            } else {
                recovery.enable().with_passphrase(&passphrase)
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
                        EnableProgress::BackingUp(counts) => {
                            (3, String::new(), counts.backed_up as u32, counts.total as u32)
                        }
                        EnableProgress::RoomKeyUploadError => (5, String::new(), 0, 0),
                        EnableProgress::Done { recovery_key } => (4, recovery_key, 0, 0),
                    };
                    if let Ok(g) = handler2.lock() {
                        g.on_enable_recovery_progress(step, &key, bu, tot);
                    }
                }
            });

            match enable.await {
                Ok(recovery_key) => {
                    let _ = progress_task.await;
                    ok(recovery_key)
                }
                Err(e) => {
                    progress_task.abort();
                    if let Ok(g) = handler.lock() {
                        g.on_enable_recovery_progress(5, &e.to_string(), 0, 0);
                    }
                    err(e.to_string())
                }
            }
        })
    }

    #[cfg(test)]
    pub fn enable_recovery(&mut self, _passphrase: &str) -> OpResult {
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
    pub fn export_room_keys(&mut self, path: &str, passphrase: &str) -> OpResult {
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
    pub fn export_room_keys(&mut self, _path: &str, _passphrase: &str) -> OpResult {
        err("not logged in")
    }

    /// Import Megolm room keys from the passphrase-encrypted file at `path`.
    /// Returns an error string on failure (wrong passphrase, bad file, etc.).
    #[cfg(not(test))]
    pub fn import_room_keys(&mut self, path: &str, passphrase: &str) -> OpResult {
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
    pub fn import_room_keys(&mut self, _path: &str, _passphrase: &str) -> OpResult {
        err("not logged in")
    }
}
