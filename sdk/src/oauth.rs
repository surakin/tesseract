//! OAuth 2.0 / Matrix Authentication Service login flow.
//!
//! The desktop client follows the "loopback redirect" pattern from RFC 8252:
//!
//!  1. `begin()` discovers the homeserver, builds the client metadata,
//!     stands up a loopback HTTP listener on 127.0.0.1:<random_port> via
//!     matrix-sdk's `LocalServerBuilder` and asks the SDK for an authorisation
//!     URL with that redirect URI.
//!  2. The C++ side opens the URL in the user's default browser.
//!  3. `await_callback()` awaits the browser's GET to the loopback listener.
//!     The SDK's `LocalServerRedirectHandle` captures the query string and we
//!     hand it to `OAuth::finish_login`, which performs the token exchange and
//!     persists the session inside the SDK's sqlite store.
//!  4. `cancel()` shuts the listener down so a stalled flow can be aborted.

use anyhow::{anyhow, Context as _};
use matrix_sdk::{
    authentication::oauth::{
        registration::{ApplicationType, ClientMetadata, Localized, OAuthGrantType},
        ClientRegistrationData,
    },
    encryption::{BackupDownloadStrategy, EncryptionSettings},
    utils::{
        local_server::{
            LocalServerBuilder, LocalServerIpAddress, LocalServerResponse,
            LocalServerShutdownHandle,
        },
        UrlOrQuery,
    },
    Client, ThreadingSupport,
};
use url::Url;

const HTML_SUCCESS: &str = "<!doctype html><html><head><meta charset='utf-8'>\
<title>Tesseract – signed in</title></head><body style='font-family:sans-serif;\
text-align:center;padding:4em'><h1>You're signed in.</h1>\
<p>You can close this window and return to Tesseract.</p></body></html>";

/// Informational `client_uri` advertised in the OAuth dynamic-registration
/// metadata. The MAS consent page may surface this to the user, so it
/// points at the project page. Must share a host with `LOGO_URI` below —
/// MAS rejects registration when `logo_uri`/`tos_uri`/`policy_uri` have a
/// different host than `client_uri`.
const CLIENT_URI: &str = "https://surakin.github.io/tesseract/";

/// Informational `logo_uri` advertised in the OAuth dynamic-registration
/// metadata. The MAS consent page may display it next to the client name.
const LOGO_URI: &str = "https://surakin.github.io/tesseract/favicon-160.png";

fn build_device_display_name() -> String {
    let platform = match std::env::consts::OS {
        "windows" => "Windows",
        "macos"   => "macOS",
        "linux"   => "Linux",
        other     => other,
    };
    format!("Tesseract on {platform}")
}

/// User-Agent string for every HTTP request matrix-sdk makes — both the
/// OAuth-flow client (token exchange, refresh, `PUT /devices/{id}`) and the
/// long-lived post-login client (sync, send, etc.). Identifies the install
/// in homeserver / MAS access logs even before the device-display-name
/// rename request fires.
pub(crate) fn build_user_agent() -> String {
    format!("Tesseract/{} ({})", env!("CARGO_PKG_VERSION"), std::env::consts::OS)
}

/// Shared HTTP client for matrix-sdk instances. Uses a 10s connect timeout
/// (fast TCP failure) and a 60s total request timeout (longer than the 30s
/// sync long-poll but short enough to surface stalled media downloads faster
/// than the outer tokio::select! backstop).
///
/// Must use `matrix_sdk::reqwest` (reqwest 0.13) — matrix-sdk re-exports its
/// own reqwest version, which is a different crate version than the reqwest
/// 0.12 used for tile/URL fetches. `ClientBuilder::http_client` only accepts
/// the matrix-sdk re-export.
pub(crate) fn build_sdk_http_client() -> matrix_sdk::reqwest::Client {
    matrix_sdk::reqwest::Client::builder()
        .user_agent(build_user_agent())
        .connect_timeout(std::time::Duration::from_secs(10))
        .timeout(std::time::Duration::from_secs(60))
        .http2_adaptive_window(true)
        // Default H2 stream window is 64 KB — too small for large media; 4 MB eliminates
        // WINDOW_UPDATE stalls on multi-megabyte images and videos.
        .http2_initial_stream_window_size(4 * 1024 * 1024)
        // 16 MB connection window allows all 18 concurrent streams to saturate the pipe
        // without the connection-level flow-control becoming the bottleneck.
        .http2_initial_connection_window_size(16 * 1024 * 1024)
        .build()
        .unwrap_or_else(|_| matrix_sdk::reqwest::Client::new())
}

/// State carried between `begin` and `await_callback`.
pub struct PendingFlow {
    /// The half-built client; `finish_login` populates it with tokens.
    pub client: Client,
    /// Handle that resolves to the redirect query string when the browser
    /// hits the loopback server; consumed by `await_callback`.
    pub redirect_handle: matrix_sdk::utils::local_server::LocalServerRedirectHandle,
    /// Shutdown handle cloned from `redirect_handle`; used by `cancel()`.
    pub shutdown_handle: LocalServerShutdownHandle,
}

/// Output of `begin`.
pub struct BeginResult {
    pub auth_url: String,
    pub redirect_uri: String,
    pub flow: PendingFlow,
}

/// Phase 1 — bind the loopback socket, register the client, build the auth URL.
pub async fn begin(
    homeserver: &str,
    sqlite_path: &std::path::Path,
    register: bool,
) -> anyhow::Result<BeginResult> {
    // 1. Bind a loopback redirect server on a random port. We need the port
    //    before asking the SDK to embed it in the redirect URI, so spawn the
    //    server first.
    let (base_url, redirect_handle) = LocalServerBuilder::new()
        .ip_address(LocalServerIpAddress::Localhostv4)
        .response(LocalServerResponse::Html(HTML_SUCCESS.to_owned()))
        .spawn()
        .await
        .context("bind loopback redirect server")?;
    let shutdown_handle = redirect_handle.shutdown_handle();

    let mut redirect_url = base_url.clone();
    redirect_url.set_path("callback");
    let redirect_uri = redirect_url.to_string();

    // 2. Build the SDK client. `server_name_or_homeserver_url` accepts
    //    either `matrix.org` or `https://matrix.org` and performs
    //    well-known discovery.
    let client = Client::builder()
        .server_name_or_homeserver_url(homeserver)
        .sqlite_store(sqlite_path, None)
        .handle_refresh_tokens()
        .user_agent(build_user_agent())
        .http_client(build_sdk_http_client())
        .with_encryption_settings(EncryptionSettings {
            backup_download_strategy: BackupDownloadStrategy::AfterDecryptionFailure,
            // Bootstrap cross-signing automatically on first login so a fresh
            // account's device is cross-signed (i.e. verified) without manual
            // steps. matrix-sdk's recovery().enable() — run by the encryption
            // setup wizard — only stores EXISTING cross-signing keys into secret
            // storage; it never creates them. Without this the device stays
            // unverified and RecoveryState is stuck at Incomplete. OAuth/OIDC
            // servers permit the initial cross-signing key upload without UIA.
            auto_enable_cross_signing: true,
            ..Default::default()
        })
        // matrix-sdk's room event cache only routes sync'd thread events to
        // per-thread linked chunks (which our focused thread Timeline
        // subscribes to) when threading support is enabled. Without this,
        // the open thread panel never sees new replies arriving via sync.
        .with_threading_support(ThreadingSupport::Enabled {
            with_subscriptions: false,
        })
        .build()
        .await
        .context("build matrix-sdk Client")?;

    // 3. Native-app client metadata for dynamic registration.
    let metadata = ClientMetadata {
        client_name: Some(Localized::new("Tesseract".to_owned(), [])),
        logo_uri: Some(Localized::new(Url::parse(LOGO_URI).context("logo_uri")?, [])),
        ..ClientMetadata::new(
            ApplicationType::Native,
            vec![OAuthGrantType::AuthorizationCode {
                redirect_uris: vec![redirect_url.clone()],
            }],
            // client_uri – purely informational; required to be a valid URL.
            Localized::new(Url::parse(CLIENT_URI).context("client_uri")?, []),
        )
    };
    let raw_metadata = matrix_sdk::ruma::serde::Raw::new(&metadata)
        .map_err(|e| anyhow!("serialise client metadata: {e}"))?;
    let registration_data = ClientRegistrationData::new(raw_metadata);

    // 4. Fail fast if the homeserver does not support Simplified Sliding Sync.
    //    Tesseract has a single sync code path (no v2 fallback).
    probe_sss_support(&client).await?;

    // 5. Ask the SDK for the authorisation URL. PKCE verifier, state, and
    //    nonce are generated inside `OAuth::login()`; they're held in the
    //    `Client` until `finish_login` is called on the same instance.
    let mut login_builder = client.oauth().login(
        redirect_url,
        None, /* device_id */
        Some(registration_data),
        None, /* additional_scopes */
    );
    if register {
        use matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::Prompt;
        login_builder = login_builder.prompt(vec![Prompt::Create]);
    }
    let mut auth_data = login_builder
        .build()
        .await
        .context("oauth login() — does the homeserver support OAuth?")?;

    auth_data
        .url
        .query_pairs_mut()
        .append_pair("device_display_name", &build_device_display_name());

    Ok(BeginResult {
        auth_url: auth_data.url.to_string(),
        redirect_uri,
        flow: PendingFlow { client, redirect_handle, shutdown_handle },
    })
}

/// Phase 2 — await the browser's loopback redirect, then ask the SDK to
/// finalise the login.
pub async fn await_callback(flow: PendingFlow) -> anyhow::Result<Client> {
    let PendingFlow { client, redirect_handle, .. } = flow;

    let query = redirect_handle
        .await
        .ok_or_else(|| anyhow!("oauth flow cancelled or redirect server error"))?;

    client
        .oauth()
        .finish_login(UrlOrQuery::Query(query.0))
        .await
        .context("oauth finish_login")?;

    // Best-effort: set the device display name so the user can tell sessions
    // apart. ClientMetadata.client_name only feeds the OAuth consent page;
    // the device display name comes from PUT /devices.
    if let Some(device_id) = client.device_id() {
        let display_name = build_device_display_name();
        if let Err(e) = client.rename_device(device_id, &display_name).await {
            tracing::warn!("rename_device({display_name:?}) failed: {e}");
        }
    }

    Ok(client)
}

/// Check that the homeserver supports Simplified Sliding Sync.
/// Tesseract has no v2-sync fallback; login/restore must fail clearly here
/// rather than surfacing a cryptic error later inside SyncService::start().
async fn probe_sss_support(client: &Client) -> anyhow::Result<()> {
    let versions = client.available_sliding_sync_versions().await;
    let has_native = versions
        .iter()
        .any(|v| matches!(v, matrix_sdk::sliding_sync::Version::Native));
    if !has_native {
        anyhow::bail!(
            "This homeserver does not support Simplified Sliding Sync (SSS). \
             Tesseract requires SSS support (Synapse ≥ 1.110 or Conduit ≥ 0.7). \
             Please use a compatible homeserver."
        );
    }
    Ok(())
}

/// Trip the cancel flag; the redirect server shuts down immediately.
pub fn cancel(flow: &PendingFlow) {
    flow.shutdown_handle.clone().shutdown();
}

/// Best-effort check whether `homeserver` allows account registration via the
/// OIDC `prompt=create` flow. Builds a throwaway client and inspects the OAuth
/// authorization-server metadata. Returns `false` on any error (non-OAuth
/// server, network failure, missing metadata).
pub async fn supports_registration(homeserver: &str) -> bool {
    use matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::Prompt;
    let client = match Client::builder()
        .server_name_or_homeserver_url(homeserver)
        .user_agent(build_user_agent())
        .http_client(build_sdk_http_client())
        .build()
        .await
    {
        Ok(c) => c,
        Err(_) => return false,
    };
    match client.oauth().server_metadata().await {
        Ok(meta) => meta.prompt_values_supported.contains(&Prompt::Create),
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn html_success_mentions_signed_in() {
        assert!(HTML_SUCCESS.contains("signed in"));
    }
}
