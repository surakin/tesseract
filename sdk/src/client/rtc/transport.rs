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
    transports: Vec<TransportEntry>,
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

/// Fetch the LiveKit service URL for a call session.
///
/// Discovery order (mirrors Element Call):
///  1. `GET /_matrix/client/v1/rtc/transports` (stable MSC4195)
///  2. `GET /_matrix/client/unstable/org.matrix.msc4143/rtc/transports`
///  3. `org.matrix.msc4143.rtc_foci` in `/.well-known/matrix/client`
///
/// `server_name` is the Matrix server name (e.g. `"example.com"`) used to
/// build the well-known URL; it is distinct from `homeserver_url` in
/// delegated deployments.
pub async fn fetch_livekit_service_url(
    http: &reqwest::Client,
    homeserver_url: &str,
    access_token: &str,
    server_name: &str,
) -> anyhow::Result<String> {
    let base = homeserver_url.trim_end_matches('/');
    let candidates = [
        format!("{base}/_matrix/client/v1/rtc/transports"),
        format!("{base}/_matrix/client/unstable/org.matrix.msc4143/rtc/transports"),
    ];

    for url in &candidates {
        let resp = http
            .get(url)
            .bearer_auth(access_token)
            .send()
            .await
            .context("GET /rtc/transports")?;

        if resp.status() == reqwest::StatusCode::NOT_FOUND {
            continue;
        }
        if !resp.status().is_success() {
            return Err(anyhow!("GET /rtc/transports → {}", resp.status()));
        }

        let text = resp.text().await.context("read /rtc/transports body")?;
        let body: TransportsResponse = serde_json::from_str(&text)
            .with_context(|| format!("parse /rtc/transports — body was: {text}"))?;

        if let Some(url) = body
            .transports
            .into_iter()
            .find(|t| t.kind == "livekit")
            .and_then(|t| t.livekit_service_url)
        {
            return Ok(url);
        }
        // Endpoint responded but listed no livekit transport — fall through to well-known.
        break;
    }

    // Fall back to org.matrix.msc4143.rtc_foci in /.well-known/matrix/client.
    // This is how Element Call discovers LiveKit when /rtc/transports is absent
    // or returns an empty transport list.
    if let Some(url) = rtc_foci_from_well_known(http, server_name).await {
        return Ok(url);
    }

    Err(anyhow!(
        "no livekit transport found via /rtc/transports or \
         /.well-known/matrix/client (org.matrix.msc4143.rtc_foci)"
    ))
}

/// Returns true when the homeserver has at least one configured livekit transport.
/// Used during server-info fetch to gate the call UI without doing a full JWT exchange.
pub async fn probe_livekit_support(
    http: &reqwest::Client,
    homeserver_url: &str,
    access_token: &str,
    server_name: &str,
) -> bool {
    fetch_livekit_service_url(http, homeserver_url, access_token, server_name)
        .await
        .is_ok()
}

/// Look up `org.matrix.msc4143.rtc_foci` in `https://<server_name>/.well-known/matrix/client`
/// and return the `livekit_service_url` of the first livekit focus, if present.
async fn rtc_foci_from_well_known(http: &reqwest::Client, server_name: &str) -> Option<String> {
    let url = format!("https://{}/.well-known/matrix/client", server_name);
    let resp = http.get(&url).send().await.ok()?;
    if !resp.status().is_success() {
        return None;
    }
    let body: serde_json::Value = resp.json().await.ok()?;
    let foci = body.get("org.matrix.msc4143.rtc_foci")?.as_array()?;
    foci.iter()
        .find(|f| f.get("type").and_then(|t| t.as_str()) == Some("livekit"))
        .and_then(|f| f.get("livekit_service_url")?.as_str())
        .map(|s| s.to_owned())
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

    #[test]
    fn room_alias_is_room_id() {
        let room_id = "!ji2UuenQYTErm9NXv2juKhYCUNM3DRZMM-MhRPvsLRk";
        assert_eq!(livekit_room_alias(room_id, "m.call#ROOM"), room_id);
        assert_eq!(livekit_room_alias(room_id, ""), room_id);
    }
}
