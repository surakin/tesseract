//! OAuth 2.0 / Matrix Authentication Service login flow.
//!
//! The desktop client follows the "loopback redirect" pattern from RFC 8252:
//!
//!  1. `begin()` discovers the homeserver, builds the client metadata,
//!     stands up a `tiny_http` listener on `127.0.0.1:<random_port>` and
//!     asks the SDK for an authorisation URL with that redirect URI.
//!  2. The C++ side opens the URL in the user's default browser.
//!  3. `await_callback()` blocks the worker thread until the user finishes
//!     authentication and the browser GETs `/callback?code=…&state=…` on
//!     our loopback listener. We hand the full URL to `OAuth::finish_login`
//!     which performs the token exchange and persists the session inside
//!     the SDK's sqlite store.
//!  4. `cancel()` shuts the listener down so a stalled flow can be aborted.

use std::{
    net::{SocketAddr, TcpListener},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread,
    time::Duration,
};

use anyhow::{anyhow, bail, Context as _};
use matrix_sdk::{
    authentication::oauth::{
        registration::{
            ApplicationType, ClientMetadata, Localized, OAuthGrantType,
        },
        ClientRegistrationData,
    },
    utils::UrlOrQuery,
    Client,
};
use tiny_http::{Method, Response, Server};
use url::Url;

const HTML_SUCCESS: &str = "<!doctype html><html><head><meta charset='utf-8'>\
<title>Tesseract – signed in</title></head><body style='font-family:sans-serif;\
text-align:center;padding:4em'><h1>You're signed in.</h1>\
<p>You can close this window and return to Tesseract.</p></body></html>";

const HTML_FAILURE: &str = "<!doctype html><html><head><meta charset='utf-8'>\
<title>Tesseract – sign-in failed</title></head><body style='font-family:sans-serif;\
text-align:center;padding:4em'><h1>Sign-in failed.</h1>\
<p>Return to Tesseract for details.</p></body></html>";

/// State carried between `begin` and `await_callback`.
pub struct PendingFlow {
    /// The half-built client; `finish_login` populates it with tokens.
    pub client:        Client,
    /// Listener that owns the local TCP socket; consumed by `await_callback`.
    pub server:        Arc<Server>,
    /// Path component the SDK was configured to redirect to.
    pub redirect_path: String,
    /// Cooperative cancel flag; tripped by `cancel()`.
    pub cancel:        Arc<AtomicBool>,
}

/// Output of `begin`.
pub struct BeginResult {
    pub auth_url:     String,
    pub redirect_uri: String,
    pub flow:         PendingFlow,
}

/// Phase 1 — bind the loopback socket, register the client, build the auth URL.
pub async fn begin(
    homeserver: &str,
    sqlite_path: &std::path::Path,
) -> anyhow::Result<BeginResult> {
    // 1. Pick an ephemeral port. We bind first so we know the port number
    //    *before* asking the SDK to embed it in the redirect URI.
    let std_listener = TcpListener::bind("127.0.0.1:0")
        .context("bind loopback redirect listener")?;
    std_listener.set_nonblocking(false).ok();
    let local_addr: SocketAddr = std_listener.local_addr()?;
    let server = Arc::new(
        Server::from_listener(std_listener, None)
            .map_err(|e| anyhow!("tiny_http: {e}"))?,
    );

    let redirect_path = "/callback".to_owned();
    let redirect_uri  = format!("http://{local_addr}{redirect_path}");
    let redirect_url  = Url::parse(&redirect_uri).expect("redirect URI parse");

    // 2. Build the SDK client. `server_name_or_homeserver_url` accepts
    //    either `matrix.org` or `https://matrix.org` and performs
    //    well-known discovery.
    let client = Client::builder()
        .server_name_or_homeserver_url(homeserver)
        .sqlite_store(sqlite_path, None)
        .handle_refresh_tokens()
        .build()
        .await
        .context("build matrix-sdk Client")?;

    // 3. Native-app client metadata for dynamic registration.
    let metadata = ClientMetadata {
        client_name: Some(Localized::new("Tesseract".to_owned(), [])),
        ..ClientMetadata::new(
            ApplicationType::Native,
            vec![OAuthGrantType::AuthorizationCode {
                redirect_uris: vec![redirect_url.clone()],
            }],
            // client_uri – purely informational; required to be a valid URL.
            Localized::new(
                Url::parse("https://github.com/").expect("client_uri"),
                [],
            ),
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
    let auth_data = client
        .oauth()
        .login(redirect_url, None /* device_id */, Some(registration_data), None /* additional_scopes */)
        .build()
        .await
        .context("oauth login() — does the homeserver support OAuth?")?;

    Ok(BeginResult {
        auth_url:     auth_data.url.to_string(),
        redirect_uri,
        flow:         PendingFlow {
            client,
            server,
            redirect_path,
            cancel: Arc::new(AtomicBool::new(false)),
        },
    })
}

/// Phase 2 — block the worker thread until the browser hits the loopback
/// redirect, then ask the SDK to finalise the login.
pub async fn await_callback(flow: PendingFlow) -> anyhow::Result<Client> {
    let server  = flow.server.clone();
    let cancel  = flow.cancel.clone();
    let path    = flow.redirect_path.clone();

    // tiny_http is sync; run it in a blocking task and return the captured
    // query string.
    let query: anyhow::Result<String> = tokio::task::spawn_blocking(move || {
        loop {
            if cancel.load(Ordering::SeqCst) {
                bail!("oauth flow cancelled");
            }
            // Poll with a short timeout so we observe `cancel` promptly.
            match server.recv_timeout(Duration::from_millis(250))? {
                None      => continue,
                Some(req) => {
                    if req.method() != &Method::Get || !req.url().starts_with(&path) {
                        let _ = req.respond(Response::from_string("Not found")
                            .with_status_code(404));
                        continue;
                    }
                    // Capture the URL with its query string before we respond.
                    let captured = req.url().to_owned();

                    // Echo a friendly page back to the browser.
                    let body = if captured.contains("error=") {
                        HTML_FAILURE
                    } else {
                        HTML_SUCCESS
                    };
                    let _ = req.respond(
                        Response::from_string(body)
                            .with_header(
                                "Content-Type: text/html; charset=utf-8"
                                    .parse::<tiny_http::Header>()
                                    .unwrap(),
                            ),
                    );
                    return Ok(captured);
                }
            }
        }
    })
    .await
    .context("loopback listener task")?;

    let captured = query?;

    // Re-assemble the full URL the SDK expects (scheme + host + path + query).
    // tiny_http strips the scheme/host, leaving only "/callback?code=…".
    // OAuth::finish_login takes a UrlOrQuery — the query-string variant is
    // exactly what we have.
    let query_part = extract_query(&captured)?;

    flow.client
        .oauth()
        .finish_login(UrlOrQuery::Query(query_part))
        .await
        .context("oauth finish_login")?;

    // Best-effort: rename the freshly minted device to "<hostname>" so the
    // user can tell sessions apart. ClientMetadata.client_name only feeds the
    // OAuth consent page; the device display name comes from PUT /devices.
    if let (Some(device_id), Some(host)) = (
        flow.client.device_id(),
        hostname::get().ok().and_then(|h| h.into_string().ok()),
    ) {
        if let Err(e) = flow.client.rename_device(device_id, &host).await {
            tracing::warn!("rename_device({host:?}) failed: {e}");
        }
    }

    Ok(flow.client)
}

fn extract_query(captured: &str) -> anyhow::Result<String> {
    captured
        .splitn(2, '?')
        .nth(1)
        .ok_or_else(|| anyhow!("redirect did not include a query string"))
        .map(str::to_owned)
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

/// Trip the cancel flag; the listener will return on its next poll.
pub fn cancel(flow: &PendingFlow) {
    flow.cancel.store(true, Ordering::SeqCst);
    // Nudge the listener so it returns immediately rather than waiting out
    // the recv_timeout. tiny_http's server_addr() returns a ListenAddr;
    // the IP variant is what loopback bindings produce.
    if let Some(addr) = flow.server.server_addr().to_ip() {
        let _ = thread::Builder::new()
            .name("oauth-cancel-poke".into())
            .spawn(move || {
                let _ = std::net::TcpStream::connect_timeout(
                    &addr, Duration::from_millis(250));
            });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extract_query_returns_code_and_state() {
        let url = "/callback?code=abc123&state=xyz";
        assert_eq!(extract_query(url).unwrap(), "code=abc123&state=xyz");
    }

    #[test]
    fn extract_query_returns_single_param() {
        assert_eq!(extract_query("/callback?code=X").unwrap(), "code=X");
    }

    #[test]
    fn extract_query_errors_without_question_mark() {
        assert!(extract_query("/callback").is_err());
    }

    #[test]
    fn html_success_mentions_signed_in() {
        assert!(HTML_SUCCESS.contains("signed in"));
    }

    #[test]
    fn html_failure_mentions_failed() {
        assert!(HTML_FAILURE.contains("failed"));
    }
}
