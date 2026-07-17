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

use super::{err, oauth_err, ok, ClientFfi};
use crate::ffi::{OAuthBegin, OpResult};
use crate::oauth;

use std::sync::atomic::Ordering;

#[cfg(not(test))]
use super::BACKUP_STATE_UNKNOWN;

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
        let auth_tag = value.get("auth").and_then(|v| v.as_str()).unwrap_or("oauth");
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

impl ClientFfi {
    // -----------------------------------------------------------------------
    // OAuth login
    // -----------------------------------------------------------------------

    pub fn oauth_begin(&mut self, homeserver: &str, register_account: bool) -> OAuthBegin {
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

        match self.rt.block_on(oauth::begin(&hs, &path, register_account)) {
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
            Err(e) => oauth_err(e.to_string()),
        }
    }

    pub fn homeserver_supports_registration(&self, homeserver: &str) -> bool {
        let hs = homeserver.to_owned();
        let rt = &self.rt;
        // Keep a tokio context in TLS for the whole call so the throwaway Client
        // built inside `supports_registration` drops with a runtime handle
        // available — matrix-sdk's store/crypto teardown calls Handle::current()
        // in Drop. Same hazard guarded by oauth_await_callback / restore_session /
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

    pub fn oauth_await_callback(&mut self) -> OpResult {
        let Some(flow) = self.oauth_flow.take() else {
            return err("no oauth flow in progress; call oauth_begin first");
        };

        match self.rt.block_on(oauth::await_callback(flow)) {
            Ok(client) => {
                // Enter the runtime so any prior Client we're overwriting drops
                // with a tokio context in TLS — matrix-sdk's SqliteStateStore /
                // deadpool tear-down calls Handle::current() in its Drop impl.
                let _guard = self.rt.enter();
                self.client = Some(client);
                ok("")
            }
            Err(e) => err(format!("{e:#}")),
        }
    }

    pub fn oauth_cancel(&mut self) {
        if let Some(flow) = self.oauth_flow.take() {
            oauth::cancel(&flow);
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

        let result = self.rt.block_on(async move {
            match envelope {
                SessionEnvelope::OAuth { client_id, user } => {
                    let homeserver = user.meta.user_id.server_name().to_string();
                    let client = oauth::build_configured_client(&homeserver, &path).await?;

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
                    let client = oauth::build_configured_client(&homeserver_url, &path).await?;
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
            Err(e) => err(e.to_string()),
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
    /// Returns a JSON blob or empty string when not logged in or on any error.
    /// Blocks the calling thread — call only from a worker thread.
    pub fn get_server_info(&self) -> String {
        let Some(client) = &self.client else {
            return String::new();
        };
        let client = client.clone();
        let http = self.http_client.clone();
        let prefix_slot = self.profile_fields_prefix.clone();

        self.rt.block_on(async move {
            let base = {
                let url = client.homeserver().to_string();
                url.trim_end_matches('/').to_owned()
            };
            let access_token = client.access_token().unwrap_or_default();
            let server_name = client
                .user_id()
                .map(|uid| uid.server_name().to_string())
                .unwrap_or_default();

            let (versions_resp, caps_resp) = tokio::join!(
                http.get(format!("{base}/_matrix/client/versions")).send(),
                http.get(format!("{base}/_matrix/client/v3/capabilities"))
                    .bearer_auth(&access_token)
                    .send()
            );

            let versions_json: serde_json::Value = match versions_resp {
                Ok(r) => r.json().await.unwrap_or(serde_json::Value::Null),
                Err(_) => serde_json::Value::Null,
            };

            let spec_versions: Vec<String> = versions_json["versions"]
                .as_array()
                .map(|arr| {
                    arr.iter()
                        .filter_map(|s| s.as_str().map(str::to_owned))
                        .collect()
                })
                .unwrap_or_default();

            let supports_msc3030 = versions_json
                .pointer("/unstable_features/org.matrix.msc3030")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);

            let supports_msc4133_stable = versions_json
                .pointer("/unstable_features/uk.tcpip.msc4133.stable")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            let supports_msc4133_unstable = versions_json
                .pointer("/unstable_features/uk.tcpip.msc4133")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            let supports_msc4133 = supports_msc4133_stable || supports_msc4133_unstable;

            let supports_qr_grant = versions_json
                .pointer("/unstable_features/org.matrix.msc4108")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);

            *prefix_slot.write().unwrap() = if supports_msc4133_stable {
                Some("/_matrix/client/v3".to_owned())
            } else if supports_msc4133_unstable {
                Some("/_matrix/client/unstable/uk.tcpip.msc4133".to_owned())
            } else {
                None
            };

            let caps: serde_json::Value = match caps_resp {
                Ok(r) => r.json().await.unwrap_or(serde_json::Value::Null),
                Err(_) => serde_json::Value::Null,
            };

            let cap_bool = |key: &str| -> bool {
                caps.pointer(&format!("/capabilities/{key}/enabled"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(true)
            };
            let default_room_ver = caps
                .pointer("/capabilities/m.room_versions/default")
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_owned();
            let profile_fields_enabled = caps
                .pointer("/capabilities/m.profile_fields/enabled")
                .and_then(|v| v.as_bool())
                .unwrap_or(true);

            let supports_calls = crate::client::rtc::transport::probe_livekit_support(
                &http,
                &base,
                &access_token,
                &server_name,
            )
            .await;

            serde_json::json!({
                "homeserver": base,
                "spec_versions": spec_versions,
                "supports_msc3030": supports_msc3030,
                "can_change_password": cap_bool("m.change_password"),
                "can_set_displayname": cap_bool("m.set_displayname"),
                "can_set_avatar": cap_bool("m.set_avatar_url"),
                "default_room_version": default_room_ver,
                "supports_profile_fields": supports_msc4133,
                "profile_fields_enabled": profile_fields_enabled,
                "supports_qr_grant": supports_qr_grant,
                "supports_calls": supports_calls
            })
            .to_string()
        })
    }

    /// Async counterpart of `get_server_info`. Spawns the fetch on the tokio
    /// runtime and fires `on_server_info_ready(request_id, json)` on completion
    /// (empty string on failure or when not logged in). Does not pin a thread.
    #[cfg(not(test))]
    pub fn get_server_info_async(&self, request_id: u64) {
        let Some(client) = self.client.clone() else {
            if let Some(ref h) = self.handler {
                let g = h.lock();
                g.on_server_info_ready(request_id, "");
            }
            return;
        };
        let handler = self.handler.clone();
        let http = self.http_client.clone();
        let prefix_slot = self.profile_fields_prefix.clone();

        self.rt.spawn(async move {
            let base = {
                let url = client.homeserver().to_string();
                url.trim_end_matches('/').to_owned()
            };
            let access_token = client.access_token().unwrap_or_default();
            let server_name = client
                .user_id()
                .map(|uid| uid.server_name().to_string())
                .unwrap_or_default();

            let (versions_resp, caps_resp) = tokio::join!(
                http.get(format!("{base}/_matrix/client/versions")).send(),
                http.get(format!("{base}/_matrix/client/v3/capabilities"))
                    .bearer_auth(&access_token)
                    .send()
            );

            let versions_json: serde_json::Value = match versions_resp {
                Ok(r) => r.json().await.unwrap_or(serde_json::Value::Null),
                Err(_) => serde_json::Value::Null,
            };

            let spec_versions: Vec<String> = versions_json["versions"]
                .as_array()
                .map(|arr| {
                    arr.iter()
                        .filter_map(|s| s.as_str().map(str::to_owned))
                        .collect()
                })
                .unwrap_or_default();

            let supports_msc3030 = versions_json
                .pointer("/unstable_features/org.matrix.msc3030")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);

            let supports_msc4133_stable = versions_json
                .pointer("/unstable_features/uk.tcpip.msc4133.stable")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            let supports_msc4133_unstable = versions_json
                .pointer("/unstable_features/uk.tcpip.msc4133")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            let supports_msc4133 = supports_msc4133_stable || supports_msc4133_unstable;

            let supports_qr_grant = versions_json
                .pointer("/unstable_features/org.matrix.msc4108")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);

            *prefix_slot.write().unwrap() = if supports_msc4133_stable {
                Some("/_matrix/client/v3".to_owned())
            } else if supports_msc4133_unstable {
                Some("/_matrix/client/unstable/uk.tcpip.msc4133".to_owned())
            } else {
                None
            };

            let caps: serde_json::Value = match caps_resp {
                Ok(r) => r.json().await.unwrap_or(serde_json::Value::Null),
                Err(_) => serde_json::Value::Null,
            };

            let cap_bool = |key: &str| -> bool {
                caps.pointer(&format!("/capabilities/{key}/enabled"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(true)
            };
            let default_room_ver = caps
                .pointer("/capabilities/m.room_versions/default")
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_owned();
            let profile_fields_enabled = caps
                .pointer("/capabilities/m.profile_fields/enabled")
                .and_then(|v| v.as_bool())
                .unwrap_or(true);

            let supports_calls = crate::client::rtc::transport::probe_livekit_support(
                &http,
                &base,
                &access_token,
                &server_name,
            )
            .await;

            let json = serde_json::json!({
                "homeserver": base,
                "spec_versions": spec_versions,
                "supports_msc3030": supports_msc3030,
                "can_change_password": cap_bool("m.change_password"),
                "can_set_displayname": cap_bool("m.set_displayname"),
                "can_set_avatar": cap_bool("m.set_avatar_url"),
                "default_room_version": default_room_ver,
                "supports_profile_fields": supports_msc4133,
                "profile_fields_enabled": profile_fields_enabled,
                "supports_qr_grant": supports_qr_grant,
                "supports_calls": supports_calls
            })
            .to_string();

            if let Some(h) = handler {
                let g = h.lock();
                g.on_server_info_ready(request_id, &json);
            }
        });
    }

    #[cfg(test)]
    pub fn get_server_info_async(&self, _request_id: u64) {}

    // -----------------------------------------------------------------------
    // Cache management
    // -----------------------------------------------------------------------

    /// Drop the matrix-sdk Client and wipe the on-disk event cache. State and
    /// crypto stores survive, so `restore_session` lands on the same identity
    /// and room list and `start_sync` resumes from the saved sync token.
    ///
    /// Calling `event_cache().clear_all_rooms()` on a live client is unsafe in
    /// our setup: any still-subscribed observer (matrix_sdk_ui::Timeline's
    /// internal as_vector, or one of its aborted-but-not-yet-dropped tasks)
    /// can panic mid-update inside the linked-chunk's write lock, poisoning
    /// it; the next `clear_pending` unwraps the PoisonError and aborts the
    /// process. Dropping the Client outright deletes every observer + linked
    /// chunk in one shot, so the subsequent file delete touches nothing
    /// in-memory.
    pub fn clear_caches(&mut self) -> crate::ffi::OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }

        // Drain every live Timeline / ThreadList first, so their spawned
        // tasks are signalled cancelled before the Client they reference
        // disappears underneath them.
        #[cfg(not(test))]
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

        // Drop the Client inside the runtime context: matrix-sdk's
        // SqliteStateStore calls Handle::current() in its Drop chain.
        {
            let _guard = self.rt.enter();
            drop(self.client.take());
        }

        #[cfg(not(test))]
        {
            {
                let mut db = self.app_cache_db.lock();
                *db = None;
            }
            {
                let mut db = self.search_db.lock();
                *db = None;
            }
            let _ = std::fs::remove_file(self.data_dir.join("app_cache.db"));
            let _ = std::fs::remove_file(self.data_dir.join("search_index.db"));
            for sidecar in [
                "matrix-sdk-event-cache.sqlite3",
                "matrix-sdk-event-cache.sqlite3-wal",
                "matrix-sdk-event-cache.sqlite3-shm",
            ] {
                let _ = std::fs::remove_file(self.data_dir.join(sidecar));
            }
            {
                let mut cache = self.backfill_previews.lock();
                cache.clear();
            }
        }

        ok(String::new())
    }

    // -----------------------------------------------------------------------
    // Logout
    // -----------------------------------------------------------------------

    pub fn logout(&mut self) -> OpResult {
        self.stop_sync();

        // Close the cache DBs before remove_dir_all so WAL frames are
        // checkpointed and file handles are released (required on Windows).
        // Also clear the in-memory backfill cache so nothing stale persists
        // after a re-login on the same process.
        #[cfg(not(test))]
        {
            {
                let mut db = self.app_cache_db.lock();
                *db = None;
            }
            {
                let mut db = self.search_db.lock();
                *db = None;
            }
            {
                let mut cache = self.backfill_previews.lock();
                cache.clear();
            }
        }

        if let Some(flow) = self.oauth_flow.take() {
            oauth::cancel(&flow);
        }

        // Abort each timeline's background tasks before dropping the handles,
        // mirroring Drop. Plain `.clear()` drops the AbortHandles without
        // cancelling the spawned futures, which would keep an Arc<Timeline>
        // (and the HTTP client) alive past the token revocation below.
        #[cfg(not(test))]
        {
            let _guard = self.rt.enter();
            for (_, th) in self.timelines.write().drain() {
                th.cancelled.store(true, Ordering::Release);
                for h in th.abort_tasks {
                    h.abort();
                }
            }
        }
        #[cfg(not(test))]
        {
            self.imported_keys.store(0, Ordering::Relaxed);
            self.backup_state_code
                .store(BACKUP_STATE_UNKNOWN, Ordering::Relaxed);
        }
        self.media_upload_limit.store(0, Ordering::Relaxed);
        *self.profile_fields_prefix.write().unwrap() = None;

        let Some(client) = self.client.take() else {
            let _ = std::fs::remove_dir_all(&self.data_dir);
            return ok("");
        };

        let is_oauth = client.oauth().full_session().is_some();
        let revoke: Result<(), String> = self.rt.block_on(async move {
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

        let _ = std::fs::remove_dir_all(&self.data_dir);

        match revoke {
            Ok(_) => ok(""),
            Err(e) => err(format!("logout failed (local store cleared): {e}")),
        }
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
        assert!(result.is_err(), "unknown auth tag must be rejected, not silently accepted");
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
}
