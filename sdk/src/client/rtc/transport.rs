use anyhow::{anyhow, Context};
use serde::{Deserialize, Serialize};

/// Resolved LiveKit connection details for one call session.
#[derive(Clone, Debug)]
pub struct LiveKitTransport {
    /// Base URL of the LiveKit JWT authorization service (used for sticky refresh).
    pub service_url: String,
    /// WebSocket URL of the LiveKit SFU returned by the auth service.
    pub server_url: String,
    /// Short-lived JWT for connecting to the SFU.
    pub jwt: String,
}

#[derive(Deserialize)]
struct TransportEntry {
    #[serde(rename = "type")]
    kind: String,
    livekit_service_url: Option<String>,
}

#[derive(Deserialize)]
struct TransportsResponse {
    #[serde(default)]
    rtc_transports: Vec<TransportEntry>,
}

// The nested OpenID token object the auth service expects.
#[derive(Serialize)]
struct OpenIdTokenObject<'a> {
    access_token: &'a str,
    expires_in: u64,
    matrix_server_name: &'a str,
    token_type: &'static str,
}

// Newer /sfu/get request body (matrix-js-sdk ≥ 34.x).
// The service returns JWT sub = "{user_id}:{device_id}", which matrix-js-sdk
// constructs as the expected LiveKit participant identity.
// Note: field is "room" (not "room_id") and no slot_id.
#[derive(Serialize)]
struct SfuGetRequest<'a> {
    openid_token: OpenIdTokenObject<'a>,
    device_id: &'a str,
    room: &'a str,
}

// Legacy /get_token request body (older lk-jwt-service deployments).
// Returns JWT sub = sha256(member.id) — a bare hash without the user_id
// prefix that matrix-js-sdk expects, causing participant lookup to fail.
#[derive(Serialize)]
struct GetTokenRequest<'a> {
    room_id: &'a str,
    slot_id: &'a str,
    openid_token: OpenIdTokenObject<'a>,
    member: MemberObject<'a>,
}

// Member identity claims sent to the auth service (mirrors RtcMemberDetails).
#[derive(Serialize)]
struct MemberObject<'a> {
    id: &'a str,
    claimed_device_id: &'a str,
    claimed_user_id: &'a str,
}

#[derive(Deserialize)]
struct GetTokenResponse {
    jwt: String,
    url: String, // LiveKit SFU WebSocket URL
}

/// The first LiveKit transport's `livekit_service_url` in `transports`.
fn livekit_service_url(
    transports: &[matrix_sdk::ruma::api::client::rtc::RtcTransport],
) -> Option<String> {
    use matrix_sdk::ruma::api::client::rtc::RtcTransport;
    transports.iter().find_map(|t| match t {
        RtcTransport::LiveKit(info) => Some(info.service_url.clone()),
        _ => None,
    })
}

/// Fetch the LiveKit service URL for a call session.
///
/// Discovery order (mirrors Element Call):
///  1. `GET /_matrix/client/v1/rtc/transports` (stable; schema per MSC4195,
///     formalized as the MatrixRTC transports registry by MSC4519), via
///     matrix-sdk, which caches the answer.
///  2. `m.rtc_foci` / `org.matrix.msc4143.rtc_foci` in
///     `/.well-known/matrix/client`, via matrix-sdk. Tried even when step 1
///     answered with an empty list.
///  3. `GET /_matrix/client/unstable/org.matrix.msc4143/rtc/transports`
///     (unstable-period path per MSC4519; unstable transport `type`s are
///     registry-prefixed, e.g. `msc4195.livekit`). matrix-sdk doesn't know
///     this path, so it stays a hand-rolled fallback for older homeservers.
///
/// Neither MSC4519 nor the widget-facing MSC4515 (which just delegates
/// `GET /rtc/transports` through a `get_rtc_transports` widget action —
/// not applicable here, as Tesseract hosts no Matrix widgets) deprecates
/// the well-known fallback, so all three tiers are tried in order.
pub async fn fetch_livekit_service_url(
    client: &matrix_sdk::Client,
    http: &reqwest::Client,
) -> anyhow::Result<String> {
    // A failure here (5xx, network blip) must not stop the fallbacks below.
    match client.discover_rtc_transports().await {
        Ok(found) => {
            if let Some(url) = found.as_deref().and_then(livekit_service_url) {
                return Ok(url);
            }
        }
        Err(e) => tracing::warn!("rtc: /rtc/transports discovery failed: {e}"),
    }
    // Step 1 may have answered with an empty/livekit-less list, which the SDK
    // doesn't follow up with the well-known.
    if let Some(url) = client
        .well_known_rtc_transports()
        .await
        .ok()
        .as_deref()
        .and_then(livekit_service_url)
    {
        return Ok(url);
    }
    match fetch_unstable_livekit_service_url(client, http).await {
        Ok(Some(url)) => return Ok(url),
        Ok(None) => {}
        Err(e) => tracing::warn!("rtc: unstable /rtc/transports probe failed: {e:#}"),
    }

    Err(anyhow!(
        "no livekit transport found via /rtc/transports or \
         /.well-known/matrix/client (rtc_foci)"
    ))
}

/// Tier 3 of [`fetch_livekit_service_url`]: the unstable-prefix
/// `/rtc/transports` endpoint. `Ok(None)` when it is absent (404) or lists no
/// livekit transport.
async fn fetch_unstable_livekit_service_url(
    client: &matrix_sdk::Client,
    http: &reqwest::Client,
) -> anyhow::Result<Option<String>> {
    let Some(access_token) = client.access_token() else {
        return Ok(None);
    };
    let base = client.homeserver().to_string();
    let url = format!(
        "{}/_matrix/client/unstable/org.matrix.msc4143/rtc/transports",
        base.trim_end_matches('/')
    );
    let resp = http
        .get(&url)
        .bearer_auth(access_token)
        .send()
        .await
        .context("GET unstable /rtc/transports")?;

    if resp.status() == reqwest::StatusCode::NOT_FOUND {
        return Ok(None);
    }
    if !resp.status().is_success() {
        return Err(anyhow!("GET unstable /rtc/transports → {}", resp.status()));
    }

    let text = resp.text().await.context("read /rtc/transports body")?;
    let body: TransportsResponse = serde_json::from_str(&text)
        .with_context(|| format!("parse /rtc/transports — body was: {text}"))?;
    Ok(body
        .rtc_transports
        .into_iter()
        .find(|t| t.kind == "livekit" || t.kind.ends_with(".livekit"))
        .and_then(|t| t.livekit_service_url))
}

/// Returns true when the homeserver has at least one configured livekit transport.
/// Used during server-info fetch to gate the call UI without doing a full JWT exchange.
pub async fn probe_livekit_support(client: &matrix_sdk::Client, http: &reqwest::Client) -> bool {
    fetch_livekit_service_url(client, http).await.is_ok()
}

/// Exchange an OpenID token for a LiveKit JWT at the authorization service.
///
/// Tries the newer `/sfu/get` endpoint first (matrix-js-sdk ≥ 34.x), which
/// returns JWT sub = `{user_id}:{device_id}`.  matrix-js-sdk constructs the
/// expected LiveKit participant identity as `{sender}:{session_device_id}`, so
/// the JWT sub must include the user_id prefix for participant lookup to
/// succeed.  Falls back to the legacy `/get_token` endpoint on 404.
pub async fn fetch_livekit_jwt(
    http: &reqwest::Client,
    service_url: &str,
    room_id: &str,
    slot_id: &str,
    openid_access_token: &str,
    openid_expires_in: u64,
    matrix_server_name: &str,
    member_id: &str,
    device_id: &str,
    user_id: &str,
) -> anyhow::Result<LiveKitTransport> {
    let base = service_url.trim_end_matches('/');
    let openid_token = OpenIdTokenObject {
        access_token: openid_access_token,
        expires_in: openid_expires_in,
        matrix_server_name,
        token_type: "Bearer",
    };

    // ── Try /sfu/get (newer endpoint) ────────────────────────────────────────
    let sfu_url = format!("{base}/sfu/get");
    let sfu_body = SfuGetRequest {
        openid_token: OpenIdTokenObject {
            access_token: openid_access_token,
            expires_in: openid_expires_in,
            matrix_server_name,
            token_type: "Bearer",
        },
        device_id,
        room: room_id,
    };
    let sfu_resp = http
        .post(&sfu_url)
        .json(&sfu_body)
        .send()
        .await
        .context("POST livekit sfu/get")?;

    if sfu_resp.status() != reqwest::StatusCode::NOT_FOUND {
        if !sfu_resp.status().is_success() {
            let status = sfu_resp.status();
            let body = sfu_resp.text().await.unwrap_or_default();
            return Err(anyhow!("POST {sfu_url} → {status} — body: {body}"));
        }
        let tok: GetTokenResponse = sfu_resp.json().await.context("parse sfu/get response")?;
        return Ok(LiveKitTransport {
            service_url: service_url.to_owned(),
            server_url: tok.url,
            jwt: tok.jwt,
        });
    }

    // ── Fall back to /get_token (legacy endpoint) ─────────────────────────
    let token_url = format!("{base}/get_token");
    let req_body = GetTokenRequest {
        room_id,
        slot_id,
        openid_token,
        member: MemberObject {
            id: member_id,
            claimed_device_id: device_id,
            claimed_user_id: user_id,
        },
    };
    let resp = http
        .post(&token_url)
        .json(&req_body)
        .send()
        .await
        .context("POST livekit jwt service")?;

    if !resp.status().is_success() {
        let status = resp.status();
        let body = resp.text().await.unwrap_or_default();
        return Err(anyhow!("POST {token_url} → {status} — body: {body}"));
    }

    let tok: GetTokenResponse = resp.json().await.context("parse livekit jwt response")?;
    Ok(LiveKitTransport {
        service_url: service_url.to_owned(),
        server_url: tok.url,
        jwt: tok.jwt,
    })
}

/// Return the LiveKit room alias for a Matrix room.
///
/// Despite MSC4195 specifying base64url(SHA256(canonical_json([room_id, slot_id]))),
/// Element Web/X uses the Matrix room ID directly as the alias (e.g.
/// "!ji2U...:server" or the bare localpart for v2 room IDs).  Advertising a
/// hash that no one else computes causes Element Android to filter us out as
/// "not in this call".  Use the room ID verbatim to match.
pub fn livekit_room_alias(room_id: &str, _slot_id: &str) -> String {
    room_id.to_owned()
}

/// Decode the `sub` (participant identity) claim from a LiveKit JWT without
/// verifying the signature.  LiveKit JWTs use the compact JWS format
/// `header.payload.signature`; the payload is base64url-encoded JSON.
///
/// Returns `None` if the token is malformed, the payload is not valid JSON,
/// or the `sub` claim is missing/not a string.
pub fn decode_jwt_sub(jwt: &str) -> Option<String> {
    use base64ct::{Base64UrlUnpadded, Encoding};
    let payload_b64 = jwt.split('.').nth(1)?;
    let payload_bytes = Base64UrlUnpadded::decode_vec(payload_b64).ok()?;
    let claims: serde_json::Value = serde_json::from_slice(&payload_bytes).ok()?;
    claims.get("sub")?.as_str().map(|s| s.to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn transports(json: serde_json::Value) -> Vec<matrix_sdk::ruma::api::client::rtc::RtcTransport> {
        serde_json::from_value(json).unwrap()
    }

    #[test]
    fn livekit_service_url_picks_the_first_livekit_transport() {
        let t = transports(serde_json::json!([
            {"type": "something.else", "foo": 1},
            {"type": "livekit", "livekit_service_url": "https://lk-a.example.com"},
            {"type": "livekit", "livekit_service_url": "https://lk-b.example.com"},
        ]));
        assert_eq!(
            livekit_service_url(&t).as_deref(),
            Some("https://lk-a.example.com")
        );
    }

    #[test]
    fn livekit_service_url_is_none_without_a_livekit_transport() {
        assert_eq!(livekit_service_url(&[]), None);
        let t = transports(serde_json::json!([{"type": "something.else"}]));
        assert_eq!(livekit_service_url(&t), None);
    }

    #[test]
    fn room_alias_is_room_id() {
        let room_id = "!ji2UuenQYTErm9NXv2juKhYCUNM3DRZMM-MhRPvsLRk";
        assert_eq!(livekit_room_alias(room_id, "m.call#ROOM"), room_id);
        assert_eq!(livekit_room_alias(room_id, ""), room_id);
    }

    #[test]
    fn transports_response_parses_rtc_transports_key() {
        let body = r#"{"rtc_transports": [{"type": "livekit", "livekit_service_url": "https://lk.example.com"}]}"#;
        let parsed: TransportsResponse = serde_json::from_str(body).unwrap();
        let found = parsed
            .rtc_transports
            .into_iter()
            .find(|t| t.kind == "livekit" || t.kind.ends_with(".livekit"))
            .and_then(|t| t.livekit_service_url);
        assert_eq!(found.as_deref(), Some("https://lk.example.com"));
    }

    #[test]
    fn transports_response_matches_unstable_prefixed_type() {
        let body = r#"{"rtc_transports": [{"type": "msc4195.livekit", "livekit_service_url": "https://lk.example.com"}]}"#;
        let parsed: TransportsResponse = serde_json::from_str(body).unwrap();
        let found = parsed
            .rtc_transports
            .into_iter()
            .find(|t| t.kind == "livekit" || t.kind.ends_with(".livekit"))
            .and_then(|t| t.livekit_service_url);
        assert_eq!(found.as_deref(), Some("https://lk.example.com"));
    }

    fn jwt_with_payload(payload: &str) -> String {
        use base64ct::{Base64UrlUnpadded, Encoding};
        format!(
            "e30.{}.sig",
            Base64UrlUnpadded::encode_string(payload.as_bytes())
        )
    }

    #[test]
    fn decode_jwt_sub_reads_sub_claim() {
        let jwt = jwt_with_payload(r#"{"sub":"@alice:example.org:DEVICE","exp":1}"#);
        assert_eq!(
            decode_jwt_sub(&jwt).as_deref(),
            Some("@alice:example.org:DEVICE")
        );
    }

    #[test]
    fn decode_jwt_sub_rejects_malformed_tokens() {
        assert_eq!(decode_jwt_sub(""), None);
        assert_eq!(decode_jwt_sub("only-one-part"), None);
        // Payload is not base64url.
        assert_eq!(decode_jwt_sub("a.!!!.c"), None);
        // Valid base64 but not JSON.
        assert_eq!(decode_jwt_sub(&jwt_with_payload("not json")), None);
    }

    #[test]
    fn decode_jwt_sub_rejects_missing_or_non_string_sub() {
        assert_eq!(decode_jwt_sub(&jwt_with_payload(r#"{"iss":"x"}"#)), None);
        assert_eq!(decode_jwt_sub(&jwt_with_payload(r#"{"sub":42}"#)), None);
        assert_eq!(decode_jwt_sub(&jwt_with_payload(r#"{"sub":null}"#)), None);
    }

    #[test]
    fn transports_response_defaults_to_empty_list() {
        let parsed: TransportsResponse = serde_json::from_str("{}").unwrap();
        assert!(parsed.rtc_transports.is_empty());
    }

    #[test]
    fn transports_response_ignores_non_livekit_and_missing_url() {
        let body = r#"{"rtc_transports": [
            {"type": "other", "livekit_service_url": "https://nope"},
            {"type": "livekit"}
        ]}"#;
        let parsed: TransportsResponse = serde_json::from_str(body).unwrap();
        assert_eq!(parsed.rtc_transports.len(), 2);
        let found = parsed
            .rtc_transports
            .into_iter()
            .find(|t| t.kind == "livekit" || t.kind.ends_with(".livekit"))
            .and_then(|t| t.livekit_service_url);
        assert_eq!(found, None);
    }

    #[test]
    fn sfu_get_request_body_shape() {
        let body = SfuGetRequest {
            openid_token: OpenIdTokenObject {
                access_token: "tok",
                expires_in: 3600,
                matrix_server_name: "example.org",
                token_type: "Bearer",
            },
            device_id: "DEV",
            room: "!r:example.org",
        };
        assert_eq!(
            serde_json::to_value(&body).unwrap(),
            serde_json::json!({
                "openid_token": {
                    "access_token": "tok",
                    "expires_in": 3600,
                    "matrix_server_name": "example.org",
                    "token_type": "Bearer",
                },
                "device_id": "DEV",
                "room": "!r:example.org",
            })
        );
    }

    #[test]
    fn get_token_request_body_shape() {
        let body = GetTokenRequest {
            room_id: "!r:example.org",
            slot_id: "m.call#ROOM",
            openid_token: OpenIdTokenObject {
                access_token: "tok",
                expires_in: 1,
                matrix_server_name: "example.org",
                token_type: "Bearer",
            },
            member: MemberObject {
                id: "mid",
                claimed_device_id: "DEV",
                claimed_user_id: "@a:example.org",
            },
        };
        let v = serde_json::to_value(&body).unwrap();
        assert_eq!(v["room_id"], "!r:example.org");
        assert_eq!(v["slot_id"], "m.call#ROOM");
        assert_eq!(v["member"]["id"], "mid");
        assert_eq!(v["member"]["claimed_device_id"], "DEV");
        assert_eq!(v["member"]["claimed_user_id"], "@a:example.org");
        assert_eq!(v["openid_token"]["token_type"], "Bearer");
    }

    /// Serve canned `(status, body)` responses, one per connection, on a
    /// loopback port; returns the base URL and a handle yielding the
    /// request-lines seen.
    async fn serve(
        responses: Vec<(u16, &'static str)>,
    ) -> (String, tokio::task::JoinHandle<Vec<String>>) {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let base = format!("http://{}", listener.local_addr().unwrap());
        let handle = tokio::spawn(async move {
            let mut seen = Vec::new();
            for (status, body) in responses {
                let (mut sock, _) = listener.accept().await.unwrap();
                let mut buf = vec![0u8; 8192];
                let mut got = 0;
                // Read until the headers (and a short JSON body) have arrived.
                loop {
                    let n = sock.read(&mut buf[got..]).await.unwrap();
                    if n == 0 {
                        break;
                    }
                    got += n;
                    let text = String::from_utf8_lossy(&buf[..got]);
                    if let Some(h) = text.find("\r\n\r\n") {
                        let len = text[..h]
                            .lines()
                            .find_map(|l| {
                                l.to_ascii_lowercase()
                                    .strip_prefix("content-length:")
                                    .and_then(|v| v.trim().parse::<usize>().ok())
                            })
                            .unwrap_or(0);
                        if got >= h + 4 + len {
                            break;
                        }
                    }
                }
                let text = String::from_utf8_lossy(&buf[..got]).to_string();
                seen.push(text.lines().next().unwrap_or("").to_owned());
                let resp = format!(
                    "HTTP/1.1 {status} X\r\ncontent-type: application/json\r\n\
                     content-length: {}\r\nconnection: close\r\n\r\n{body}",
                    body.len()
                );
                sock.write_all(resp.as_bytes()).await.unwrap();
                let _ = sock.shutdown().await;
            }
            seen
        });
        (base, handle)
    }

    async fn call_jwt(base: &str) -> anyhow::Result<LiveKitTransport> {
        let http = reqwest::Client::new();
        fetch_livekit_jwt(
            &http, base, "!r:example.org", "slot", "tok", 60, "example.org", "mid", "DEV",
            "@a:example.org",
        )
        .await
    }

    #[tokio::test]
    async fn fetch_jwt_uses_sfu_get_when_available() {
        let (base, seen) =
            serve(vec![(200, r#"{"jwt":"J","url":"wss://sfu.example.org"}"#)]).await;
        // Trailing slash on the service URL must not produce `//sfu/get`.
        let t = call_jwt(&format!("{base}/")).await.unwrap();
        assert_eq!(t.jwt, "J");
        assert_eq!(t.server_url, "wss://sfu.example.org");
        assert_eq!(t.service_url, format!("{base}/"));
        let seen = seen.await.unwrap();
        assert_eq!(seen.len(), 1);
        assert!(seen[0].starts_with("POST /sfu/get "), "{}", seen[0]);
    }

    #[tokio::test]
    async fn fetch_jwt_falls_back_to_get_token_on_404() {
        let (base, seen) = serve(vec![
            (404, "{}"),
            (200, r#"{"jwt":"LEGACY","url":"wss://old.example.org"}"#),
        ])
        .await;
        let t = call_jwt(&base).await.unwrap();
        assert_eq!(t.jwt, "LEGACY");
        assert_eq!(t.server_url, "wss://old.example.org");
        let seen = seen.await.unwrap();
        assert!(seen[0].starts_with("POST /sfu/get "));
        assert!(seen[1].starts_with("POST /get_token "));
    }

    #[tokio::test]
    async fn fetch_jwt_reports_non_404_failure_without_fallback() {
        let (base, seen) = serve(vec![(500, "boom")]).await;
        let e = call_jwt(&base).await.unwrap_err().to_string();
        assert!(e.contains("500") && e.contains("boom"), "{e}");
        assert_eq!(seen.await.unwrap().len(), 1);
    }

    #[tokio::test]
    async fn fetch_jwt_reports_legacy_failure() {
        let (base, _seen) = serve(vec![(404, ""), (403, "denied")]).await;
        let e = call_jwt(&base).await.unwrap_err().to_string();
        assert!(e.contains("/get_token") && e.contains("403") && e.contains("denied"), "{e}");
    }

    #[tokio::test]
    async fn fetch_jwt_rejects_malformed_success_body() {
        let (base, _seen) = serve(vec![(200, r#"{"jwt":"only"}"#)]).await;
        assert!(call_jwt(&base).await.is_err());
    }
}
