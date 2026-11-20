//! Legacy `m.login.sso` login — a fallback for homeservers without an OIDC/MAS
//! provider. Reuses the redirect plumbing in [`crate::oauth`]: the browser
//! answers on the same loopback / private-use redirect, and the C++ side
//! drives the same await/commit/cancel FFI calls.
#![cfg(feature = "legacy_login")]

use anyhow::Context as _;
use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::Client;
use url::Url;

use crate::oauth::{self, FlowKind, RedirectMode};

/// Phase 1 — set up the redirect, build the client and compute the SSO URL.
/// `idp_id == ""` means the homeserver's generic SSO redirect.
pub async fn begin(
    homeserver: &str,
    sqlite_path: &std::path::Path,
    idp_id: &str,
    store_key: &[u8; 32],
    mode: RedirectMode,
) -> anyhow::Result<oauth::BeginResult> {
    let setup = oauth::setup_redirect(mode).await?;
    let client = oauth::build_configured_client(homeserver, sqlite_path, Some(store_key)).await?;
    oauth::probe_sss_support(&client).await?;
    let nonce = oauth::generate_sso_nonce();
    let mut redirect = setup.url.clone();
    redirect
        .query_pairs_mut()
        .append_pair(oauth::SSO_NONCE_PARAM, &nonce);
    let auth_url = sso_url(&client, &redirect, idp_id).await?;
    let redirect_uri = setup.url.to_string();
    Ok(oauth::BeginResult {
        auth_url,
        redirect_uri,
        flow: oauth::pending_flow(client, setup, FlowKind::Sso { nonce }),
    })
}

/// The homeserver's SSO redirect URL for `redirect`, optionally for one IdP.
pub(crate) async fn sso_url(
    client: &Client,
    redirect: &Url,
    idp_id: &str,
) -> anyhow::Result<String> {
    let idp = (!idp_id.is_empty()).then_some(idp_id);
    client
        .matrix_auth()
        .get_sso_login_url(redirect.as_str(), idp)
        .await
        .context("build sso login url")
}

/// Download an unauthenticated IdP icon (`mxc://`) through a throwaway client.
pub async fn fetch_icon(homeserver: &str, mxc: &str) -> anyhow::Result<Vec<u8>> {
    let client = Client::builder()
        .server_name_or_homeserver_url(homeserver)
        .user_agent(oauth::build_user_agent())
        .http_client(oauth::build_sdk_http_client())
        .build()
        .await
        .context("build client")?;
    let uri: matrix_sdk::ruma::OwnedMxcUri = mxc.into();
    let request = MediaRequestParameters {
        source: MediaSource::Plain(uri.clone()),
        format: MediaFormat::File,
    };
    // The SDK picks the authenticated media endpoint on v1.11+ servers, which
    // needs a token we do not have before login. Fall back to the legacy
    // unauthenticated endpoint, which servers keep serving for IdP icons.
    match client.media().get_media_content(&request, false).await {
        Ok(bytes) => Ok(bytes),
        Err(sdk_err) => {
            #[allow(deprecated)]
            {
                use matrix_sdk::ruma::api::client::media::get_content::v3::Request;
                let (server_name, media_id) = uri
                    .parts()
                    .map_err(|e| anyhow::anyhow!("invalid mxc uri: {e}"))?;
                let response = client
                    .send(Request::new(media_id.to_owned(), server_name.to_owned()))
                    .await
                    .map_err(|e| anyhow::anyhow!("fetch login icon: {sdk_err}; fallback: {e}"))?;
                Ok(response.file)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    /// Test-only homeserver stub answering every request with a versions body.
    async fn versions_stub() -> (Url, tokio::task::JoinHandle<()>) {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = Url::parse(&format!("http://{}", listener.local_addr().unwrap())).unwrap();
        let handle = tokio::spawn(async move {
            loop {
                let Ok((mut sock, _)) = listener.accept().await else {
                    return;
                };
                tokio::spawn(async move {
                    let mut buf = [0u8; 4096];
                    let _ = sock.read(&mut buf).await;
                    let body = r#"{"versions":["v1.11"]}"#;
                    let resp = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                        body.len()
                    );
                    let _ = sock.write_all(resp.as_bytes()).await;
                    let _ = sock.shutdown().await;
                });
            }
        });
        (url, handle)
    }

    async fn url_for(redirect: &str, idp: &str) -> (String, Url) {
        let (stub, handle) = versions_stub().await;
        let client = Client::builder()
            .homeserver_url(stub.as_str())
            .build()
            .await
            .unwrap();
        let out = sso_url(&client, &Url::parse(redirect).unwrap(), idp)
            .await
            .unwrap();
        handle.abort();
        (out, stub)
    }

    #[tokio::test]
    async fn sso_url_without_idp() {
        let (out, stub) = url_for("http://127.0.0.1:1234/callback", "").await;
        let prefix = format!(
            "{}/_matrix/client/v3/login/sso/redirect?",
            stub.as_str().trim_end_matches('/')
        );
        assert!(out.starts_with(&prefix), "{out}");
        assert!(
            out.contains("redirectUrl=http%3A%2F%2F127.0.0.1%3A1234%2Fcallback"),
            "{out}"
        );
    }

    #[tokio::test]
    async fn sso_url_keeps_nonce_param_in_redirect() {
        let mut redirect = Url::parse("http://127.0.0.1:1234/callback").unwrap();
        redirect
            .query_pairs_mut()
            .append_pair(oauth::SSO_NONCE_PARAM, "abc123");
        let (out, _) = url_for(redirect.as_str(), "").await;
        let parsed = Url::parse(&out).unwrap();
        let (_, v) = parsed
            .query_pairs()
            .find(|(k, _)| k == "redirectUrl")
            .expect("redirectUrl");
        assert!(v.contains("tsr_nonce=abc123"), "{v}");
        assert!(out.contains("tsr_nonce%3Dabc123"), "{out}");
    }

    #[tokio::test]
    async fn sso_url_with_idp() {
        let (out, _) = url_for("http://127.0.0.1:1234/callback", "oidc-github").await;
        assert!(
            out.contains("/_matrix/client/v3/login/sso/redirect/oidc-github?"),
            "{out}"
        );
    }

    #[tokio::test]
    async fn sso_url_external_redirect() {
        let (out, _) = url_for(oauth::EXTERNAL_REDIRECT_URI, "").await;
        let encoded: String =
            url::form_urlencoded::byte_serialize(oauth::EXTERNAL_REDIRECT_URI.as_bytes()).collect();
        assert!(out.contains(&format!("redirectUrl={encoded}")), "{out}");
    }
}
