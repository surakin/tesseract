//! Cross-signing identity reset — Element's "Reset cryptographic identity".
//!
//! Resets the user's cross-signing keys via matrix-sdk's
//! `Encryption::reset_cross_signing`. On the app's OAuth/OIDC homeservers the
//! reset must be approved by the user in a browser: `reset_cross_signing`
//! returns a handle whose `auth_type()` is `OAuth { approval_url }`. The UI
//! opens that URL and the SDK polls the server (up to ~2 minutes) until the
//! user approves. That poll runs as a spawned task — NOT a blocking FFI call —
//! so a concurrent `cancel_reset_crypto_identity` isn't stuck behind the C++
//! wrapper mutex (`MUT_FFI`) that each FFI call holds for its duration.

#[cfg(not(test))]
use std::sync::Arc;

#[cfg(not(test))]
use super::ClientFfi;
#[cfg(not(test))]
use crate::ffi::CryptoResetBegin;

#[cfg(not(test))]
impl ClientFfi {
    pub fn begin_reset_crypto_identity(&mut self) -> CryptoResetBegin {
        use matrix_sdk::encryption::CrossSigningResetAuthType;

        let fail = |message: String| CryptoResetBegin {
            ok: false,
            message,
            needs_approval: false,
            approval_url: String::new(),
        };

        let Some(client) = self.client.clone() else {
            return fail("not logged in".into());
        };
        let Some(handler) = self.handler.as_ref().map(Arc::clone) else {
            return fail("sync not started".into());
        };

        // Drop any handle left over from a previous (abandoned) attempt.
        self.crypto_reset_handle = None;

        let client_for_reset = client.clone();
        let handle = match self
            .rt
            .block_on(async move { client_for_reset.encryption().reset_cross_signing().await })
        {
            // Reset completed immediately with no further auth required.
            Ok(None) => {
                return CryptoResetBegin {
                    ok: true,
                    message: String::new(),
                    needs_approval: false,
                    approval_url: String::new(),
                }
            }
            Ok(Some(h)) => h,
            Err(e) => return fail(e.to_string()),
        };

        let approval_url = match handle.auth_type() {
            CrossSigningResetAuthType::OAuth(info) => info.approval_url.to_string(),
            // This client authenticates only via OAuth/OIDC; a password-based
            // UIA reset flow isn't wired. Bail rather than half-complete.
            CrossSigningResetAuthType::Uiaa(_) => {
                return fail(
                    "This server requires a password to reset your identity, \
                     which isn't supported here."
                        .into(),
                )
            }
        };

        let handle = Arc::new(handle);
        self.crypto_reset_handle = Some(Arc::clone(&handle));

        // Poll for the user's browser approval off the FFI mutex.
        self.rt.spawn(async move {
            let res = handle.auth(None).await;
            if let Ok(g) = handler.lock() {
                match res {
                    Ok(()) => g.on_crypto_reset_result(true, ""),
                    Err(e) => g.on_crypto_reset_result(false, &e.to_string()),
                }
            }
        });

        CryptoResetBegin {
            ok: true,
            message: String::new(),
            needs_approval: true,
            approval_url,
        }
    }

    pub fn cancel_reset_crypto_identity(&mut self) {
        if let Some(handle) = self.crypto_reset_handle.take() {
            self.rt.block_on(async move { handle.cancel().await });
        }
    }
}
