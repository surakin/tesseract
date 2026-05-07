use std::path::PathBuf;

use anyhow::Context as _;
use serde::{Deserialize, Serialize};
use matrix_sdk::{
    authentication::oauth::{ClientId, OAuthSession, UserSession},
    ruma::events::room::message::RoomMessageEventContent,
    store::RoomLoadSettings,
    Client,
};
use tokio::runtime::Runtime;
use tokio::sync::watch;

use crate::ffi::{OAuthBegin, OpResult};
use crate::oauth;

#[cfg(not(test))]
use std::sync::{Arc, Mutex};
#[cfg(not(test))]
use cxx::UniquePtr;
#[cfg(not(test))]
use matrix_sdk::{
    config::SyncSettings,
    ruma::events::room::message::MessageType,
    SessionChange,
};
#[cfg(not(test))]
use crate::ffi::{EventHandlerBridge, TimelineEvent};

// ---------------------------------------------------------------------------

fn ok(msg: impl Into<String>) -> OpResult {
    OpResult { ok: true, message: msg.into() }
}

fn err(msg: impl Into<String>) -> OpResult {
    OpResult { ok: false, message: msg.into() }
}

fn oauth_err(msg: impl Into<String>) -> OAuthBegin {
    OAuthBegin {
        ok:           false,
        message:      msg.into(),
        auth_url:     String::new(),
        redirect_uri: String::new(),
    }
}

/// Per-user data directory used for the SDK's encrypted sqlite store. Created
/// on first use; same location is reused on every launch so encryption keys
/// and rooms cache survive across sessions.
fn data_dir() -> PathBuf {
    let base = dirs_like_home().unwrap_or_else(std::env::temp_dir);
    let dir  = base.join("tesseract").join("matrix-store");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

fn dirs_like_home() -> Option<PathBuf> {
    // Cheap stand-in for the `dirs` crate; covers the platforms we ship to.
    #[cfg(target_os = "windows")]
    {
        std::env::var_os("APPDATA").map(PathBuf::from)
    }
    #[cfg(target_os = "macos")]
    {
        std::env::var_os("HOME")
            .map(PathBuf::from)
            .map(|h| h.join("Library").join("Application Support"))
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
    }
}

// ---------------------------------------------------------------------------

#[cfg(not(test))]
struct SendHandler(UniquePtr<EventHandlerBridge>);
#[cfg(not(test))]
unsafe impl Send for SendHandler {}
#[cfg(not(test))]
impl std::ops::Deref for SendHandler {
    type Target = UniquePtr<EventHandlerBridge>;
    fn deref(&self) -> &Self::Target { &self.0 }
}

/// Serialisable wrapper for a full OAuth session (client_id + user session).
#[derive(Serialize, Deserialize)]
struct PersistedSession {
    client_id: ClientId,
    #[serde(flatten)]
    user: UserSession,
}

pub struct ClientFfi {
    rt:           Runtime,
    client:       Option<Client>,
    stop_tx:      Option<watch::Sender<bool>>,
    oauth_flow:   Option<oauth::PendingFlow>,
}

impl ClientFfi {
    pub fn new() -> Self {
        // try_init() instead of init() so a second ClientFfi (e.g. after
        // logout-then-login in the same process) doesn't panic.
        let _ = tracing_subscriber::fmt()
            .with_env_filter(
                tracing_subscriber::EnvFilter::from_default_env()
                    .add_directive("matrix_sdk=info".parse().unwrap()),
            )
            .try_init();

        Self {
            rt:         Runtime::new().expect("tokio runtime"),
            client:     None,
            stop_tx:    None,
            oauth_flow: None,
        }
    }

    // -----------------------------------------------------------------------
    // OAuth login (Matrix Authentication Service)
    // -----------------------------------------------------------------------

    pub fn oauth_begin(&mut self, homeserver: &str) -> OAuthBegin {
        // Cancel any previous half-finished flow before starting a new one.
        if let Some(prev) = self.oauth_flow.take() {
            oauth::cancel(&prev);
        }

        let hs   = homeserver.to_owned();
        let path = data_dir();

        match self.rt.block_on(oauth::begin(&hs, &path)) {
            Ok(begin) => {
                let auth_url     = begin.auth_url;
                let redirect_uri = begin.redirect_uri;
                self.oauth_flow  = Some(begin.flow);
                OAuthBegin { ok: true, message: String::new(), auth_url, redirect_uri }
            }
            Err(e) => oauth_err(e.to_string()),
        }
    }

    pub fn oauth_await_callback(&mut self) -> OpResult {
        let Some(flow) = self.oauth_flow.take() else {
            return err("no oauth flow in progress; call oauth_begin first");
        };

        match self.rt.block_on(oauth::await_callback(flow)) {
            Ok(client) => { self.client = Some(client); ok("") }
            Err(e)     => err(e.to_string()),
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
            Ok(s)  => s,
            Err(e) => return err(format!("parse session JSON: {e}")),
        };

        let homeserver = persisted.user.meta.user_id.server_name().to_string();
        let path       = data_dir();

        let result = self.rt.block_on(async move {
            let client = Client::builder()
                .server_name_or_homeserver_url(homeserver)
                .sqlite_store(&path, None)
                .build()
                .await
                .context("build client")?;

            let session = OAuthSession { client_id: persisted.client_id, user: persisted.user };
            client
                .oauth()
                .restore_session(session, RoomLoadSettings::default())
                .await
                .context("restore oauth session")?;

            anyhow::Ok(client)
        });

        match result {
            Ok(c)  => { self.client = Some(c); ok("") }
            Err(e) => err(e.to_string()),
        }
    }

    pub fn export_session(&self) -> String {
        let Some(client) = &self.client else { return String::new() };
        let Some(session) = client.oauth().full_session() else { return String::new() };
        let persisted = PersistedSession { client_id: session.client_id, user: session.user };
        serde_json::to_string(&persisted).unwrap_or_default()
    }

    #[cfg(not(test))]
    pub fn start_sync(
        &mut self,
        handler: UniquePtr<EventHandlerBridge>,
    ) {
        let Some(client) = self.client.clone() else { return };

        let (stop_tx, mut stop_rx) = watch::channel(false);
        self.stop_tx = Some(stop_tx);

        // Wrap in SendHandler to allow sharing across threads (Qt signals are thread-safe).
        let handler = Arc::new(Mutex::new(SendHandler(handler)));

        // Subscribe to OAuth session changes so we can re-export the session
        // JSON every time the SDK refreshes tokens; the UI persists it.
        {
            let h            = Arc::clone(&handler);
            let client_clone = client.clone();
            self.rt.spawn(async move {
                let mut changes = client_clone.subscribe_to_session_changes();
                loop {
                    match changes.recv().await {
                        Ok(SessionChange::TokensRefreshed) => {
                            // Emit the *full* session shape (client_id + user)
                            // so it round-trips through restore_session(),
                            // which expects a PersistedSession.
                            let Some(full) = client_clone.oauth().full_session() else {
                                continue;
                            };
                            let persisted = PersistedSession {
                                client_id: full.client_id,
                                user:      full.user,
                            };
                            let Ok(json) = serde_json::to_string(&persisted) else { continue };
                            if let Ok(guard) = h.lock() {
                                guard.on_session_refreshed(&json);
                            }
                        }
                        // Logout/unknown change — UI handles those via other paths.
                        Ok(_)  => continue,
                        Err(_) => break,
                    }
                }
            });
        }

        self.rt.spawn(async move {
            let settings = SyncSettings::default();

            // Attach a room-message event handler.
            {
                let h = Arc::clone(&handler);
                client.add_event_handler(
                    move |ev: matrix_sdk::ruma::events::SyncMessageLikeEvent<
                        RoomMessageEventContent,
                    >,
                          room: matrix_sdk::Room| {
                        let h = Arc::clone(&h);
                        async move {
                            if let matrix_sdk::ruma::events::SyncMessageLikeEvent::Original(e) = ev
                            {
                                if let MessageType::Text(t) = e.content.msgtype {
                                    let event = TimelineEvent {
                                        event_id:  e.event_id.to_string(),
                                        room_id:   room.room_id().to_string(),
                                        sender:    e.sender.to_string(),
                                        body:      t.body,
                                        timestamp: e
                                            .origin_server_ts
                                            .as_secs()
                                            .into(),
                                        msg_type:  "m.text".to_owned(),
                                    };
                                    if let Ok(guard) = h.lock() {
                                        guard.on_message_event(&event);
                                    }
                                }
                            }
                        }
                    },
                );
            }

            loop {
                tokio::select! {
                    _ = stop_rx.changed() => {
                        if *stop_rx.borrow() { break; }
                    }
                    result = client.sync_once(settings.clone()) => {
                        match result {
                            Ok(resp) => {
                                if let Ok(guard) = handler.lock() {
                                    guard.on_sync_token(resp.next_batch.as_str());
                                }
                            }
                            Err(e) => {
                                if let Ok(guard) = handler.lock() {
                                    guard.on_error("sync", &e.to_string());
                                }
                                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
                            }
                        }
                    }
                }
            }
        });
    }

    pub fn stop_sync(&mut self) {
        if let Some(tx) = self.stop_tx.take() {
            let _ = tx.send(true);
        }
    }

    pub fn list_rooms(&self) -> Vec<crate::ffi::RoomInfo> {
        let Some(client) = self.client.clone() else { return Vec::new() };
        let rooms = client.joined_rooms();
        self.rt.block_on(async move {
            let mut result = Vec::new();
            for room in rooms {
                let name = room
                    .display_name()
                    .await
                    .map(|n| n.to_string())
                    .unwrap_or_else(|_| room.room_id().to_string());
                result.push(crate::ffi::RoomInfo {
                    id:           room.room_id().to_string(),
                    name,
                    topic:        room.topic().unwrap_or_default(),
                    unread_count: room.unread_notification_counts().notification_count,
                    is_direct:    room.is_direct().await.unwrap_or(false),
                });
            }
            result
        })
    }

    pub fn send_message(&mut self, room_id: &str, body: &str) -> crate::ffi::OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id)  => id,
            Err(e)  => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };
        let content = RoomMessageEventContent::text_plain(body);
        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    pub fn room_messages(&self, _room_id: &str, _limit: u64) -> Vec<crate::ffi::TimelineEvent> {
        // Will be replaced by a Timeline-backed implementation once the
        // sliding-sync rewrite lands. Until then, history loading is a no-op.
        Vec::new()
    }

    /// Revoke OAuth tokens at the MAS, drop the in-memory client and stop
    /// the sync loop, then remove the local SQLite store so the next login
    /// starts from a clean state. The caller (C++ SessionStore) is also
    /// expected to delete its persisted session.json.
    pub fn logout(&mut self) -> crate::ffi::OpResult {
        // Stop sync first so no background tasks touch the Client while we
        // tear it down.
        self.stop_sync();

        // Cancel any in-flight oauth flow.
        if let Some(flow) = self.oauth_flow.take() {
            oauth::cancel(&flow);
        }

        let Some(client) = self.client.take() else {
            // Already logged out; clean the store anyway in case a previous
            // logout left it half-populated.
            let _ = std::fs::remove_dir_all(data_dir());
            return ok("");
        };

        let revoke = self.rt.block_on(async move {
            client.oauth().logout().await
        });

        // Always remove the local store. Even if the server-side revoke
        // failed, the local keys are no longer useful with this account.
        let _ = std::fs::remove_dir_all(data_dir());

        match revoke {
            Ok(_)  => ok(""),
            Err(e) => err(format!("oauth logout failed (local store cleared): {e}")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn client_ffi_new_starts_empty() {
        let c = ClientFfi::new();
        assert!(c.client.is_none());
        assert!(c.stop_tx.is_none());
        assert!(c.oauth_flow.is_none());
    }

    #[test]
    fn op_result_ok_sets_fields() {
        let r = ok("success");
        assert!(r.ok);
        assert_eq!(r.message, "success");
    }

    #[test]
    fn op_result_err_sets_fields() {
        let r = err("failure");
        assert!(!r.ok);
        assert_eq!(r.message, "failure");
    }

    #[test]
    fn oauth_err_has_no_urls() {
        let r = oauth_err("bad server");
        assert!(!r.ok);
        assert_eq!(r.message, "bad server");
        assert!(r.auth_url.is_empty());
        assert!(r.redirect_uri.is_empty());
    }

    #[test]
    fn export_session_is_empty_when_not_logged_in() {
        let c = ClientFfi::new();
        assert!(c.export_session().is_empty());
    }

    #[test]
    fn list_rooms_is_empty_when_not_logged_in() {
        let c = ClientFfi::new();
        assert!(c.list_rooms().is_empty());
    }

    #[test]
    fn room_messages_is_empty() {
        let c = ClientFfi::new();
        assert!(c.room_messages("!x:example.com", 10).is_empty());
    }

    #[test]
    fn send_message_fails_when_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.send_message("!room:example.com", "hello");
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }

    #[test]
    fn restore_session_fails_on_invalid_json() {
        let mut c = ClientFfi::new();
        let r = c.restore_session("not json {{{");
        assert!(!r.ok);
        assert!(r.message.contains("parse session JSON"));
    }

    #[test]
    fn oauth_cancel_is_noop_without_flow() {
        let mut c = ClientFfi::new();
        c.oauth_cancel(); // must not panic
    }

    #[test]
    fn stop_sync_is_noop_without_start() {
        let mut c = ClientFfi::new();
        c.stop_sync(); // must not panic
    }

    #[test]
    #[cfg(all(unix, not(target_os = "macos")))]
    fn data_dir_ends_with_expected_suffix() {
        let d = data_dir();
        let s = d.to_string_lossy();
        assert!(
            s.ends_with("tesseract/matrix-store"),
            "unexpected data_dir: {s}"
        );
    }
}
