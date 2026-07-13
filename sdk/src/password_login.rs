//! Legacy `m.login.password` login — a fallback for homeservers that don't
//! run an OIDC/MAS provider (see [`crate::oauth`] for the OAuth counterpart).
//!
//! Shares [`crate::oauth::build_configured_client`] with the OAuth flow so
//! store config, encryption settings, and threading support can never drift
//! between the two login paths, and enforces the same Simplified Sliding
//! Sync requirement via [`crate::oauth::probe_sss_support`] before any
//! credential is sent over the wire.
#![cfg(feature = "legacy_login")]

use anyhow::Context as _;
use matrix_sdk::Client;

/// Log in to `homeserver` with a Matrix user ID (`@user:server`) or bare
/// localpart, plus password. Fails fast if the homeserver doesn't support
/// Simplified Sliding Sync — Tesseract has no v2-sync fallback.
pub async fn login(
    homeserver: &str,
    sqlite_path: &std::path::Path,
    user_id_or_localpart: &str,
    password: &str,
) -> anyhow::Result<Client> {
    let client = crate::oauth::build_configured_client(homeserver, sqlite_path).await?;
    crate::oauth::probe_sss_support(&client).await?;

    client
        .matrix_auth()
        .login_username(user_id_or_localpart, password)
        .initial_device_display_name(&crate::oauth::build_device_display_name())
        .request_refresh_token()
        .send()
        .await
        .context("invalid credentials or unsupported login flow")?;

    Ok(client)
}
