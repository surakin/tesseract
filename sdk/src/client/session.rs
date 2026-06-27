//! OAuth login, session restore/export, server info, logout, cache clearing.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use anyhow::Context as _;
use matrix_sdk::{
    authentication::oauth::{ClientId, OAuthSession, UserSession},
    encryption::{BackupDownloadStrategy, EncryptionSettings},
    store::RoomLoadSettings,
    Client, ThreadingSupport,
};
use serde::{Deserialize, Serialize};

use super::{err, ok, oauth_err, ClientFfi};
use crate::ffi::{OAuthBegin, OpResult};
use crate::oauth;

use std::sync::atomic::Ordering;

#[cfg(not(test))]
use super::BACKUP_STATE_UNKNOWN;

/// Serialisable wrapper for a full OAuth session (client_id + user session).
#[derive(Serialize, Deserialize)]
pub(super) struct PersistedSession {
    pub(super) client_id: ClientId,
    #[serde(flatten)]
    pub(super) user: UserSession,
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
        let persisted: PersistedSession = match serde_json::from_str(session_json) {
            Ok(s) => s,
            Err(e) => return err(format!("parse session JSON: {e}")),
        };

        let homeserver = persisted.user.meta.user_id.server_name().to_string();
        let path = self.data_dir.clone();
        let _ = std::fs::create_dir_all(&path);

        let result = self.rt.block_on(async move {
            let client = Client::builder()
                .server_name_or_homeserver_url(homeserver)
                .sqlite_store(&path, None)
                .handle_refresh_tokens()
                .user_agent(crate::oauth::build_user_agent())
                .http_client(crate::oauth::build_sdk_http_client())
                .with_encryption_settings(EncryptionSettings {
                    backup_download_strategy: BackupDownloadStrategy::AfterDecryptionFailure,
                    // Bootstrap cross-signing automatically (see oauth.rs for
                    // the rationale): recovery().enable() stores existing
                    // cross-signing keys but never creates them, so without
                    // this a fresh device is never verified and RecoveryState
                    // sticks at Incomplete.
                    auto_enable_cross_signing: true,
                    ..Default::default()
                })
                // Keep parity with oauth.rs: route sync'd thread events to
                // per-thread linked chunks so the focused thread Timeline
                // sees live updates.
                .with_threading_support(ThreadingSupport::Enabled {
                    with_subscriptions: false,
                })
                .build()
                .await
                .context("build client")?;

            let session = OAuthSession {
                client_id: persisted.client_id,
                user: persisted.user,
            };
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
                    let Some(full) = c.oauth().full_session() else {
                        return Ok(());
                    };
                    let user_id = full.user.meta.user_id.to_string();
                    let persisted = PersistedSession {
                        client_id: full.client_id,
                        user: full.user,
                    };
                    let json = serde_json::to_string(&persisted)
                        .map_err(|e| Box::new(e) as Box<dyn std::error::Error + Send + Sync>)?;
                    crate::ffi::persist_session(&user_id, &json);
                    Ok(())
                }),
            );

            anyhow::Ok(client)
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
        let Some(session) = client.oauth().full_session() else {
            return String::new();
        };
        let persisted = PersistedSession {
            client_id: session.client_id,
            user: session.user,
        };
        serde_json::to_string(&persisted).unwrap_or_default()
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

            #[cfg(feature = "calls")]
            let supports_calls = crate::client::rtc::transport::probe_livekit_support(
                &http, &base, &access_token, &server_name,
            )
            .await;
            #[cfg(not(feature = "calls"))]
            let supports_calls = false;

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

            #[cfg(feature = "calls")]
            let supports_calls = crate::client::rtc::transport::probe_livekit_support(
                &http, &base, &access_token, &server_name,
            )
            .await;
            #[cfg(not(feature = "calls"))]
            let supports_calls = false;

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

        let revoke = self
            .rt
            .block_on(async move { client.oauth().logout().await });

        let _ = std::fs::remove_dir_all(&self.data_dir);

        match revoke {
            Ok(_) => ok(""),
            Err(e) => err(format!("oauth logout failed (local store cleared): {e}")),
        }
    }
}
