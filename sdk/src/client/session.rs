//! OAuth login, session restore/export, server info, logout, cache clearing.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use anyhow::Context as _;
use matrix_sdk::{
    authentication::oauth::{ClientId, OAuthSession, UserSession},
    store::RoomLoadSettings,
    Client,
};
use serde::{Deserialize, Serialize};

use super::mutual_rooms::{mutual_rooms_mode_for, MUTUAL_ROOMS_NONE};
use super::{err, oauth_err, ok, ClientFfi};
use crate::ffi::{OAuthBegin, OAuthWaitStep, OpResult, ServerInfoFfi, SpecVersionFfi};
use crate::oauth;

use std::sync::atomic::Ordering;

#[cfg(not(test))]
use super::BACKUP_STATE_UNKNOWN;

/// MSC4452: whether `/preview_url` is available to this user, from a
/// `/capabilities` response. Prefers the stable `m.preview_url` key over the
/// unstable `io.element.msc4452.preview_url`; absent means available.
pub(super) fn preview_url_enabled(caps: &serde_json::Value) -> bool {
    ["m.preview_url", "io.element.msc4452.preview_url"]
        .iter()
        .find_map(|key| {
            caps.pointer(&format!("/capabilities/{key}/enabled"))
                .and_then(|v| v.as_bool())
        })
        .unwrap_or(true)
}

/// Tagged JSON envelope for a persisted session. The `auth` tag records which
/// mechanism authenticated the underlying `Client` (OAuth/MAS vs. native
/// `m.login.password`), so `restore_session`/`export_session`/`logout` and
/// every session-persistence call site can branch on it uniformly instead of
/// each caller needing to know which shape to expect.
///
/// `Deserialize` is hand-written (see below) rather than derived: every
/// `session.json` persisted before this envelope existed has no `"auth"` key
/// at all (bare `client_id` + flattened `UserSession` fields), and a derived
/// internally-tagged enum would reject those as "missing field `auth`" —
/// which is exactly what happened to already-logged-in installs on upgrade.
/// A missing tag is treated as legacy OAuth, since OAuth was the only
/// mechanism that could have produced a file with no tag at all.
#[derive(Serialize)]
#[serde(tag = "auth")]
pub(super) enum SessionEnvelope {
    #[serde(rename = "oauth")]
    OAuth {
        client_id: ClientId,
        #[serde(flatten)]
        user: UserSession,
    },
    /// Native `m.login.password` session. The variant itself is always
    /// defined (even in `TESSERACT_ENABLE_LEGACY_LOGIN=OFF` builds) so a
    /// `"native"`-tagged envelope from an earlier legacy-login-enabled build
    /// still deserializes cleanly instead of producing an "unknown variant"
    /// parse error; only the code paths that *act* on this variant are
    /// feature-gated (see `restore_session`/`export_session`/`logout` below).
    #[serde(rename = "native")]
    Native {
        /// Captured at login time rather than re-derived from the MXID's
        /// `server_name()` — self-hosted/non-OIDC deployments are more
        /// likely to have a client-API domain that diverges from the MXID
        /// server name (custom port, reverse proxy, IP-based setups).
        homeserver_url: String,
        #[serde(flatten)]
        session: matrix_sdk::authentication::matrix::MatrixSession,
    },
}

impl<'de> Deserialize<'de> for SessionEnvelope {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        #[derive(Deserialize)]
        struct OAuthFields {
            client_id: ClientId,
            #[serde(flatten)]
            user: UserSession,
        }
        #[derive(Deserialize)]
        struct NativeFields {
            homeserver_url: String,
            #[serde(flatten)]
            session: matrix_sdk::authentication::matrix::MatrixSession,
        }

        let value = serde_json::Value::deserialize(deserializer)?;
        let auth_tag = match value.get("auth") {
            None => "oauth",
            Some(v) => v.as_str().ok_or_else(|| {
                serde::de::Error::custom("session envelope `auth` must be a string")
            })?,
        };
        match auth_tag {
            "oauth" => {
                let f: OAuthFields =
                    serde_json::from_value(value).map_err(serde::de::Error::custom)?;
                Ok(SessionEnvelope::OAuth {
                    client_id: f.client_id,
                    user: f.user,
                })
            }
            "native" => {
                let f: NativeFields =
                    serde_json::from_value(value).map_err(serde::de::Error::custom)?;
                Ok(SessionEnvelope::Native {
                    homeserver_url: f.homeserver_url,
                    session: f.session,
                })
            }
            other => Err(serde::de::Error::custom(format!(
                "unknown session auth tag: {other}"
            ))),
        }
    }
}

impl SessionEnvelope {
    /// Snapshot `client`'s current auth session into an envelope, tagged by
    /// whichever mechanism actually authenticated it. Returns `None` if the
    /// client isn't authenticated (neither `oauth()` nor `matrix_auth()` has
    /// a live session).
    pub(super) fn snapshot(client: &Client) -> Option<Self> {
        if let Some(full) = client.oauth().full_session() {
            return Some(SessionEnvelope::OAuth {
                client_id: full.client_id,
                user: full.user,
            });
        }
        #[cfg(feature = "legacy_login")]
        if let Some(session) = client.matrix_auth().session() {
            return Some(SessionEnvelope::Native {
                homeserver_url: client.homeserver().to_string(),
                session,
            });
        }
        None
    }
}

/// Placeholder for "no server info": `ok == false`, everything else zeroed.
/// The C++ side ignores the other fields and uses its own defaults.
fn server_info_unavailable() -> ServerInfoFfi {
    ServerInfoFfi {
        ok: false,
        homeserver_url: String::new(),
        spec_versions: Vec::new(),
        supports_msc3030: false,
        can_change_password: false,
        can_set_displayname: false,
        can_set_avatar: false,
        supports_profile_fields: false,
        profile_fields_enabled: false,
        preview_url_enabled: false,
        supports_qr_grant: false,
        supports_calls: false,
        default_room_version: String::new(),
        profile_fields_stable: false,
        supports_invite_reason: false,
        supports_mutual_rooms: false,
        server_name: String::new(),
        server_version: String::new(),
        upload_size_limit: 0,
        login_flows: Vec::new(),
    }
}

/// `(name, version)` from a `/_matrix/federation/v1/version` response; empty
/// strings when the body doesn't carry them.
fn parse_federation_version(body: &serde_json::Value) -> (String, String) {
    let field = |key: &str| {
        body.pointer(&format!("/server/{key}"))
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned()
    };
    (field("name"), field("version"))
}

/// `m.upload.size` from a media `/config` response; 0 when absent.
fn parse_upload_limit(body: &serde_json::Value) -> u64 {
    body.get("m.upload.size")
        .and_then(|v| v.as_u64())
        .unwrap_or(0)
}

/// Flow types from a `GET /login` response with the `m.login.` prefix
/// stripped, in server order and without duplicates.
fn parse_login_flows(body: &serde_json::Value) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for flow in body
        .get("flows")
        .and_then(|v| v.as_array())
        .into_iter()
        .flatten()
    {
        let Some(ty) = flow.get("type").and_then(|v| v.as_str()) else {
            continue;
        };
        let ty = ty.strip_prefix("m.login.").unwrap_or(ty).to_owned();
        if !out.contains(&ty) {
            out.push(ty);
        }
    }
    out
}

/// GET `url` and parse the body as JSON; `Null` on any transport, status or
/// parse failure (these probes are best-effort and never fail server info).
async fn get_json_lenient(
    http: &reqwest::Client,
    url: String,
    bearer: Option<&str>,
) -> serde_json::Value {
    let mut req = http.get(url).timeout(std::time::Duration::from_secs(10));
    if let Some(token) = bearer {
        req = req.bearer_auth(token);
    }
    match req.send().await.and_then(|r| r.error_for_status()) {
        Ok(r) => r.json().await.unwrap_or(serde_json::Value::Null),
        Err(_) => serde_json::Value::Null,
    }
}

/// Build the `get_server_info` result for `client`: spec versions + feature flags
/// from the SDK's `/versions` handling (so they match the paths it picks for
/// requests), `/capabilities` from a direct fetch, and the side-effect slots
/// other code reads (profile-field prefix, invite-reason, URL previews).
async fn build_server_info(
    client: Client,
    http: reqwest::Client,
    prefix_slot: std::sync::Arc<std::sync::RwLock<Option<String>>>,
    invite_reason_slot: std::sync::Arc<std::sync::RwLock<bool>>,
    mutual_rooms_slot: std::sync::Arc<std::sync::atomic::AtomicU8>,
    preview_url_slot: std::sync::Arc<std::sync::atomic::AtomicBool>,
) -> ServerInfoFfi {
    let base = {
        let url = client.homeserver().to_string();
        url.trim_end_matches('/').to_owned()
    };
    let access_token = client.access_token().unwrap_or_default();

    // Drop any cached `/versions` so every call starts from live data: the
    // SDK keeps it for 24h and picks endpoint paths (stable vs unstable)
    // from it, so a stale entry after a server upgrade sent requests down
    // the wrong path.
    let (caps_resp, supported, fed_body, media_body, login_body) = tokio::join!(
        http.get(format!("{base}/_matrix/client/v3/capabilities"))
            .bearer_auth(&access_token)
            .send(),
        async {
            if let Err(e) = client.reset_supported_versions().await {
                tracing::warn!("failed to reset cached supported versions: {e}");
            }
            match client.supported_versions().await {
                Ok(v) => v,
                Err(e) => {
                    tracing::warn!("failed to fetch supported versions: {e}");
                    matrix_sdk::ruma::api::SupportedVersions::from_parts(
                        &[],
                        &std::collections::BTreeMap::new(),
                    )
                }
            }
        },
        get_json_lenient(&http, format!("{base}/_matrix/federation/v1/version"), None),
        async {
            // Authenticated media config (spec v1.11) first, then the legacy path.
            let v1 = get_json_lenient(
                &http,
                format!("{base}/_matrix/client/v1/media/config"),
                Some(access_token.as_str()),
            )
            .await;
            if v1.is_object() {
                v1
            } else {
                get_json_lenient(
                    &http,
                    format!("{base}/_matrix/media/v3/config"),
                    Some(access_token.as_str()),
                )
                .await
            }
        },
        get_json_lenient(&http, format!("{base}/_matrix/client/v3/login"), None),
    );
    let (server_name, server_version) = parse_federation_version(&fed_body);
    let upload_size_limit = parse_upload_limit(&media_body);
    let login_flows = parse_login_flows(&login_body);
    let spec_versions: Vec<SpecVersionFfi> = supported
        .versions
        .iter()
        .filter_map(|v| {
            let (major, minor) = v.as_str()?.strip_prefix('v')?.split_once('.')?;
            Some(SpecVersionFfi {
                major: major.parse().ok()?,
                minor: minor.parse().ok()?,
            })
        })
        .collect();
    let has_feature = |name: &str| supported.features.contains(&name.into());

    let supports_msc3030 = has_feature("org.matrix.msc3030");

    let supports_msc4133_stable = has_feature("uk.tcpip.msc4133.stable");
    let supports_msc4133_unstable = has_feature("uk.tcpip.msc4133");
    let supports_msc4133 = supports_msc4133_stable || supports_msc4133_unstable;

    let supports_qr_grant = has_feature("org.matrix.msc4108");

    // MSC4491 (experimental as of Synapse 1.156, gated behind
    // msc4491_enabled) — no stable/unstable pair yet, just the one
    // unstable_features key.
    let supports_invite_reason = has_feature("uk.timedout.msc4491.create_room_invite_reasons");

    *prefix_slot.write().unwrap() = if supports_msc4133_stable {
        Some("/_matrix/client/v3".to_owned())
    } else if supports_msc4133_unstable {
        Some("/_matrix/client/unstable/uk.tcpip.msc4133".to_owned())
    } else {
        None
    };
    *invite_reason_slot.write().unwrap() = supports_invite_reason;

    let mutual_rooms_mode = mutual_rooms_mode_for(has_feature, &spec_versions);
    mutual_rooms_slot.store(mutual_rooms_mode, Ordering::Relaxed);

    let caps: serde_json::Value = match caps_resp {
        Ok(r) => r.json().await.unwrap_or(serde_json::Value::Null),
        Err(_) => serde_json::Value::Null,
    };

    // A failed or unreadable capabilities request tells us nothing about the
    // server, so optional features default off. A valid response that omits a
    // key still means available, as the spec says (see preview_url_enabled).
    let caps_known = caps.is_object();
    let cap_bool = |key: &str| -> bool {
        caps.pointer(&format!("/capabilities/{key}/enabled"))
            .and_then(|v| v.as_bool())
            .unwrap_or(caps_known)
    };
    let default_room_ver = caps
        .pointer("/capabilities/m.room_versions/default")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_owned();
    let profile_fields_enabled = caps
        .pointer("/capabilities/m.profile_fields/enabled")
        .and_then(|v| v.as_bool())
        .unwrap_or(caps_known);
    let preview_url_enabled = caps_known && preview_url_enabled(&caps);
    preview_url_slot.store(preview_url_enabled, Ordering::Relaxed);

    let supports_calls = crate::client::rtc::transport::probe_livekit_support(&client, &http).await;

    ServerInfoFfi {
        ok: true,
        homeserver_url: base,
        spec_versions,
        supports_msc3030,
        can_change_password: cap_bool("m.change_password"),
        can_set_displayname: cap_bool("m.set_displayname"),
        can_set_avatar: cap_bool("m.set_avatar_url"),
        supports_profile_fields: supports_msc4133,
        profile_fields_enabled,
        preview_url_enabled,
        supports_qr_grant,
        supports_calls,
        default_room_version: default_room_ver,
        profile_fields_stable: supports_msc4133_stable,
        supports_invite_reason,
        supports_mutual_rooms: mutual_rooms_mode != MUTUAL_ROOMS_NONE,
        server_name,
        server_version,
        upload_size_limit,
        login_flows,
    }
}

impl ClientFfi {
    // -----------------------------------------------------------------------
    // OAuth login
    // -----------------------------------------------------------------------

    pub fn oauth_begin(
        &mut self,
        homeserver: &str,
        register_account: bool,
        user_id_hint: &str,
    ) -> OAuthBegin {
        if let Some(prev) = self.oauth_flow.take() {
            oauth::cancel(&prev);
        }

        let hs = homeserver.to_owned();
        let path = self.data_dir.clone();

        // Only start from a clean store when there is no active session. If a
        // session was already restored (e.g. the user hit "Sign in again"
        // after a timeout, or a second flow), wiping here would silently
        // destroy the live SQLite cache for the restored account. The store
        // is cleared explicitly on logout instead.
        if self.client.is_none() {
            let _ = std::fs::remove_dir_all(&path);
        }
        let _ = std::fs::create_dir_all(&path);

        // A fresh login always gets a fresh store-encryption key — generated
        // once here and retrievable afterwards via `store_key()` so the
        // caller can persist it alongside the session. Restored sessions
        // never hit this path, so a legacy (never-encrypted) account is
        // never retroactively given one.
        let key = *self.store_key.get_or_insert_with(oauth::generate_store_key);

        match self
            .rt
            .block_on(oauth::begin(&hs, &path, register_account, user_id_hint, &key))
        {
            Ok(begin) => {
                let auth_url = begin.auth_url;
                let redirect_uri = begin.redirect_uri;
                self.oauth_flow = Some(begin.flow);
                OAuthBegin {
                    ok: true,
                    message: String::new(),
                    auth_url,
                    redirect_uri,
                }
            }
            // {e:#} includes the full anyhow context chain — e.to_string()
            // would print only the outermost .context(...) label (see the
            // matching restore_session error conversion below).
            Err(e) => oauth_err(format!("{e:#}")),
        }
    }

    pub fn homeserver_supports_registration(&self, homeserver: &str) -> bool {
        let hs = homeserver.to_owned();
        let rt = &self.rt;
        // Keep a tokio context in TLS for the whole call so the throwaway Client
        // built inside `supports_registration` drops with a runtime handle
        // available — matrix-sdk's store/crypto teardown calls Handle::current()
        // in Drop. Same hazard guarded by oauth_commit / restore_session /
        // ClientFfi::drop. catch_unwind then ensures no panic crosses the cxx
        // `-> bool` boundary (which would abort, not raise a C++ exception); the
        // probe is best-effort, so any panic degrades to `false`.
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _guard = rt.enter();
            rt.block_on(oauth::supports_registration(&hs))
        }))
        .unwrap_or(false)
    }

    pub fn homeserver_supports_oauth(&self, homeserver: &str) -> bool {
        let hs = homeserver.to_owned();
        let rt = &self.rt;
        // Same hazard/guard rationale as homeserver_supports_registration above.
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _guard = rt.enter();
            rt.block_on(oauth::supports_oauth(&hs))
        }))
        .unwrap_or(false)
    }

    // One slice of the OAuth browser wait. `&self` (not `&mut self`) so the C++
    // side (client.cpp) can call it under a shared lock. The wait itself runs
    // as a background task, and each slice waits only briefly for its outcome.
    // The C++ side releases the shared lock between slices, so `oauth_cancel`
    // and writers queued behind the sign-in are not held for minutes. Storing
    // the resulting `Client` needs `&mut self`, so that step stays in
    // `oauth_commit`, called once a slice reports success.
    pub fn oauth_await_slice(&self) -> OAuthWaitStep {
        let Some(flow) = self.oauth_flow.as_ref() else {
            return OAuthWaitStep {
                pending: false,
                ok: false,
                message: "no oauth flow in progress; call oauth_begin first".into(),
            };
        };

        match self.rt.block_on(oauth::await_slice(flow)) {
            None => OAuthWaitStep {
                pending: true,
                ok: false,
                message: String::new(),
            },
            Some(Ok(())) => OAuthWaitStep {
                pending: false,
                ok: true,
                message: String::new(),
            },
            Some(Err(e)) => OAuthWaitStep {
                pending: false,
                ok: false,
                message: format!("{e:#}"),
            },
        }
    }

    /// Store the `Client` a successful `oauth_await_slice` produced. Fast
    /// and non-blocking (no network I/O), so briefly taking `&mut self` here
    /// cannot deadlock a concurrent `oauth_cancel` the way holding it across
    /// the whole await did.
    pub fn oauth_commit(&mut self) -> OpResult {
        let Some(flow) = self.oauth_flow.take() else {
            return err("no oauth flow in progress; call oauth_begin first");
        };
        match oauth::take_finished(&flow) {
            Some(client) => {
                // Enter the runtime so any prior Client we're overwriting drops
                // with a tokio context in TLS — matrix-sdk's SqliteStateStore /
                // deadpool tear-down calls Handle::current() in its Drop impl.
                let _guard = self.rt.enter();
                self.client = Some(client);
                ok("")
            }
            None => err("oauth flow has not finished successfully yet"),
        }
    }

    // `&self`, like `oauth_await_slice` above — this is the one thing
    // that must be reachable *while* that call is blocked.
    pub fn oauth_cancel(&self) {
        if let Some(flow) = self.oauth_flow.as_ref() {
            oauth::cancel(flow);
        }
    }

    // -----------------------------------------------------------------------
    // Session persistence
    // -----------------------------------------------------------------------

    pub fn restore_session(&mut self, session_json: &str) -> OpResult {
        let envelope: SessionEnvelope = match serde_json::from_str(session_json) {
            Ok(s) => s,
            Err(e) => return err(format!("parse session JSON: {e}")),
        };

        let path = self.data_dir.clone();
        let _ = std::fs::create_dir_all(&path);
        // `None` for any session that predates store encryption, or that was
        // never given a key by the C++ caller (`set_store_key`) before this
        // call — such a session stays permanently unencrypted, matching
        // Element X's own choice not to migrate pre-existing sessions.
        let store_key = self.store_key;

        let result = self.rt.block_on(async move {
            match envelope {
                SessionEnvelope::OAuth { client_id, user } => {
                    let homeserver = user.meta.user_id.server_name().to_string();
                    let client =
                        oauth::build_configured_client(&homeserver, &path, store_key.as_ref())
                            .await?;

                    let session = OAuthSession { client_id, user };
                    client
                        .oauth()
                        .restore_session(session, RoomLoadSettings::default())
                        .await
                        .context("restore oauth session")?;

                    // Install a synchronous save_session_callback so that any token
                    // refresh that completes — even if the tokio runtime is being torn
                    // down and our async TokensRefreshed watcher is aborted mid-flight
                    // — immediately persists the new tokens.  Without this, servers
                    // that rotate refresh tokens (e.g. MAS on matrix.org) can mark RT_n
                    // as used before the app receives the response, leaving a stale RT_n
                    // persisted and causing invalid_grant on the next launch.
                    //
                    // The save MUST land in the same authoritative store the next launch
                    // restores from: the platform secret store (Credential Manager /
                    // Keychain / libsecret) via SessionStore::save_account — reached here
                    // through the persist_session FFI.  Writing a plaintext session.json
                    // beside the sqlite store does NOT work: after the secret-store
                    // migration that file holds only a sentinel and load_account ignores
                    // it, so the rotated token would be silently dropped.
                    let _ = client.set_session_callbacks(
                        Box::new(move |c: Client| {
                            c.session_tokens().ok_or_else(|| "no session tokens".into())
                        }),
                        Box::new(move |c: Client| {
                            let Some(envelope) = SessionEnvelope::snapshot(&c) else {
                                return Ok(());
                            };
                            let user_id = c.user_id().map(|u| u.to_string()).unwrap_or_default();
                            let json = serde_json::to_string(&envelope).map_err(|e| {
                                Box::new(e) as Box<dyn std::error::Error + Send + Sync>
                            })?;
                            crate::ffi::persist_session(&user_id, &json);
                            Ok(())
                        }),
                    );

                    anyhow::Ok(client)
                }
                #[cfg(feature = "legacy_login")]
                SessionEnvelope::Native {
                    homeserver_url,
                    session,
                } => {
                    let client =
                        oauth::build_configured_client(&homeserver_url, &path, store_key.as_ref())
                            .await?;
                    client
                        .matrix_auth()
                        .restore_session(session, RoomLoadSettings::default())
                        .await
                        .context("restore native (m.login.password) session")?;

                    // Same rationale as the OAuth branch above: persist any
                    // refreshed native token immediately via a synchronous
                    // callback rather than relying solely on the async
                    // TokensRefreshed watcher, which may be aborted mid-flight
                    // during runtime teardown.
                    let _ = client.set_session_callbacks(
                        Box::new(move |c: Client| {
                            c.session_tokens().ok_or_else(|| "no session tokens".into())
                        }),
                        Box::new(move |c: Client| {
                            let Some(envelope) = SessionEnvelope::snapshot(&c) else {
                                return Ok(());
                            };
                            let user_id = c.user_id().map(|u| u.to_string()).unwrap_or_default();
                            let json = serde_json::to_string(&envelope).map_err(|e| {
                                Box::new(e) as Box<dyn std::error::Error + Send + Sync>
                            })?;
                            crate::ffi::persist_session(&user_id, &json);
                            Ok(())
                        }),
                    );

                    anyhow::Ok(client)
                }
                #[cfg(not(feature = "legacy_login"))]
                SessionEnvelope::Native { .. } => {
                    anyhow::bail!(
                        "this build does not support legacy username/password login sessions"
                    )
                }
            }
        });

        match result {
            Ok(c) => {
                // Enter the runtime so the previous Client (still in self.client
                // after an auth-error → re-restore path) drops with a tokio
                // context in TLS — matrix-sdk's SqliteStateStore / deadpool
                // tear-down calls Handle::current() in its Drop impl.
                let _guard = self.rt.enter();
                self.client = Some(c);
                ok("")
            }
            // {e:#} includes the full anyhow context chain — e.to_string()
            // would print only the outermost .context(...) label (see the
            // matching oauth_await_slice error conversion above).
            Err(e) => err(format!("{e:#}")),
        }
    }

    pub fn export_session(&self) -> String {
        let Some(client) = &self.client else {
            return String::new();
        };
        let Some(envelope) = SessionEnvelope::snapshot(client) else {
            return String::new();
        };
        serde_json::to_string(&envelope).unwrap_or_default()
    }

    /// Fetch homeserver spec versions and enabled capabilities.
    /// Returns `ok == false` when not logged in.
    /// Blocks the calling thread — call only from a worker thread.
    pub fn get_server_info(&self) -> ServerInfoFfi {
        let Some(client) = &self.client else {
            return server_info_unavailable();
        };
        let client = client.clone();
        let http = self.http_client.clone();
        let prefix_slot = self.profile_fields_prefix.clone();
        let invite_reason_slot = self.supports_invite_reason.clone();
        let mutual_rooms_slot = self.mutual_rooms_mode.clone();
        let preview_url_slot = self.homeserver_preview_url_enabled.clone();

        #[cfg(not(test))]
        let _in_flight = self.in_flight_guard("server-info");
        self.rt.block_on(build_server_info(
            client,
            http,
            prefix_slot,
            invite_reason_slot,
            mutual_rooms_slot,
            preview_url_slot,
        ))
    }

    /// Async counterpart of `get_server_info`. Spawns the fetch on the tokio
    /// runtime and fires `on_server_info_ready(request_id, info)` on completion
    /// (`ok == false` when not logged in). Does not pin a thread.
    #[cfg(not(test))]
    pub fn get_server_info_async(&self, request_id: u64) {
        let Some(client) = self.client.clone() else {
            if let Some(ref h) = self.handler {
                let g = h.lock();
                g.on_server_info_ready(request_id, &server_info_unavailable());
            }
            return;
        };
        let handler = self.handler.clone();
        let http = self.http_client.clone();
        let prefix_slot = self.profile_fields_prefix.clone();
        let invite_reason_slot = self.supports_invite_reason.clone();
        let mutual_rooms_slot = self.mutual_rooms_mode.clone();
        let preview_url_slot = self.homeserver_preview_url_enabled.clone();
        let in_flight = self.in_flight_ctx();

        self.rt.spawn(async move {
            let _in_flight = in_flight.guard("server-info");
            let info = build_server_info(
                client,
                http,
                prefix_slot,
                invite_reason_slot,
                mutual_rooms_slot,
                preview_url_slot,
            )
            .await;

            if let Some(h) = handler {
                let g = h.lock();
                g.on_server_info_ready(request_id, &info);
            }
        });
    }

    #[cfg(test)]
    pub fn get_server_info_async(&self, _request_id: u64) {}

    // -----------------------------------------------------------------------
    // Cache management
    // -----------------------------------------------------------------------

    /// Abort every live Timeline / ThreadList background task and reset the
    /// session-scoped in-memory state that must not survive a local wipe or a
    /// logout. Does NOT touch `self.client` or the on-disk stores. Shared by
    /// [`clear_caches`](Self::clear_caches) and [`logout`](Self::logout).
    #[cfg(not(test))]
    fn drain_and_reset_session_state(&mut self) {
        // Signal every spawned task cancelled before the Client they reference
        // is torn down. Plain `.drain()` alone would drop the AbortHandles
        // without cancelling the futures, keeping an Arc<Timeline> (and the
        // HTTP client) alive.
        {
            let _guard = self.rt.enter();
            for (_, th) in self.timelines.write().drain() {
                th.cancelled.store(true, Ordering::Release);
                for h in th.abort_tasks {
                    h.abort();
                }
            }
            for (_, th) in self.thread_timelines.write().drain() {
                th.cancelled.store(true, Ordering::Release);
                for h in th.abort_tasks {
                    h.abort();
                }
            }
            for (_, h) in self.thread_lists.write().drain() {
                h.abort.abort();
            }
        }

        if let Some(flow) = self.oauth_flow.take() {
            oauth::cancel(&flow);
        }

        {
            let mut db = self.app_cache_db.lock();
            *db = None;
        }
        {
            let mut db = self.search_db.lock();
            *db = None;
        }
        {
            // Drop the handle so the file is unlocked, but do NOT delete
            // `thread_read_state.db` — `clear_caches` re-seeds from it on the
            // in-place restore, and it's user read-state, not cache. Only
            // `logout`'s `remove_dir_all(data_dir)` removes it.
            let mut db = self.thread_read_db.lock();
            *db = None;
        }
        {
            let mut cache = self.backfill_previews.lock();
            cache.clear();
        }
        // Session-scoped read-state caches: must not survive a local wipe or
        // logout. `thread_read_markers` is re-seeded from `thread_read_state.db`
        // on the next `start_sync`.
        self.thread_read_markers.write().clear();
        self.thread_receipt_cache.write().clear();
        self.thread_chip_overrides.write().clear();

        self.imported_keys.store(0, Ordering::Relaxed);
        self.backup_state_code
            .store(BACKUP_STATE_UNKNOWN, Ordering::Relaxed);
        self.media_upload_limit.store(0, Ordering::Relaxed);
        *self.profile_fields_prefix.write().unwrap() = None;
    }

    /// Bounded (15s) `pause()` + drop of `self.client`, if set. `pause()`
    /// disables the send queue then deterministically closes all four
    /// SQLite-backed stores (state, event-cache, media, crypto) via
    /// `BaseClient::close_stores()` — each `close()` drains in-flight writes,
    /// runs `PRAGMA wal_checkpoint(TRUNCATE)`, and waits for the connection
    /// pool to empty — so no `-wal` / `-shm` handles remain open when the
    /// caller deletes the store files (a plain `Drop` does none of this and
    /// the deletes then fail on Windows).
    fn pause_and_drop_client(&mut self) {
        if let Some(client) = self.client.take() {
            close_stores_and_drop(&self.rt, client);
        }
    }

    /// Wipe an account's local *synced* state and leave the client torn down,
    /// ready for an immediate `restore_session` + `start_sync` by the caller.
    ///
    /// Self-contained (like [`logout`](Self::logout)): the C++ caller must NOT
    /// `stop_sync()` first. In order:
    ///
    /// 1. `SyncService::expire_sessions()` — resets the room-list *and*
    ///    encryption sliding-sync cursors to `pos = None` and persists that.
    ///    **This is load-bearing:** matrix-sdk stashes the sliding-sync `pos`
    ///    in `matrix-sdk-crypto.sqlite3`, not the state store (a documented
    ///    "TERRIBLE HACK" for cross-process safety — see
    ///    `matrix-sdk/src/sliding_sync/cache.rs`). We delete the state store in
    ///    step 4 but keep the crypto store, so without expiring first the next
    ///    client restores the stale cursor, resumes the sync connection
    ///    incrementally, and never re-fetches the room list — leaving it
    ///    permanently empty. Element X's FFI `clear_caches` does the same.
    /// 2. `stop_sync()` — task/handler teardown (sync_service already taken).
    /// 3. `drain_and_reset_session_state()` + bounded `pause()` + drop.
    /// 4. Delete the state, event-cache and media SQLite stores plus our
    ///    `app_cache.db` / `search_index.db`. The crypto store is kept, so the
    ///    device identity and room keys survive and no re-verification is
    ///    needed.
    ///
    /// The client is `pause()`d before the files are removed — see
    /// [`pause_and_drop_client`](Self::pause_and_drop_client). Dropping the
    /// Client outright (rather than `event_cache().clear_all_rooms()` on a live
    /// client) also sidesteps the linked-chunk observer poison panics that
    /// plagued earlier versions.
    pub fn clear_caches(&mut self) -> crate::ffi::OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }

        // 1. Expire the sliding-sync sessions (see the doc comment above —
        //    this is what actually makes the room list re-fetch). Take the
        //    service here so the stop_sync() below skips its own svc.stop().
        //    Bounded like stop_sync()'s svc.stop(): expire_sessions() awaits
        //    the supervisor shutdown, which can wait out an in-flight
        //    long-poll (Tesseract caps requests at 30s).
        #[cfg(not(test))]
        if let Some(svc) = self.sync_service.take() {
            let _ = self.rt.block_on(async {
                tokio::time::timeout(std::time::Duration::from_secs(30), svc.expire_sessions())
                    .await
            });
        }

        // 2. Task/handler bookkeeping (aborts sync/media/paginate tasks,
        //    detaches the event handler, removes global event handlers).
        self.stop_sync();

        // 3. Cancels timelines + drops the app-cache / search DB handles +
        //    backfill cache (among other session-scoped state), then a bounded
        //    pause() so every SQLite pool is closed before the deletes.
        #[cfg(not(test))]
        self.drain_and_reset_session_state();
        self.pause_and_drop_client();

        // 4. Remove the on-disk stores (crypto kept).
        #[cfg(not(test))]
        wipe_local_stores(&self.data_dir, /* keep_crypto = */ true);

        // 5. The FTS index file is gone; drop the in-process gate to match.
        //    The C++ shell re-applies the search-indexing preference after the
        //    in-place restore below (via apply_search_indexing_pref_), and that
        //    call only re-triggers the one-time history backfill on a genuine
        //    off->on transition. Leaving the gate `true` here would make it a
        //    no-op, so only live indexing would resume and the (now empty)
        //    history would never be re-crawled until the next app restart.
        #[cfg(not(test))]
        self.search_indexing_enabled
            .store(false, std::sync::atomic::Ordering::Relaxed);

        ok(String::new())
    }

    /// Tear the client down in place so the caller's `restore_session` +
    /// `start_sync` rebuild it, re-reading the process-wide build-time
    /// settings (MSC4153 / block-sends). Unlike [`clear_caches`](Self::clear_caches)
    /// nothing on disk is touched and sliding sync is not expired, so the
    /// persisted `pos` stays consistent with the kept stores.
    pub fn reload_client(&mut self) -> crate::ffi::OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }

        self.stop_sync();

        #[cfg(not(test))]
        self.drain_and_reset_session_state();
        self.pause_and_drop_client();

        // The search DB handle was dropped above; reset the gate so the
        // shell's re-apply of the indexing preference reopens live indexing
        // (the persisted backfill marker keeps a finished crawl from rerunning).
        #[cfg(not(test))]
        self.search_indexing_enabled
            .store(false, std::sync::atomic::Ordering::Relaxed);

        ok(String::new())
    }

    // -----------------------------------------------------------------------
    // Logout
    // -----------------------------------------------------------------------

    pub fn logout(&mut self) -> OpResult {
        self.stop_sync();

        // Cancel in-flight tasks and drop session-scoped in-memory state
        // (timelines, oauth flow, backfill cache, app-cache / search DB
        // handles, per-session counters) before revoking tokens and deleting
        // the store directory.
        #[cfg(not(test))]
        self.drain_and_reset_session_state();

        let Some(client) = self.client.take() else {
            let _ = std::fs::remove_dir_all(&self.data_dir);
            return ok("");
        };

        // The account is going away: stop routing its call events to its handler.
        if self
            .rtc_sink_registered
            .swap(false, std::sync::atomic::Ordering::Relaxed)
        {
            if let Some(uid) = client.user_id() {
                super::rtc::unregister_sink(uid.as_str());
            }
        }

        let is_oauth = client.oauth().full_session().is_some();
        let revoke: Result<(), String> = self.rt.block_on(async {
            if is_oauth {
                client
                    .oauth()
                    .logout()
                    .await
                    .map(|_| ())
                    .map_err(|e| e.to_string())
            } else {
                #[cfg(feature = "legacy_login")]
                {
                    client
                        .matrix_auth()
                        .logout()
                        .await
                        .map(|_| ())
                        .map_err(|e| e.to_string())
                }
                #[cfg(not(feature = "legacy_login"))]
                {
                    Ok(())
                }
            }
        });

        // `client` is still owned here (the revoke block borrowed it, it did
        // not move it in) specifically so token revocation could run first.
        // close_stores_and_drop() does the bounded pause() — closing all four
        // SQLite stores (crypto included) deterministically — then drops the
        // client, so the directory removal below hits no open handles.
        close_stores_and_drop(&self.rt, client);

        // Best-effort: every store (crypto included) is explicitly closed
        // above, so this should now succeed cleanly outside a close timeout.
        // On failure, the C++ caller (finalize_login_blocking_/
        // logout_active_account_impl_) routes around a leftover directory by
        // allocating a fresh one for the next login rather than needing this
        // removal to succeed — see SessionStore::allocate_account_dir /
        // sweep_orphaned_account_dirs.
        let _ = std::fs::remove_dir_all(&self.data_dir);

        match revoke {
            Ok(_) => ok(""),
            Err(e) => err(format!("logout failed (local store cleared): {e}")),
        }
    }
}

/// Bounded (15s) `client.pause()` — which disables the send queue then closes
/// all four SQLite-backed stores (state, event-cache, media, crypto) via
/// `BaseClient::close_stores()`, each an awaited `connection::close_connections`
/// (in-flight write drain + `PRAGMA wal_checkpoint(TRUNCATE)` + pool-empty
/// wait) — followed by dropping the client with the runtime entered
/// (matrix-sdk's `SqliteStateStore` `Drop` calls `Handle::current()`).
///
/// Shared by [`ClientFfi::logout`] (which keeps the client alive for token
/// revocation first) and [`ClientFfi::pause_and_drop_client`]. The timeout is
/// a backstop: each individual `close()` is already bounded inside
/// matrix-sdk-sqlite, so it should only fire if something is genuinely stuck.
fn close_stores_and_drop(rt: &tokio::runtime::Runtime, client: Client) {
    let close_result = rt.block_on(async {
        tokio::time::timeout(std::time::Duration::from_secs(15), client.pause()).await
    });
    match close_result {
        Ok(Err(e)) => tracing::warn!("closing stores: {e}"),
        Err(_) => tracing::warn!("closing stores: timed out"),
        Ok(Ok(())) => {}
    }
    let _guard = rt.enter();
    drop(client);
}

/// Delete the on-disk cache/store files under `data_dir`. Always removes the
/// state, event-cache and media SQLite stores (with their `-wal` / `-shm`
/// sidecars) plus our `app_cache.db` / `search_index.db`; the crypto store is
/// removed only when `keep_crypto` is false. Every removal is best-effort.
/// [`ClientFfi::pause_and_drop_client`] must run first so no handles are open.
fn wipe_local_stores(data_dir: &std::path::Path, keep_crypto: bool) {
    let mut stores = vec![
        "matrix-sdk-state.sqlite3",
        "matrix-sdk-event-cache.sqlite3",
        "matrix-sdk-media.sqlite3",
    ];
    if !keep_crypto {
        stores.push("matrix-sdk-crypto.sqlite3");
    }
    for stem in stores {
        for suffix in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(data_dir.join(format!("{stem}{suffix}")));
        }
    }
    let _ = std::fs::remove_file(data_dir.join("app_cache.db"));
    let _ = std::fs::remove_file(data_dir.join("search_index.db"));
}

#[cfg(test)]
mod wipe_local_stores_tests {
    use super::wipe_local_stores;
    use std::fs;

    const ALL: &[&str] = &[
        "matrix-sdk-state.sqlite3",
        "matrix-sdk-state.sqlite3-wal",
        "matrix-sdk-state.sqlite3-shm",
        "matrix-sdk-event-cache.sqlite3",
        "matrix-sdk-event-cache.sqlite3-wal",
        "matrix-sdk-event-cache.sqlite3-shm",
        "matrix-sdk-media.sqlite3",
        "matrix-sdk-media.sqlite3-wal",
        "matrix-sdk-media.sqlite3-shm",
        "matrix-sdk-crypto.sqlite3",
        "matrix-sdk-crypto.sqlite3-wal",
        "matrix-sdk-crypto.sqlite3-shm",
        "app_cache.db",
        "search_index.db",
        "session.json",
        // Read-state, not cache — must survive `wipe_local_stores`
        // ("Clear all caches"). Only `logout`'s `remove_dir_all` clears it.
        "thread_read_state.db",
    ];

    fn populate(dir: &std::path::Path) {
        for name in ALL {
            fs::write(dir.join(name), b"x").unwrap();
        }
    }

    #[test]
    fn keeps_crypto_and_session() {
        let dir = std::env::temp_dir().join(format!(
            "tess-wipe-keep-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&dir).unwrap();
        populate(&dir);

        wipe_local_stores(&dir, /* keep_crypto = */ true);

        for name in ALL {
            let kept = name.starts_with("matrix-sdk-crypto")
                || *name == "session.json"
                || *name == "thread_read_state.db";
            assert_eq!(
                dir.join(name).exists(),
                kept,
                "{name}: expected kept={kept}"
            );
        }
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn drops_crypto_when_not_kept() {
        let dir = std::env::temp_dir().join(format!(
            "tess-wipe-all-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&dir).unwrap();
        populate(&dir);

        wipe_local_stores(&dir, /* keep_crypto = */ false);

        for name in ALL {
            let kept = *name == "session.json" || *name == "thread_read_state.db";
            assert_eq!(
                dir.join(name).exists(),
                kept,
                "{name}: expected kept={kept}"
            );
        }
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn missing_files_are_not_an_error() {
        let dir = std::env::temp_dir().join(format!(
            "tess-wipe-empty-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&dir).unwrap();
        wipe_local_stores(&dir, true); // no panic on an empty dir
        let _ = fs::remove_dir_all(&dir);
    }
}

#[cfg(test)]
mod envelope_tests {
    use super::*;
    use matrix_sdk::authentication::matrix::MatrixSession;
    use matrix_sdk::ruma::{owned_device_id, owned_user_id};
    use matrix_sdk::{SessionMeta, SessionTokens};

    fn sample_oauth_envelope() -> SessionEnvelope {
        SessionEnvelope::OAuth {
            client_id: ClientId::new("test-client-id".to_owned()),
            user: UserSession {
                meta: SessionMeta {
                    user_id: owned_user_id!("@alice:example.org"),
                    device_id: owned_device_id!("DEVICEID"),
                },
                tokens: SessionTokens {
                    access_token: "oauth-access-token".to_owned(),
                    refresh_token: Some("oauth-refresh-token".to_owned()),
                },
            },
        }
    }

    fn sample_native_envelope() -> SessionEnvelope {
        SessionEnvelope::Native {
            homeserver_url: "https://matrix.example.org".to_owned(),
            session: MatrixSession {
                meta: SessionMeta {
                    user_id: owned_user_id!("@bob:example.org"),
                    device_id: owned_device_id!("DEVICEID2"),
                },
                tokens: SessionTokens {
                    access_token: "native-access-token".to_owned(),
                    refresh_token: None,
                },
            },
        }
    }

    #[test]
    fn oauth_envelope_round_trips_with_auth_tag() {
        let json = serde_json::to_string(&sample_oauth_envelope()).unwrap();
        assert!(json.contains("\"auth\":\"oauth\""));

        let restored: SessionEnvelope = serde_json::from_str(&json).unwrap();
        match restored {
            SessionEnvelope::OAuth { client_id, user } => {
                assert_eq!(client_id.as_str(), "test-client-id");
                assert_eq!(user.meta.user_id.as_str(), "@alice:example.org");
                assert_eq!(user.tokens.access_token, "oauth-access-token");
            }
            SessionEnvelope::Native { .. } => panic!("expected OAuth variant"),
        }
    }

    #[test]
    fn native_envelope_round_trips_with_auth_tag() {
        let json = serde_json::to_string(&sample_native_envelope()).unwrap();
        assert!(json.contains("\"auth\":\"native\""));

        let restored: SessionEnvelope = serde_json::from_str(&json).unwrap();
        match restored {
            SessionEnvelope::Native {
                homeserver_url,
                session,
            } => {
                assert_eq!(homeserver_url, "https://matrix.example.org");
                assert_eq!(session.meta.user_id.as_str(), "@bob:example.org");
                assert_eq!(session.tokens.access_token, "native-access-token");
                assert!(session.tokens.refresh_token.is_none());
            }
            SessionEnvelope::OAuth { .. } => panic!("expected Native variant"),
        }
    }

    #[test]
    fn unknown_auth_tag_fails_cleanly_rather_than_panicking() {
        let json = r#"{"auth":"carrier_pigeon"}"#;
        let result: Result<SessionEnvelope, _> = serde_json::from_str(json);
        assert!(
            result.is_err(),
            "unknown auth tag must be rejected, not silently accepted"
        );
    }

    /// Every `session.json` persisted before the SessionEnvelope migration has
    /// this exact shape: bare `client_id` + flattened `UserSession` fields,
    /// with no `"auth"` tag at all (that tag didn't exist yet). These files
    /// are still sitting on disk for every already-logged-in user, so parsing
    /// must treat a missing tag as legacy OAuth rather than failing outright.
    #[test]
    fn legacy_untagged_json_parses_as_oauth() {
        let json = r#"{
            "client_id": "legacy-client-id",
            "user_id": "@alice:example.org",
            "device_id": "DEVICEID",
            "access_token": "legacy-access-token",
            "refresh_token": "legacy-refresh-token"
        }"#;

        let restored: SessionEnvelope =
            serde_json::from_str(json).expect("legacy session JSON must still parse");
        match restored {
            SessionEnvelope::OAuth { client_id, user } => {
                assert_eq!(client_id.as_str(), "legacy-client-id");
                assert_eq!(user.meta.user_id.as_str(), "@alice:example.org");
                assert_eq!(user.tokens.access_token, "legacy-access-token");
            }
            SessionEnvelope::Native { .. } => panic!("expected OAuth variant"),
        }
    }

    #[test]
    fn oauth_envelope_serializes_flat_with_tokens() {
        let v = serde_json::to_value(sample_oauth_envelope()).unwrap();
        assert_eq!(v["auth"], "oauth");
        assert_eq!(v["client_id"], "test-client-id");
        assert_eq!(v["user_id"], "@alice:example.org");
        assert_eq!(v["device_id"], "DEVICEID");
        assert_eq!(v["access_token"], "oauth-access-token");
        assert_eq!(v["refresh_token"], "oauth-refresh-token");
    }

    #[test]
    fn oauth_envelope_preserves_refresh_token_through_roundtrip() {
        let json = serde_json::to_string(&sample_oauth_envelope()).unwrap();
        let SessionEnvelope::OAuth { user, .. } = serde_json::from_str(&json).unwrap() else {
            panic!("expected OAuth variant");
        };
        assert_eq!(
            user.tokens.refresh_token.as_deref(),
            Some("oauth-refresh-token")
        );
        assert_eq!(user.meta.device_id.as_str(), "DEVICEID");
    }

    #[test]
    fn oauth_tag_without_client_id_is_rejected() {
        let json = r#"{"auth":"oauth","user_id":"@a:x.org","device_id":"D","access_token":"t"}"#;
        assert!(serde_json::from_str::<SessionEnvelope>(json).is_err());
    }

    #[test]
    fn native_tag_without_homeserver_url_is_rejected() {
        let json = r#"{"auth":"native","user_id":"@a:x.org","device_id":"D","access_token":"t"}"#;
        assert!(serde_json::from_str::<SessionEnvelope>(json).is_err());
    }

    #[test]
    fn native_envelope_serializes_homeserver_url_alongside_session() {
        let v = serde_json::to_value(sample_native_envelope()).unwrap();
        assert_eq!(v["auth"], "native");
        assert_eq!(v["homeserver_url"], "https://matrix.example.org");
        assert_eq!(v["user_id"], "@bob:example.org");
        assert!(v.get("refresh_token").is_none() || v["refresh_token"].is_null());
    }

    #[test]
    fn unknown_tag_error_names_the_tag() {
        let e = match serde_json::from_str::<SessionEnvelope>(r#"{"auth":"carrier_pigeon"}"#) {
            Ok(_) => panic!("unknown tag must be rejected"),
            Err(e) => e.to_string(),
        };
        assert!(e.contains("carrier_pigeon"), "{e}");
    }

    #[test]
    fn non_string_auth_tag_is_rejected_not_treated_as_legacy() {
        for json in [r#"{"auth":5}"#, r#"{"auth":null}"#, r#"{"auth":{"a":1}}"#] {
            let e = match serde_json::from_str::<SessionEnvelope>(json) {
                Ok(_) => panic!("{json} must be rejected"),
                Err(e) => e.to_string(),
            };
            assert!(e.contains("`auth` must be a string"), "{json}: {e}");
        }
    }

    #[test]
    fn missing_auth_tag_is_still_the_legacy_oauth_shape() {
        // Fails as an OAuth envelope (missing fields), not on the tag.
        let e = match serde_json::from_str::<SessionEnvelope>("{}") {
            Ok(_) => panic!("empty object is not a session"),
            Err(e) => e.to_string(),
        };
        assert!(!e.contains("`auth`"), "{e}");
        assert!(e.contains("client_id"), "{e}");
    }

    #[test]
    fn non_object_json_is_rejected() {
        for json in ["null", "[]", "\"oauth\"", "42"] {
            assert!(
                serde_json::from_str::<SessionEnvelope>(json).is_err(),
                "{json}"
            );
        }
    }

    #[test]
    fn native_envelope_with_refresh_token_roundtrips() {
        let SessionEnvelope::Native {
            homeserver_url,
            mut session,
        } = sample_native_envelope()
        else {
            unreachable!()
        };
        session.tokens.refresh_token = Some("rt".to_owned());
        let env = SessionEnvelope::Native {
            homeserver_url,
            session,
        };
        let json = serde_json::to_string(&env).unwrap();
        let SessionEnvelope::Native { session, .. } = serde_json::from_str(&json).unwrap() else {
            panic!("expected Native variant");
        };
        assert_eq!(session.tokens.refresh_token.as_deref(), Some("rt"));
    }
}

#[cfg(test)]
mod ffi_state_tests {
    use super::*;

    fn temp_dir(tag: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!(
            "tess-session-{tag}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }

    #[test]
    fn oauth_await_slice_requires_a_flow() {
        let c = ClientFfi::new();
        let r = c.oauth_await_slice();
        assert!(!r.pending);
        assert!(!r.ok);
        assert!(r.message.contains("oauth_begin"), "{}", r.message);
    }

    #[test]
    fn oauth_commit_requires_a_flow() {
        let mut c = ClientFfi::new();
        let r = c.oauth_commit();
        assert!(!r.ok);
        assert!(r.message.contains("oauth_begin"), "{}", r.message);
        assert!(c.client.is_none());
    }

    #[test]
    fn restore_session_rejects_unknown_auth_tag_with_parse_error() {
        let mut c = ClientFfi::new();
        let r = c.restore_session(r#"{"auth":"carrier_pigeon"}"#);
        assert!(!r.ok);
        assert!(
            r.message.starts_with("parse session JSON:"),
            "{}",
            r.message
        );
        assert!(r.message.contains("carrier_pigeon"), "{}", r.message);
        assert!(c.client.is_none());
    }

    #[test]
    fn restore_session_rejects_empty_input() {
        let mut c = ClientFfi::new();
        let r = c.restore_session("");
        assert!(!r.ok);
        assert!(
            r.message.starts_with("parse session JSON:"),
            "{}",
            r.message
        );
    }

    #[test]
    fn get_server_info_is_empty_when_not_logged_in() {
        assert!(!ClientFfi::new().get_server_info().ok);
    }

    #[test]
    fn clear_caches_requires_a_logged_in_client() {
        let mut c = ClientFfi::new();
        let r = c.clear_caches();
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }

    #[test]
    fn reload_client_requires_a_logged_in_client() {
        let mut c = ClientFfi::new();
        let r = c.reload_client();
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }

    #[test]
    fn logout_without_client_removes_the_data_dir() {
        let dir = temp_dir("logout");
        let mut c = ClientFfi::new();
        c.set_data_dir(dir.to_str().unwrap());
        assert!(dir.exists());
        std::fs::write(dir.join("matrix-sdk-state.sqlite3"), b"x").unwrap();

        let r = c.logout();
        assert!(r.ok, "{}", r.message);
        assert!(!dir.exists(), "logout must wipe the per-account directory");
    }

    #[test]
    fn logout_without_client_tolerates_a_missing_data_dir() {
        let dir = temp_dir("logout-missing");
        let mut c = ClientFfi::new();
        c.set_data_dir(dir.to_str().unwrap());
        std::fs::remove_dir_all(&dir).unwrap();
        assert!(c.logout().ok);
    }
}

#[cfg(test)]
mod capability_tests {
    use super::{
        parse_federation_version, parse_login_flows, parse_upload_limit, preview_url_enabled,
    };
    use serde_json::json;

    #[test]
    fn federation_version_reads_name_and_version() {
        let body = json!({ "server": { "name": "Synapse", "version": "1.120.0" } });
        assert_eq!(
            parse_federation_version(&body),
            ("Synapse".to_owned(), "1.120.0".to_owned())
        );
        assert_eq!(
            parse_federation_version(&serde_json::Value::Null),
            (String::new(), String::new())
        );
    }

    #[test]
    fn upload_limit_defaults_to_zero() {
        assert_eq!(
            parse_upload_limit(&json!({ "m.upload.size": 52428800 })),
            52_428_800
        );
        assert_eq!(parse_upload_limit(&json!({})), 0);
        assert_eq!(parse_upload_limit(&serde_json::Value::Null), 0);
    }

    #[test]
    fn login_flows_strip_prefix_and_dedupe() {
        let body = json!({ "flows": [
            { "type": "m.login.sso" },
            { "type": "m.login.password" },
            { "type": "m.login.sso" },
            { "bogus": 1 }
        ] });
        assert_eq!(parse_login_flows(&body), vec!["sso", "password"]);
        assert!(parse_login_flows(&serde_json::Value::Null).is_empty());
    }

    #[test]
    fn preview_url_capability_defaults_to_enabled() {
        assert!(preview_url_enabled(&serde_json::Value::Null));
        assert!(preview_url_enabled(&json!({ "capabilities": {} })));
    }

    #[test]
    fn preview_url_capability_reads_stable_and_unstable_keys() {
        let stable = json!({ "capabilities": { "m.preview_url": { "enabled": false } } });
        assert!(!preview_url_enabled(&stable));
        let unstable = json!({
            "capabilities": { "io.element.msc4452.preview_url": { "enabled": false } }
        });
        assert!(!preview_url_enabled(&unstable));
    }

    #[test]
    fn preview_url_capability_prefers_stable_key() {
        let both = json!({ "capabilities": {
            "m.preview_url": { "enabled": true },
            "io.element.msc4452.preview_url": { "enabled": false }
        } });
        assert!(preview_url_enabled(&both));
    }
}
