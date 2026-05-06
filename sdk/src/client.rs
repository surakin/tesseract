use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
};

use anyhow::Context as _;
use cxx::UniquePtr;
use matrix_sdk::{
    authentication::oauth::UserSession as OAuthSession,
    config::SyncSettings,
    ruma::events::room::message::{MessageType, RoomMessageEventContent},
    Client, SessionChange,
};
use tokio::runtime::Runtime;
use tokio::sync::watch;

use crate::ffi::{EventHandlerBridge, OAuthBegin, OpResult, RoomInfo, TimelineEvent};
use crate::oauth;

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

pub struct ClientFfi {
    rt:           Runtime,
    client:       Option<Client>,
    stop_tx:      Option<watch::Sender<bool>>,
    oauth_flow:   Option<oauth::PendingFlow>,
}

impl ClientFfi {
    pub fn new() -> Self {
        tracing_subscriber::fmt()
            .with_env_filter(
                tracing_subscriber::EnvFilter::from_default_env()
                    .add_directive("matrix_sdk=info".parse().unwrap()),
            )
            .init();

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
        let session: OAuthSession = match serde_json::from_str(session_json) {
            Ok(s)  => s,
            Err(e) => return err(format!("parse session JSON: {e}")),
        };

        let homeserver = session.user.meta.homeserver().to_owned();
        let path       = data_dir();

        let result = self.rt.block_on(async move {
            let client = Client::builder()
                .homeserver_url(homeserver)
                .sqlite_store(&path, None)
                .build()
                .await
                .context("build client")?;

            client
                .oauth()
                .restore_session(session)
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
        let Some(session) = client.oauth().user_session() else { return String::new() };
        serde_json::to_string(&session).unwrap_or_default()
    }

    pub fn start_sync(
        &mut self,
        handler: UniquePtr<EventHandlerBridge>,
    ) {
        let Some(client) = self.client.clone() else { return };

        let (stop_tx, mut stop_rx) = watch::channel(false);
        self.stop_tx = Some(stop_tx);

        // The handler is not Send, so we wrap it in a Mutex and only call it
        // from the single sync task.
        let handler = Arc::new(Mutex::new(handler));

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
                            let Some(session) = client_clone.oauth().user_session() else {
                                continue;
                            };
                            let Ok(json) = serde_json::to_string(&session) else { continue };
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
        if let Some(tx) = s