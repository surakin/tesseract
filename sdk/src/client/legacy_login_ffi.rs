// Legacy username/password (m.login.password) FFI methods on ClientFfi.
//
// Always compiled so the cxx bridge declaration is satisfied in both
// legacy-login-enabled and -disabled builds. The bodies use
// `#[cfg(feature = "legacy_login")]` to either call the real implementation
// or return an error, mirroring rtc_ffi.rs's convention for the `calls`
// feature.
#![allow(unused_imports, unused_variables)]

use super::{err, ok, ClientFfi};
use crate::ffi::OpResult;

impl ClientFfi {
    /// Log in with a Matrix user ID (or localpart) and password, as a
    /// fallback for homeservers without OIDC/MAS. Blocks the calling thread —
    /// call only from a worker thread.
    pub fn login_password(&mut self, homeserver: &str, username: &str, password: &str) -> OpResult {
        #[cfg(not(feature = "legacy_login"))]
        return err("legacy login not built into this binary");

        #[cfg(feature = "legacy_login")]
        {
            let hs = homeserver.to_owned();
            let username = username.to_owned();
            let password = password.to_owned();
            let path = self.data_dir.clone();

            // Only start from a clean store when there is no active session.
            // Mirrors oauth_begin's rationale: if a session was already
            // restored, wiping here would silently destroy the live SQLite
            // cache for the restored account.
            if self.client.is_none() {
                let _ = std::fs::remove_dir_all(&path);
            }
            let _ = std::fs::create_dir_all(&path);

            match self
                .rt
                .block_on(crate::password_login::login(&hs, &path, &username, &password))
            {
                Ok(client) => {
                    // Enter the runtime so any prior Client we're overwriting
                    // drops with a tokio context in TLS — matrix-sdk's
                    // SqliteStateStore / deadpool tear-down calls
                    // Handle::current() in its Drop impl.
                    let _guard = self.rt.enter();
                    self.client = Some(client);
                    ok("")
                }
                Err(e) => err(format!("{e:#}")),
            }
        }
    }
}
