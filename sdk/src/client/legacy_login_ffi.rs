// Legacy username/password (m.login.password) FFI methods on ClientFfi.
//
// Always compiled so the cxx bridge declaration is satisfied in both
// legacy-login-enabled and -disabled builds. The bodies use
// `#[cfg(feature = "legacy_login")]` to either call the real implementation
// or return an error, mirroring rtc_ffi.rs's convention for the `calls`
// feature.
#![allow(unused_imports, unused_variables)]

use super::{err, ok, ClientFfi};
use crate::ffi::{OAuthBegin, OpResult};

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

            // Same rationale as oauth_begin: a fresh login always gets a
            // fresh store-encryption key.
            let key = *self
                .store_key
                .get_or_insert_with(crate::oauth::generate_store_key);

            match self.rt.block_on(crate::password_login::login(
                &hs, &path, &username, &password, &key,
            )) {
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

    /// Begin the legacy SSO login. Returns the homeserver SSO URL to open in a
    /// browser; the redirect is awaited with the shared `oauth_await_slice` /
    /// `oauth_commit` / `oauth_cancel` calls. `idp_id` "" selects the generic
    /// SSO redirect. Blocks the calling thread — worker thread only.
    pub fn sso_begin(
        &mut self,
        homeserver: &str,
        idp_id: &str,
        external_redirect: bool,
    ) -> OAuthBegin {
        #[cfg(not(feature = "legacy_login"))]
        return OAuthBegin {
            ok: false,
            message: "legacy login not built into this binary".into(),
            auth_url: String::new(),
            redirect_uri: String::new(),
        };

        #[cfg(feature = "legacy_login")]
        {
            let hs = homeserver.to_owned();
            let idp = idp_id.to_owned();
            let mode = if external_redirect {
                crate::oauth::RedirectMode::External
            } else {
                crate::oauth::RedirectMode::Loopback
            };
            self.begin_login_flow(move |path, key| async move {
                crate::sso_login::begin(&hs, &path, &idp, &key, mode).await
            })
        }
    }
}

/// Fetch an IdP icon (`mxc://`) for the login screen. Empty on any error,
/// timeout (~10 s) or an icon over 256 KiB. Blocks — worker thread. Takes no
/// `ClientFfi`: it runs on its own throwaway runtime so a slow icon never
/// holds the FFI lock.
pub fn fetch_login_icon(homeserver: &str, mxc: &str) -> Vec<u8> {
    #[cfg(not(feature = "legacy_login"))]
    return Vec::new();

    #[cfg(feature = "legacy_login")]
    {
        // catch_unwind keeps a panic from crossing the cxx boundary.
        std::panic::catch_unwind(|| {
            let Ok(rt) = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
            else {
                return Vec::new();
            };
            // Keep a tokio context in TLS so the throwaway Client drops with
            // a runtime handle available (same hazard as supports_registration).
            let _guard = rt.enter();
            let fetch = crate::sso_login::fetch_icon(homeserver, mxc);
            let timeout = std::time::Duration::from_secs(10);
            match rt.block_on(tokio::time::timeout(timeout, fetch)) {
                Ok(Ok(bytes)) if bytes.len() <= 256 * 1024 => bytes,
                _ => Vec::new(),
            }
        })
        .unwrap_or_default()
    }
}
